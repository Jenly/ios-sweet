#!/bin/bash
# test-efi-image.sh -- assert shim.efi is a loadable AArch64 EFI application.
#
# "It built" is not the same as "firmware can load it". A PE with the wrong machine
# type, or the wrong subsystem, or PE32 instead of PE32+, links without complaint
# and is then rejected by the loader with no useful message. This reads the headers
# and checks the four values that decide whether it loads at all.
#
# Needs the cross/EFI toolchain that produced it; skips cleanly when absent.
set -uo pipefail
cd "$(dirname "$0")/.."

IMG=shim/out/shim.efi

if [ ! -f "$IMG" ]; then
	echo "shim.efi not built (run shim/build.sh with clang and lld-link)" >&2
	exit 1
fi

python3 - "$IMG" <<'PY'
import struct, sys

path = sys.argv[1]
d = open(path, 'rb').read()
fail = 0

def check(desc, got, want):
    global fail
    if got == want:
        print("  ok   %-34s %s" % (desc, got))
    else:
        print("  FAIL %-34s want %s got %s" % (desc, want, got))
        fail += 1

check("DOS magic", d[:2], b'MZ')
pe = struct.unpack_from('<I', d, 0x3c)[0]
check("PE signature", d[pe:pe+4], b'PE\0\0')

machine, nsec = struct.unpack_from('<HH', d, pe + 4)
check("machine is AArch64", hex(machine), "0xaa64")
check("has sections", nsec > 0, True)

opt_size = struct.unpack_from('<H', d, pe + 20)[0]
check("optional header present", opt_size > 0, True)

opt = pe + 24
magic, = struct.unpack_from('<H', d, opt)
check("PE32+ (not PE32)", hex(magic), "0x20b")

entry, = struct.unpack_from('<I', d, opt + 16)
check("entry point RVA non-zero", entry != 0, True)

image_base, = struct.unpack_from('<Q', d, opt + 24)
check("image base 4K aligned", image_base % 0x1000, 0)

size_of_image, = struct.unpack_from('<I', d, opt + 56)
check("SizeOfImage 4K aligned", size_of_image % 0x1000, 0)
check("SizeOfImage plausible", 0x1000 <= size_of_image <= 0x4000000, True)

subsystem, = struct.unpack_from('<H', d, opt + 68)
check("subsystem EFI_APPLICATION", subsystem, 10)

check("entry lies inside the image", entry < size_of_image, True)

print()
if fail == 0:
    print("shim.efi is a loadable AArch64 EFI application")
    sys.exit(0)
print("shim.efi FAILED %d check(s)" % fail)
sys.exit(1)
PY
