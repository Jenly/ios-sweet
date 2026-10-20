# chiron: the handoff is proven; the kernel is the remaining unknown

Written 2026-10-21, after four boots on hardware.

## What is now proven on the phone, not assumed

The panel photograph from the last boot reads:

```
SHIM    chiron milestone 4: xnu loader
INITRD  lo=43400000  hi=443E0F50
PAYLOAD lo=43400000  hi=443E0F50
RAMNODE none found in the device tree
PAYLEN  need=0FE0F50  avail=0FE0F50
PHYS    base=100000000  size=BD8C0000
VIRT    FFFFFE0006000000
ENTRY   FFFFFE0007490488
SEGS    n=0000000A  bad=00000000
FB      base=9D400000
FB      w=00000438  h=00000870
FB      stride=10E0
BOOTARG pa=024D4000  top=024D8000
JUMP    0000000101490488
XNU     jumping (fbcon clears screen)
```

Four separate claims, each now backed by a measurement rather than an inference:

**1. The payload arrives complete.** `avail = 0xFE0F50 = 16,650,064` against
`need = 0xFE0F50`. An earlier boot reported `avail = 45,616`; that was a stale image,
not a transport limit. The boot loader carries the whole thing.

**2. The copy lands.** `bad=00000000` is the result of reading every segment back out of
its destination and comparing it against the source -- 16 samples per segment, 10
segments. This rules out "the destination is not RAM", which was the leading suspicion
because the payload's `phys_base`/`mem_size` come from `tools/devices.py`'s **6 GB**
chiron entry and this handset may be the 4 GB variant.

**3. The entry point is correct.** The payload's `entry_va` is
`0xfffffe0007490488`, which is segment 2 at `+0x1a8488`, file offset `0x48c878`. The
four bytes there are

```
19070014    b +0x1c64
```

a branch over exactly sixteen `nop`s -- which is `LowExceptionVectorBase`'s
`.align 12` padding. That is `_start`: `b EXT(start_first_cpu)` followed by the low
exception vector table. Not mid-function code, not zeroes.

**4. The jump address is right.** `JUMP 0x101490488` is `entry_va + (phys_base -
virt_base)` = `0xfffffe0007490488 + (0x100000000 - 0xfffffe0006000000)`. Arithmetic
checked against the payload header.

So: **everything the shim is responsible for works.** The boot image format, the load
addresses, the framebuffer address, the device tree, the payload header, the segment
copy, the entry point and the jump are all correct and all measured.

`RAMNODE none found` is not a fault: chiron's device tree has no `/memory` node, and it
should not -- the kernel gets its RAM from the **Apple** device tree the shim places
behind it (`dram-base` / `dram-size`, magic `0x09000000`), not from the FDT. That check
was written against the wrong format and its "none found" is the expected answer.

## What this leaves

The kernel is entered correctly and produces nothing. It does not panic on screen, it
does not draw, it does not print. The panel keeps the shim's last line.

Nothing the shim does can narrow this further: the next thing that executes is kernel
code, and the observation after that is silence. Diagnosing it needs one of:

- **a way to see kernel output** -- `debug=0xc serial=3` is in the command line, but
  there is no serial port on this handset, so `serial=` selects something that does not
  physically exist. The framebuffer console is what would print, and it requires the
  kernel to get far enough to initialise it. `sweet_early.c` is the bootstrap console
  that runs before that -- and patch 0090 fixed it to use the real framebuffer address,
  which was sweet's 0x9c000000 until now. If the kernel were reaching early-boot C code,
  that fix would have produced text. It produced none, so the kernel is dying before
  `sweet_early_mark()` is called or before the early console is installed.
- **QEMU** -- this port has a twin (`tools/qemu-sweet.py`) that runs the same kernel
  against a virtual platform and can `--fail-on-panic`. If the same kernel image boots
  under QEMU, the difference is hardware-specific; if it fails identically, the fault is
  in the kernel and reproducible in a debugger.
- **a photo of the panel with a longer wait**. The shim returns control to nothing, so
  the panel is frozen; but a kernel that resets would have gone back to fastboot, and it
  did not.

## Patch 0090, which is still the right fix regardless

Five sites had sweet's framebuffer address compiled in, including the very first store
the kernel makes:

```asm
#if defined(SWEET)
    MOV64   x0, (0x9c000000 + 2368 * 4320)
```

On chiron that is 20 MiB from the real region. Fixed to read `boot_args.Video` via
`genassym.c`-exported offsets, with the panel geometry in `sweet_early.c` no longer a
compile-time constant either. That fix is in the image that produced the photograph
above, so the kernel did not reach the code that draws with it.

## Where the next attempt should look

Not at the shim, and not at the boot image. Both are measured working. The question is
what the kernel does between `b EXT(start_first_cpu)` and the first thing it can print.
The most informative cheap experiment is the QEMU twin, because it turns "silence on
hardware" into a panic message and a PC.
