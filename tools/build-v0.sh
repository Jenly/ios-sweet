#!/bin/bash
# build-v0.sh -- the packaging recipe that actually got past ABL.
#
# The one thing to keep from tonight's session: `fastboot boot` on this phone accepts
# **header_version 0**, and rejects v1/v2 with `remote: 'BootImage is Incomplete'`.
# A v1 header is 1648 bytes with a declared header_size; chiron's ABL is from 2017 and
# was compiled against the 1632-byte v0 struct, so its own size accounting never
# matches a v1 image.
#
# The device tree travels appended to the kernel (the `.img.gz-dtb` arrangement
# edk2-msm8998 uses), because v0 has no dtb field -- and ABL scans for the DTB at the
# end of the kernel blob anyway.
set -euo pipefail
cd "$(dirname "$0")/.."

RAMDISK=${1:-out-chiron/payload-full-cmdline.bin}
OUT=${2:-out-chiron/boot-chiron-v0.img}
MK=/root/src/edk2-msm8998/mkbootimg.py
DTB=/root/src/edk2-msm8998/Platforms/Msm8998Pkg/Device/xiaomi/chiron/DeviceTreeBlob/Android/Android-chiron.dtb

[ -f "$RAMDISK" ] || { echo "no ramdisk at $RAMDISK" >&2; exit 1; }
[ -f shim/out/Image ] || { echo "build the shim first: ./shim/build.sh --device chiron PAYLOAD" >&2; exit 1; }

mkdir -p out-chiron

# kernel slot = gzip(shim) + DTB
gzip -9 -n -c shim/out/Image > out-chiron/shim.img.gz
cat out-chiron/shim.img.gz "$DTB" > out-chiron/shim.img.gz-dtb
echo "  kernel slot $(stat -c%s out-chiron/shim.img.gz) gz + $(stat -c%s "$DTB") dtb"

# header v0, edk2's offsets, edk2's base default
python3 "$MK" \
	--kernel out-chiron/shim.img.gz-dtb \
	--ramdisk "$RAMDISK" \
	--kernel_offset 0x00000000 \
	--ramdisk_offset 0x00000000 \
	--tags_offset 0x00000000 \
	--os_version 17.0.0 --os_patch_level 2035-06 \
	--header_version 0 \
	-o "$OUT"

echo "  built $OUT ($(stat -c%s "$OUT") bytes)"
