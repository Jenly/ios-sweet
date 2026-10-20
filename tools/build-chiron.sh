#!/bin/bash
# build-chiron.sh -- produce a flashable chiron boot.img from a built kernel.
#
# This is the "basic platform" build: everything between a CI kernel artifact and
# something you can `fastboot boot`. Each step existed already but there was no
# single entry point, so the chain was only reproducible by remembering four
# commands and the path to a device tree.
#
#   ./tools/build-chiron.sh KERNEL
#
# Steps:
#   1. ADT      mkadt.py   --device chiron   (the Apple device tree XNU expects)
#   2. payload  mkpayload.py                 (resolves chained fixups, appends ADT)
#   3. shim     shim/build.sh --device chiron (its own MMU, reads the live FDT,
#                                             loads the payload, enters the kernel)
#   4. boot.img mkbootimg with the chiron DTB
#
# Nothing here is new logic: it is the four steps wired together, with the input
# checks that were previously done in my head.
set -euo pipefail
cd "$(dirname "$0")/.."

DEVICE=chiron
KERNEL=${1:?usage: build-chiron.sh KERNEL [OUTDIR]}
OUT=${2:-out-chiron}

# The device tree mkbootimg must embed. Taken from edk2-msm8998, which is what the
# phone actually runs: the ABL passes whatever DTB the boot image carries, and the
# shim reads /chosen from it.
DTB=${DTB:-/root/src/edk2-msm8998/Platforms/Msm8998Pkg/Device/xiaomi/chiron/DeviceTreeBlob/Android/Android-chiron.dtb}
MKBOOTIMG=${MKBOOTIMG:-/root/src/edk2-msm8998/mkbootimg.py}

die() { echo "error: $*" >&2; exit 1; }

[ -f "$KERNEL" ] || die "no kernel at $KERNEL"
[ -f "$DTB" ] || die "no device tree at $DTB (set DTB=...)"
[ -x "$MKBOOTIMG" ] || [ -f "$MKBOOTIMG" ] || die "no mkbootimg at $MKBOOTIMG"

mkdir -p "$OUT"
# shim/build.sh cd's into shim/ and copies the payload from there, so every path
# handed to it has to be absolute.
OUT=$(cd "$OUT" && pwd)
KERNEL=$(cd "$(dirname "$KERNEL")" && pwd)/$(basename "$KERNEL")

echo "== 1. ADT ($DEVICE) =="
python3 tools/mkadt.py "$OUT/adt.bin" --device "$DEVICE"
ls -l "$OUT/adt.bin"

echo
echo "== 2. payload (kernel + ADT, chained fixups resolved) =="
python3 tools/mkpayload.py "$KERNEL" "$OUT/adt.bin" "$OUT/payload.bin"
ls -l "$OUT/payload.bin"

echo
echo "== 3. shim ($DEVICE) =="
# The shim regenerates devcfg.h from tools/devices.py for the requested device.
MKBOOTIMG="$MKBOOTIMG" DTB="$DTB" ./shim/build.sh --device "$DEVICE" "$OUT/payload.bin"

echo
echo "== 4. boot.img =="
# shim/build.sh already wrapped its own Image; if it did not (no mkbootimg on this
# host), do it here explicitly so the failure names the step.
if [ -f shim/out/shim-boot.img ]; then
	cp shim/out/shim-boot.img "$OUT/shim-boot-$DEVICE.img"
	echo "  $OUT/shim-boot-$DEVICE.img"
	ls -l "$OUT/shim-boot-$DEVICE.img"
else
	die "shim/build.sh did not produce shim-boot.img -- see the mkbootimg message above"
fi

echo
echo "== result =="
echo "  flash with:  fastboot boot $OUT/shim-boot-$DEVICE.img"
echo "  payload:     $OUT/payload.bin ($(stat -c%s "$OUT/payload.bin" 2>/dev/null || stat -f%z "$OUT/payload.bin") bytes)"
