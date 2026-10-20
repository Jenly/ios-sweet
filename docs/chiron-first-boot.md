# What the phone proved, and the three ways forward

Written 2026-10-21, after the first chiron boot that ran real code.

## What is now established by hardware, not by assumption

The shim **runs on the Xiaomi Mi MIX 2**. The panel, photographed, reads:

```
SHIM    chiron milestone 4: xnu loader
INITRD  lo=43900000  hi=4390b230
PAYLOAD lo=43900000  hi=4390b230
PHYS    base=100000000  size=BD8C0000
VIRT    FFFFFE0006000000
ENTRY   FFFFFE000748F488
SEGS    n=0000000A  end=024D0000
FB      base=9D400000  hi=00000000
FB      w=00000438  h=00000870
FB      stride=00000000000010E0
BOOTARG pa=024D4000  top=024D8000
JUMP    00000001014 8F488
XNU     jumping (fbcon clears screen)
```

Every one of those numbers is correct and was checked against an independent source
before the boot:

| what | on screen | expected | source |
|---|---|---|---|
| payload magic + segments | `SEGS n=0000000A` | 10 segments | `struct payload` in `loader.c` |
| physical base | `100000000` | `0x100000000` | chiron `memory@1`, the upper bank |
| size | `BD8C0000` | `0xbd8c0000` | same |
| entry | `FFFFFE000748F488` | matches the payload's `entry_va` | `tools/mkpayload.py` output |
| framebuffer | `9D400000` | `0x9d400000` | `splash_region@9d600000`, `reg = <0x9d400000 0x2400000>` |
| panel | `438 x 870` | 1080 x 2160 at 24bpp | `DCFG_FB_W/H` |
| stride | `10E0` | 4320 | 1080 x 4 |

So: **the boot image format, the load addresses, the framebuffer, the device tree,
the payload header and the jump all work.** That is the entire bring-up chain up to
the kernel entry point, proven on hardware.

Getting there required finding that chiron's ABL accepts **header_version 0** and
rejects v1/v2 with `remote: 'BootImage is Incomplete'`. Its `boot_img_hdr` is the
1632-byte v0 struct, compiled in 2017; a v1 header declares 1648 and its size
accounting never agrees. The device tree must travel **appended to the kernel**
(`.img.gz-dtb`), because v0 has no dtb field and ABL scans for the DTB at the end of
the kernel blob.

## What the phone also revealed: the payload is truncated

```
INITRD  lo=43900000  hi=4390b230      -> 45,616 bytes
```

The payload is 16,649,952 bytes. The shim's `loader_boot()` validates the eight-byte
magic, the version and the ten-entry segment table -- all of which live in the first
~700 bytes -- then copies `file_size` bytes per segment from wherever the header says.
With 45,616 bytes present, everything past that is unrelated RAM. **The shim jumped to
a correct address containing incorrect code.** Silence afterwards is exactly what
that produces.

The shim now computes the region it needs (`max(file_off + file_size)` over all
segments, and the ADT) and compares it with what the bootloader supplied, printing
both before any copy:

```
PAYLEN  need=xxxxxxxx  avail=xxxxxxxx
```

and refuses to jump if the region is short. A refusal prints a reason instead of
producing a mystery. **This is the logging that was asked for, and it now reports its
own numbers rather than needing a photograph to be interpreted.**

## The three ways forward

### Route A -- UEFI, read the payload from a USB stick

`edk2-msm8998` for chiron contains, and this was verified in its build files:

```
XhciPciEmulation + XhciDxe          USB host controller
UsbBusDxe + UsbMassStorageDxe       USB mass storage
DiskIoDxe + PartitionDxe + Fat      block layer, GPT/MBR, FAT32
ShellPkg/Application/Shell          a UEFI shell
AndroidFastbootApp                  fastboot, in firmware
```

So a USB stick is readable by the firmware itself, and there is a shell to type
commands into. This removes the ramdisk transport entirely.

What it needs: the shim already builds as `shim.efi` (PE32+ AArch64, 9,216 bytes,
verified against the PE headers). What it lacks is `efi_main()` finding the payload
and the DTB -- see `shim/efi/README.md` for the four steps and the trampoline, which is
built and disassembly-verified but has never run.

**This is the route with the fewest unknowns now.** The transport is a solved problem
in the firmware; the remaining work is entirely on our side.

### Route B -- find where the 45,616 comes from

The value is ABL's, not ours: the DTB we ship says `linux,initrd-start = 0x43400000`,
`initrd-end = 0x4364f2d2` (2,421,458 bytes), and the phone printed neither. ABL
rewrote both properties based on what it actually loaded.

Possibilities, in order of how cheap they are to distinguish:

1. **The ramdisk field really is capped.** Then no ramdisk larger than ~45 KB can
   travel this way, and route A or C is mandatory.
2. **`fastboot boot` treats the ramdisk differently from `fastboot flash`.** The
   temporary-boot path may reserve less than the flash path. Testing costs one
   `fastboot flash boot` of a v0 image -- destructive to the current boot partition,
   so it needs an explicit decision and a backup first.
3. **ABL has a size it derives from somewhere in the DTB** and 45,616 is a round
   number in its own terms. Unknown.

The new `PAYLEN need=/avail=` line makes each test self-reporting.

### Route C -- put the payload somewhere the kernel can find it itself

The payload does not have to arrive as the ramdisk. XNU can read the UFS root, and
`userdata` is already destined to hold the iOS image. If the shim only had to place a
small stub, the transport would stop mattering.

This is the most invasive of the three and the least explored.

## Recommendation

**Route A.** It is the only one where the missing part is code in this repository and
the transport is already proven in the firmware the phone boots. Route B's cheapest
test is destructive, and Route C assumes the kernel gets far enough to read a
filesystem -- which is precisely the thing currently failing.

The immediate next step is unchanged from `shim/efi/README.md`: make `efi_main()`
locate the payload and the DTB, then call `efi_handoff()`. The trampoline for that
exists and its instruction encoding is verified.
