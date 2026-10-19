#!/bin/bash
# Build the shim and wrap it in a boot.img v2.
#
#   ./build.sh                 # sweet: kernel-only build (no payload), needs ../dtb/
#   ./build.sh payload.bin     # sweet: boot.img with the xnu payload as ramdisk
#   ./build.sh --device chiron [payload.bin]
#
# --device NAME (default sweet) regenerates devcfg.h from tools/devices.py and
# picks the device tree that mkbootimg has to embed:
#   sweet   uses ../dtb/boot_unpacked/dtb          (unpacked from the phone)
#   chiron  uses $DTB, else Platforms/.../chiron   from edk2-msm8998, else none
# The device tree in the boot image is what the bootloader passes to the shim
# in x0; without one the shim still loads the payload (the ADT carries the
# kernel's hardware description) but cannot find /chosen/linux,initrd-start.
set -euo pipefail
cd "$(dirname "$0")"

DEVICE=sweet
PAYLOAD=
while [ $# -gt 0 ]; do
	case "$1" in
	--device) DEVICE="${2:?--device needs a name}"; shift 2 ;;
	--device=*) DEVICE="${1#--device=}"; shift ;;
	*) PAYLOAD="$1"; shift ;;
	esac
done

# devcfg.h: per-SoC addresses for this build (tools/devices.py documents them).
python3 ../tools/gen_devcfg.py "$DEVICE" > devcfg.h
grep -q "DCFG_DEVICE" devcfg.h || { echo "error: devcfg.h generation failed" >&2; exit 1; }

CC=${CROSS_COMPILE:-aarch64-linux-gnu-}gcc
OBJCOPY=${CROSS_COMPILE:-aarch64-linux-gnu-}objcopy
READELF=${CROSS_COMPILE:-aarch64-linux-gnu-}readelf
NM=${CROSS_COMPILE:-aarch64-linux-gnu-}nm
CFLAGS="-O2 -ffreestanding -nostdlib -fno-pic -mgeneral-regs-only -mstrict-align -fno-stack-protector -Wall -Wextra"
mkdir -p out
for f in start.S shim.c console.c mmu.c fdt.c loader.c ufs.c; do
	$CC $CFLAGS -c $f -o out/${f%.*}.o
done
# The image runs wherever the bootloader puts it with no relocation pass: only
# the header's image_size (start.o, linked at 0) may be an absolute address.
abs=$($READELF -r out/shim.o out/console.o out/mmu.o out/fdt.o out/loader.o out/ufs.o | grep -c ABS64 || true)
[ "$abs" = 0 ] || { echo "error: $abs absolute relocations in C objects" >&2; exit 1; }
$CC $CFLAGS -static -T shim.ld -Wl,--build-id=none -Wl,--no-warn-rwx-segments -Wl,-N \
	out/start.o out/shim.o out/console.o out/mmu.o out/fdt.o out/loader.o out/ufs.o -o out/shim.elf
$OBJCOPY -O binary out/shim.elf out/Image
# The bootloader copies the file; pad to image_size so .bss/stacks exist as zeroed bytes.
size=$($NM out/shim.elf | awk '$3=="_end"{print strtonum("0x"$1)}')
truncate -s "$size" out/Image

# Device tree to embed (optional; see the header comment).
DTB_ARGS=()
case "$DEVICE" in
sweet)
	if [ -f ../dtb/boot_unpacked/dtb ]; then
		DTB_ARGS=(--dtb ../dtb/boot_unpacked/dtb)
	elif [ -n "${DTB:-}" ]; then
		DTB_ARGS=(--dtb "$DTB")
	else
		echo "note: no ../dtb/boot_unpacked/dtb, building a boot.img without one" >&2
	fi
	;;
*)
	if [ -n "${DTB:-}" ]; then
		DTB_ARGS=(--dtb "$DTB")
	elif [ -f "../dtb/${DEVICE}.dtb" ]; then
		DTB_ARGS=(--dtb "../dtb/${DEVICE}.dtb")
	elif [ -f "../dtb/Android-${DEVICE}.dtb" ]; then
		DTB_ARGS=(--dtb "../dtb/Android-${DEVICE}.dtb")
	else
		echo "note: set DTB=/path/to/Android-${DEVICE}.dtb (from edk2-msm8998) to embed one" >&2
	fi
	;;
esac

# Ramdisk: an xnu payload (tools/mkpayload.py) if given, else an empty newc
# cpio (bootloaders dislike a zero-size ramdisk) -> the shim's diagnostics mode.
if [ -n "$PAYLOAD" ]; then
	cp "$PAYLOAD" out/ramdisk.bin
else
	(cd out && rm -rf rd && mkdir rd && cd rd && find . | cpio -o -H newc --quiet | gzip -9 > ../ramdisk.bin)
fi

# mkbootimg: Debian/Ubuntu ship a wrapper that needs the AOSP python package
# ("No module named 'gki'").  Try the usual names and keep the first that runs;
# MKBOOTIMG=/path/to/mkbootimg.py overrides (edk2-msm8998 has a plain copy).
MKBOOTIMG_BIN=
for cand in "${MKBOOTIMG:-}" mkbootimg mkbootimg.py; do
	[ -n "$cand" ] || continue
	command -v "$cand" >/dev/null 2>&1 || continue
	"$cand" --help >/dev/null 2>&1 && { MKBOOTIMG_BIN=$(command -v "$cand"); break; }
done
if [ -z "$MKBOOTIMG_BIN" ]; then
	echo "error: no working mkbootimg (Debian's needs the AOSP 'gki' module)." >&2
	echo "       Set MKBOOTIMG=/path/to/mkbootimg.py, e.g. from edk2-msm8998." >&2
	echo "       out/Image and out/ramdisk.bin are built and usable as they are." >&2
	exit 1
fi

# sweet mirrors the stock PixelOS header; the msm8998 UEFI/ABL path only needs
# the payload (nothing flashes these images: fastboot boot / UEFI loads them).
"$MKBOOTIMG_BIN" --header_version 2 --kernel out/Image --ramdisk out/ramdisk.bin \
	"${DTB_ARGS[@]}" --base 0 --kernel_offset 0x8000 --ramdisk_offset 0x1000000 \
	--tags_offset 0x100 --dtb_offset 0x1f00000 --pagesize 4096 \
	--os_version 17.0.0 --os_patch_level 2035-06 \
	--cmdline "console=null" -o "out/shim-boot-$DEVICE.img"
cp "out/shim-boot-$DEVICE.img" out/shim-boot.img
# Host render of the console layout (device independent).
gcc -O1 -Wall -o out/host_test console.c host_test.c && ./out/host_test
ls -l out/Image out/shim-boot.img
