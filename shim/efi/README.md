# Making the shim an EFI application: the one thing that blocks it

`docs/STATUS.md` ranks this as the first thing to fix, because it is upstream of
every other unknown: until the kernel starts, no driver can be tried.

This file records what was established while attempting it, so the next person does
not rediscover it. **The blocker is not the file I/O, the device tree, or finding
the payload — it is one `msr` that cannot be executed where the code currently
lives.**

## What is already done

`loader.c` used to do two jobs at once:

1. find the payload — read `/chosen/linux,initrd-start` from the device tree
2. load and jump — validate the header, copy segments, place the ADT and
   `boot_args`, enter the kernel with `x0` = `boot_args`

Only (1) is specific to the ABL `boot.img` path. They are now separate:

```c
const char *loader_boot(uintptr_t dtb);                    /* reads the initrd, then: */
const char *loader_boot_payload(uintptr_t dtb,
    uintptr_t payload_pa, uint64_t payload_len);           /* caller supplies it */
```

`loader_boot()` behaves exactly as before; it is now a wrapper. An EFI application
can get the payload from a file on the ESP and call `loader_boot_payload()` with
that address, reusing the reviewed segment-copy, ADT placement and `boot_args`
code unchanged.

Verified: the shim rebuilds, and both symbols are in `shim/out/loader.o`
(`loader_boot_payload` at `0x0`, `loader_boot` at `0x480`).

## The blocker

`loader_boot_payload()` writes memory in aligned 8-byte words and ends with

```c
__asm__ volatile("dsb sy; ic iallu; dsb sy; isb" ::: "memory");
((xnu_entry_t)entry_pa)(ba_pa);
```

and its comment says *"Everything was written with the MMU and caches off"*. That
is not an incidental detail — it is a requirement, and it is why the copies are
8-byte-word loops: **with the MMU off, all memory is Device memory**, where
unaligned access faults and caching does not apply.

An EFI application runs with the **MMU and caches on**. That is part of the UEFI
contract; firmware would not be able to run its own code otherwise. So:

- an EFI application cannot call `loader_boot_payload()` as it stands;
- and it cannot simply switch the MMU off, because the instruction after
  `msr sctlr_el1, x0` would be fetched through the translation it just removed.

Something has to be executing from memory that is valid *both* before and after the
switch. That is the trampoline, and it is the entire problem.

## What the trampoline needs

The shim already builds exactly the page table shape this requires. `mmu.c` sets up
a 36-bit VA space with 16K granules, `T0SZ=28`, level-2 walk, **identity mapped
(VA == PA)**, with the default mapping Device-nGnRnE. An identity map is precisely
what makes the switch survivable: after clearing `SCTLR_EL1.M`, a virtual address
still resolves to the same physical memory, so code keeps running.

The pieces:

1. **Build the shim's identity map before the switch** — reuse `mmu_init()` from
   `mmu.c`. Its default attribute is Device-nGnRnE, which is also what the payload
   copier assumes, so no extra work is needed to satisfy both sides.
2. **Copy a few instructions to a known physical address** in that identity-mapped
   region, and jump to their *physical* address. They must not be in the EFI image,
   because the EFI image's mapping is the firmware's, not ours.
3. In that copied code: `dsb sy`, `msr sctlr_el1, xzr` (MMU, D-cache, I-cache off),
   `isb`, then `br` to `loader_boot_payload`'s physical address with `x0` = dtb,
   `x1` = payload, `x2` = length.
4. `loader_boot_payload` must itself be linked so its entry is at a known physical
   address, or its address must be passed in — it lives in the EFI image, whose
   load address the firmware chose.

Step 4 is the awkward one. The clean answer is to link the loader into the same
blob as the trampoline rather than into the EFI PE — i.e. build the shim as **two**
pieces: a small EFI application that does file I/O and `ExitBootServices`, and the
existing shim payload, which the EFI application loads next to itself and enters.

## It builds now

`./shim/build.sh` produces **two** outputs from the same sources:

| output | how it is used |
|---|---|
| `out/Image` | the raw payload for `mkbootimg` + `fastboot boot` -- unchanged, still the supported route |
| `out/shim.efi` | a PE32+ AArch64 EFI application, loadable by firmware and by the `sweet-uefi` menu |

The EFI target is built **before** the boot.img step, because `mkbootimg` fails on
some hosts (Debian's wrapper wants the AOSP `gki` module) and `set -e` would then
skip it. The two outputs are independent.

`shim/efi_main.c` is the entry point; `shim/efi_app.h` declares the little UEFI it
needs with every field offset pinned by `_Static_assert` against the values the
`sweet-uefi` project verified against the specification; `shim/efi_stubs.c`
supplies `_head`/`_end`, which a PE link has no equivalent of.

`tools/test-efi-image.sh` reads the PE headers and asserts what decides whether
firmware will load it: `MZ`, the PE signature, machine `0xAA64`, PE32+ magic
`0x20B`, subsystem 10 (`EFI_APPLICATION`), a non-zero entry RVA inside the image,
and 4K alignment of both the image base and `SizeOfImage`. Twelve checks.

What it still does not do: read the payload from a file, or obtain the device tree.
`efi_main()` reports both and returns. That is deliberate -- a launcher that claims
to boot and then silently returns would be worse than one that names what it is
missing.

## Why the jump itself has not been run

Every step above is testable only on hardware, and a mistake in any of them is a
silent hang with no output: the serial console is the shim's own framebuffer code,
which is not running yet at that point. Writing it, compiling it, and reporting
"the EFI wrapper is done" would have been the exact failure this project keeps
warning about — a green tick on a path nobody has executed.

The preparatory work that *is* verifiable has been done. The trampoline has not,
and this file says so rather than implying otherwise.

## What to check first when it is written

- **How does the DTB arrive?** Today the shim receives it in `x0` from the
  bootloader (`shim_main(uint64_t dtb)`, `shim.c:383`). An EFI application gets no
  such argument, so this has to be answered first, because it decides the shape of
  the whole application. I looked for a published configuration table in
  `edk2-msm8998` and **did not find one** — no `EFI_*DTB*` GUID appears in that
  tree. The candidates are therefore: read the `dtb` partition, embed the blob at
  build time, or pass it in from whatever loads this application. **Do not assume a
  configuration table exists**; verify against the firmware actually on the phone.
- Where does the firmware load the PE, and does `mmu_init()`'s identity map cover
  it? If not, the trampoline and the loader are in different 32 MB blocks and both
  need mapping.
- After `ExitBootServices`, is the framebuffer still where the ADT says? The EFI
  application got its framebuffer from GOP, which may not be the same address the
  shim's `devcfg.h` expects for the splash.
- Allocate the payload buffer with `AllocateMaxAddress` below `0xa0000000` (chiron's
  usable low bank starts there) or above 4 GiB, or `loader_boot_payload`'s overlap
  check will reject it — which is the check working, not a bug.
