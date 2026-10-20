#!/usr/bin/env python3
"""test-bootimg.py -- check a chiron boot.img is complete and self-consistent.

TWO FORMATS MATTER HERE, and they are not interchangeable:

  header v0 (1632-byte struct)  what chiron's ABL accepts. There is no header_size
                                field and no dtb field; the device tree is APPENDED to
                                the kernel, and ABL scans for it at the end of the
                                kernel blob. This is what `tools/build-v0.sh` emits and
                                what actually boots the phone.

  header v1 (1648-byte struct)  what edk2-msm8998's build.sh nominally asks for. chiron
                                rejects it with `remote: 'BootImage is Incomplete'`
                                because ABL's own accounting uses its 1632-byte struct.

An earlier version of this file hardcoded the v1 layout and then asserted a dtb_addr
from the v2 era, which failed CI on a perfectly good v0 image. A check that pins
yesterday's format trains you to ignore it, so this one reads the header version and
verifies the layout that version defines.

Usage: test-bootimg.py IMAGE [--expect-dtb-addr 0x...]
"""
import struct
import sys
import zlib

OFF_MAGIC          = 0
OFF_KERNEL_SIZE    = 8
OFF_RAMDISK_SIZE   = 16
OFF_SECOND_SIZE    = 24
OFF_PAGE_SIZE      = 36
OFF_HEADER_VERSION = 40
OFF_HEADER_SIZE    = 1644      # v1+
OFF_DTB_SIZE       = 1648      # v2+
OFF_DTB_ADDR       = 1652      # v2+

HEADER_STRUCT = {0: 1632, 1: 1648, 2: 1660}
PAYLOAD_MAGIC = b'SWEETXNU'
DTB_MAGIC = b'\xd0\x0d\xfe\xed'


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

    if len(data) < 64:
        print("  FAIL file is %d bytes" % len(data))
        return 1

    check("magic is ANDROID!", data[OFF_MAGIC:OFF_MAGIC + 8] == b'ANDROID!',
          repr(data[OFF_MAGIC:OFF_MAGIC + 8]))

    hv, = struct.unpack_from('<I', data, OFF_HEADER_VERSION)
    struct_size = HEADER_STRUCT.get(hv)
    check("header_version is one we understand",
          struct_size is not None,
          "v%d%s" % (hv, "" if struct_size else " (unknown)"))
    if struct_size is None:
        return 1

    page, = struct.unpack_from('<I', data, OFF_PAGE_SIZE)
    check("page_size is a power of two", page and (page & (page - 1)) == 0, str(page))
    if not page or (page & (page - 1)):
        page = 2048

    ks, = struct.unpack_from('<I', data, OFF_KERNEL_SIZE)
    rs, = struct.unpack_from('<I', data, OFF_RAMDISK_SIZE)
    ss, = struct.unpack_from('<I', data, OFF_SECOND_SIZE)
    check("kernel_size > 0", ks > 0, "%d bytes" % ks)
    check("ramdisk holds the payload", rs > 1_000_000, "%d bytes" % rs)

    if hv >= 1:
        declared, = struct.unpack_from('<I', data, OFF_HEADER_SIZE)
        check("declared header_size matches the struct",
              declared == struct_size, "%d vs %d" % (declared, struct_size))

    def align(n):
        return ((n + page - 1) // page) * page

    # The header occupies its own page-aligned span; parts follow page aligned.
    k_off = align(struct_size)
    r_off = k_off + align(ks)
    s_off = r_off + align(rs)
    total = s_off + align(ss)

    check("computed total equals file length", total == len(data),
          "%d vs %d" % (total, len(data)))
    check("ramdisk region inside the file", r_off + rs <= len(data),
          "0x%x + %d" % (r_off, rs))

    # The kernel slot: gzip'd shim followed by the appended DTB. This is the layout
    # both v0 and the edk2-style v1 use, since neither has a place for a raw DTB.
    blob = data[k_off:k_off + ks]
    check("kernel slot starts with the gzip magic", blob[:2] == b'\x1f\x8b',
          repr(blob[:2]))

    try:
        dz = zlib.decompressobj(16 + zlib.MAX_WBITS)
        shim = dz.decompress(blob)
        leftover = dz.unused_data
        check("gzip stream decompresses", len(shim) > 1000, "%d bytes" % len(shim))
        check("a DTB is appended after it", len(leftover) > 100000,
              "%d bytes" % len(leftover))
        check("appended blob has the DTB magic", leftover[:4] == DTB_MAGIC,
              repr(leftover[:4]))
        if len(leftover) >= 8:
            ts, = struct.unpack_from('>I', leftover, 4)
            check("DTB total-size field agrees with the appended length",
                  ts == len(leftover), "%d vs %d" % (ts, len(leftover)))
    except zlib.error as e:
        check("gzip stream decompresses", False, str(e))

    check("ramdisk starts with SWEETXNU",
          data[r_off:r_off + 8] == PAYLOAD_MAGIC,
          repr(data[r_off:r_off + 8]))

    # v2 only: a separate dtb field. Worth checking when present, meaningless in v0.
    if hv >= 2:
        ds, = struct.unpack_from('<I', data, OFF_DTB_SIZE)
        check("v2 dtb_size > 0", ds > 100_000, "%d bytes" % ds)
        if expect_dtb_addr is not None:
            da, = struct.unpack_from('<Q', data, OFF_DTB_ADDR)
            check("v2 dtb_addr as expected", da == expect_dtb_addr,
                  "0x%x vs 0x%x" % (da, expect_dtb_addr))
    elif expect_dtb_addr is not None:
        print("  note: --expect-dtb-addr ignored; v%d has no dtb field" % hv)

    width = max(len(d) for d, _, _ in checks)
    for desc, ok, detail in checks:
        print("  %s %-*s %s" % ("ok  " if ok else "FAIL", width, desc, detail))

    print()
    if failures == 0:
        print("%d checks, v%d boot image is complete" % (len(checks), hv))
        return 0
    print("%d checks, %d FAILED" % (len(checks), failures))
    return 1


if __name__ == '__main__':
    sys.exit(main(sys.argv))
