#!/usr/bin/env python3
"""test-bootimg.py -- check a chiron boot.img is complete and self-consistent.

WHY THIS EXISTS. I twice reported this image as broken -- first "no device tree
embedded", then "dtb_size is 0" -- because I read the DTB field at offset 1632. That
is `recovery_dtbo_size`, which is *supposed* to be zero. The real `dtb_size` is at
1648. The image was correct the whole time and my verification was wrong.

A hand-typed offset in a debug one-liner is exactly the kind of check that lies. So
the offsets live here once, with the layout written out, and the tool says what it
verified and what it could not.

Checks:
  * "ANDROID!" magic
  * header_version 2 and header_size == BOOT_IMAGE_HEADER_V2_SIZE (1660)
  * kernel_size > 0
  * ramdisk holds a SWEETXNU payload: magic, version 1, a plausible segment count
  * dtb_size > 0 -- the device tree is embedded, not forgotten
  * dtb_addr is the documented 0x1f00000 for chiron
  * total file length accounts for header + all three parts, page-aligned

Usage: test-bootimg.py IMAGE [--expect-dtb-addr 0x1f00000]
"""
import struct
import sys

# BOOT_IMAGE_HEADER_V2 layout (AOSP bootimg.h). Written out because getting this
# wrong is precisely the mistake this file exists to prevent.
OFF_MAGIC           = 0
OFF_KERNEL_SIZE     = 8
OFF_RAMDISK_SIZE    = 16
OFF_HEADER_VERSION  = 40
OFF_RECOVERY_DTBO   = 1632
OFF_HEADER_SIZE     = 1644
OFF_DTB_SIZE        = 1648
OFF_DTB_ADDR        = 1652
BOOT_IMAGE_HEADER_V2_SIZE = 1660

# tools/mkpayload.py writes this header before the segments.
PAYLOAD_MAGIC = b'SWEETXNU'
PAYLOAD_HDR = struct.Struct('<8sIIQQQQQQQQ')   # magic, version, nsegs, then 8 x u64


def main(argv):
    if len(argv) < 2:
        print("usage: test-bootimg.py IMAGE [--expect-dtb-addr 0x...]", file=sys.stderr)
        return 2

    path = argv[1]
    expect_dtb_addr = None
    if '--expect-dtb-addr' in argv:
        expect_dtb_addr = int(argv[argv.index('--expect-dtb-addr') + 1], 0)

    data = open(path, 'rb').read()
    checks = []
    failures = 0

    def check(desc, ok, detail=""):
        nonlocal failures
        checks.append((desc, ok, detail))
        if not ok:
            failures += 1

    check("magic is ANDROID!",
          data[OFF_MAGIC:OFF_MAGIC + 8] == b'ANDROID!',
          repr(data[OFF_MAGIC:OFF_MAGIC + 8]))

    hv, = struct.unpack_from('<I', data, OFF_HEADER_VERSION)
    check("header_version is 2", hv == 2, str(hv))

    hs, = struct.unpack_from('<I', data, OFF_HEADER_SIZE)
    check("header_size == 1660", hs == BOOT_IMAGE_HEADER_V2_SIZE, str(hs))

    ksize, = struct.unpack_from('<I', data, OFF_KERNEL_SIZE)
    check("kernel_size > 0", ksize > 0, "%d bytes" % ksize)

    rsize, = struct.unpack_from('<I', data, OFF_RAMDISK_SIZE)
    check("ramdisk holds the payload", rsize > 1_000_000, "%d bytes" % rsize)

    rdtbo, = struct.unpack_from('<I', data, OFF_RECOVERY_DTBO)
    check("recovery_dtbo absent (expected for this image)", rdtbo == 0, str(rdtbo))

    # The check I got wrong twice.
    dsize, = struct.unpack_from('<I', data, OFF_DTB_SIZE)
    check("dtb_size > 0 (device tree embedded)", dsize > 100_000, "%d bytes" % dsize)

    daddr, = struct.unpack_from('<Q', data, OFF_DTB_ADDR)
    if expect_dtb_addr is not None:
        check("dtb_addr as expected", daddr == expect_dtb_addr,
              "0x%x vs 0x%x" % (daddr, expect_dtb_addr))
    else:
        check("dtb_addr non-zero", daddr != 0, "0x%x" % daddr)

    # The payload's own header, at the start of the ramdisk region.
    # Header is page-aligned; page size is at offset 36.
    page, = struct.unpack_from('<I', data, 36)
    rd_off = ((hs + page - 1) // page) * page + ((ksize + page - 1) // page) * page
    if rd_off + PAYLOAD_HDR.size <= len(data):
        magic, ver, nsegs = struct.unpack_from('<8sII', data, rd_off)
        check("payload magic at the ramdisk offset", magic == PAYLOAD_MAGIC,
              repr(magic))
        check("payload version is 1", ver == 1, str(ver))
        check("payload has 1..20 segments", 1 <= nsegs <= 20, str(nsegs))
    else:
        check("payload header reachable", False, "ramdisk offset past EOF")

    # EXACT length, not a lower bound. An earlier version used `>=` and a truncated
    # image -- one that had lost its device tree -- passed the check. A boot image is
    # fully described by its header, so its size is a definite number: anything else
    # is truncation or trailing garbage.
    align = lambda n: ((n + page - 1) // page) * page
    expected_len = align(hs) + align(ksize) + align(rsize) + align(dsize)
    check("file length matches header + kernel + ramdisk + dtb",
          len(data) == expected_len,
          "%d vs expected %d" % (len(data), expected_len))

    width = max(len(d) for d, _, _ in checks)
    for desc, ok, detail in checks:
        print("  %s %-*s %s" % ("ok  " if ok else "FAIL", width, desc, detail))

    print()
    if failures == 0:
        print("%d checks, boot image is complete" % len(checks))
        return 0
    print("%d checks, %d FAILED" % (len(checks), failures))
    return 1


if __name__ == '__main__':
    sys.exit(main(sys.argv))
