#!/usr/bin/env python3
"""test-usb-soc.py -- check sweet_usb.c's per-SoC table against the sources.

Why this test exists. The USB table replaced constants that were hardcoded for
SM7150. A wrong value there does not fail to compile: it reads a clock register
that belongs to a different branch, or maps a DWC3 that is not there, and the
symptom is a hang or a silent zero on hardware. The same class of mistake as the
touch driver's STOP bit, which a host test caught.

The expectations below are NOT copied from the driver. Each is transcribed from
the file cited next to it, and the driver is parsed as text, so the two
derivations are independent. If someone edits one and not the other, this fails.

Usage: tools/test-usb-soc.py bsd/dev/sweet_usb.c
"""
import re
import sys

# ---------------------------------------------------------------------------
# Independently derived expectations.
#
# msm-clocks-hwio-8998.h is include/dt-bindings/clock/msm-clocks-hwio-8998.h in
# LineageOS/android_kernel_xiaomi_msm8998; line numbers are from that file.
# ---------------------------------------------------------------------------
EXPECT = {
    "MSM8998": {
        "gcc_pa":      0x00100000,   # msm8998.dtsi:938-941 clock_gcc@100000
        "gcc_size":    0x000B0000,   # msm8998.dtsi:940 -- reg = <0x100000 0xb0000>
                                     # NOT 0x90000: the USB30 GDSC at 0x10F004
                                     # would fall outside that, and the first
                                     # power-domain read would leave the mapping
        # msm-gdsc-8998.dtsi:16-20 puts the USB30 GDSC at absolute 0x10f004,
        # and gdsc.c:485 ioremaps that address as-is. The GCC block starts at
        # 0x100000, so the offset inside it is 0x10F004 - 0x100000 = 0xF004 --
        # the same value SM7150 uses. Not a copy-paste: the register really is
        # at the same place in both parts.
        "gdscr_off":   0x0000F004,
        "dwc3_pa":     0x0A800000,   # msm8998.dtsi:1934-1936 dwc3@a800000
        "dwc3_size":   0x0000D000,
        "qusb2_pa":    0x0C012000,   # msm8998.dtsi:1981-1983 qusb@c012000
        "qusb2_size":  0x00000400,
        "clk": {
            "master":      0x0F008,  # hwio-8998.h:205 GCC_USB30_MASTER_CBCR
            "sleep":       0x0F00C,  # hwio-8998.h:206 GCC_USB30_SLEEP_CBCR
            "utmi":        0x0F010,  # hwio-8998.h:207 GCC_USB30_MOCK_UTMI_CBCR
            "cfg-noc-axi": 0x05018,  # hwio-8998.h:163 GCC_CFG_NOC_USB3_AXI_CBCR
            "aggre-axi":   0x82024,  # hwio-8998.h:125 GCC_AGGRE1_USB3_AXI_CBCR
            "phy-aux":     0x50000,  # hwio-8998.h:209 GCC_USB3_PHY_AUX_CBCR
            "clkref":      0x88008,  # hwio-8998.h:119 GCC_USB3_CLKREF_EN
        },
    },
    # The SM7150 values are what the driver already used before this table
    # existed; they are carried over unchanged so the sweet path cannot regress.
    "SM7150": {
        "gcc_pa":      0x00100000,
        "gcc_size":    0x00090000,
        "gdscr_off":   0x0F004,
        "dwc3_pa":     0x0A600000,
        "dwc3_size":   0x0000D000,
        "qusb2_pa":    0x088E2000,
        "qusb2_size":  0x00000400,
        "clk": {
            "master":      0x0F010,
            "sleep":       0x0F014,
            "utmi":        0x0F018,
            "cfg-noc-axi": 0x0502C,
            "aggre-axi":   0x8201C,
            "ahb2phy":     0x6A004,
            "clkref":      0x8C010,
        },
    },
}

# Each SoC's registers must lie inside ITS OWN mapped GCC window. The two windows
# differ: SM7150 is 0x90000, MSM8998 is 0xB0000 (msm8998.dtsi:940). Assuming one
# value for both is exactly the bug this check was written after finding -- the
# USB30 GDSC at 0x10F004 would have been read 0x6F004 bytes past the end of a
# 0x90000 mapping.
GCC_WINDOW = {
    "MSM8998": 0x000B0000,
    "SM7150":  0x00090000,
}


def parse_tables(text):
    """-> {soc_name: {field: int, 'clk': {name: int}}} from the C source."""
    out = {}
    # each table starts at "static const struct usb_soc usb_soc_<x> = {"
    for m in re.finditer(r"struct usb_soc usb_soc_(\w+)\s*=\s*\{", text):
        start = m.end()
        # find the matching closing brace of the initialiser
        depth = 1
        i = start
        while i < len(text) and depth:
            if text[i] == "{":
                depth += 1
            elif text[i] == "}":
                depth -= 1
            i += 1
        body = text[start:i - 1]
        soc = re.search(r'\.soc\s*=\s*"([^"]+)"', body)
        if not soc:
            continue
        entry = {}
        for field in ("gcc_pa", "gcc_size", "gdscr_off", "dwc3_pa",
                      "dwc3_size", "qusb2_pa", "qusb2_size"):
            fm = re.search(r"\.%s\s*=\s*(0x[0-9A-Fa-f]+)" % field, body)
            if fm:
                entry[field] = int(fm.group(1), 16)
        clocks = {}
        for cm in re.finditer(r'\{\s*"([\w-]+)"\s*,\s*(0x[0-9A-Fa-f]+)\s*\}',
                              body):
            clocks[cm.group(1)] = int(cm.group(2), 16)
        entry["clk"] = clocks
        out[soc.group(1)] = entry
    return out


def main(argv):
    path = argv[0] if argv else "bsd/dev/sweet_usb.c"
    with open(path, encoding="utf-8") as f:
        text = f.read()

    tables = parse_tables(text)
    if not tables:
        print("FAIL: no usb_soc tables found in %s" % path)
        return 1

    checks = 0
    fails = 0

    def check(cond, msg):
        nonlocal checks, fails
        checks += 1
        if not cond:
            fails += 1
            print("  FAIL  " + msg)

    print("USB SoC table: %d table(s) found in %s" % (len(tables), path))
    check(set(tables) == set(EXPECT),
          "table names are %s, expected %s"
          % (sorted(tables), sorted(EXPECT)))

    for soc, want in EXPECT.items():
        got = tables.get(soc)
        if got is None:
            check(False, "%s table missing" % soc)
            continue
        print("  %s" % soc)

        for field in ("gcc_pa", "gcc_size", "gdscr_off", "dwc3_pa",
                      "dwc3_size", "qusb2_pa", "qusb2_size"):
            if field not in got:
                check(False, "%s.%s not set" % (soc, field))
                continue
            check(got[field] == want[field],
                  "%s.%s is 0x%X, source says 0x%X"
                  % (soc, field, got[field], want[field]))
            print("    %-11s 0x%08X" % (field, got[field]))

        # clock list: same names, same offsets, same count
        check(set(got["clk"]) == set(want["clk"]),
              "%s clock names are %s, expected %s"
              % (soc, sorted(got["clk"]), sorted(want["clk"])))
        for name, off in want["clk"].items():
            if name not in got["clk"]:
                continue
            check(got["clk"][name] == off,
                  "%s clock %r is 0x%X, source says 0x%X"
                  % (soc, name, got["clk"][name], off))

        # The table must fill exactly USB_CBCR_SLOTS entries or the probe loop
        # reads uninitialised slots.
        slots = re.search(r"#define USB_CBCR_SLOTS\s+(\d+)", text)
        if slots:
            check(len(got["clk"]) == int(slots.group(1)),
                  "%s has %d clock entries, USB_CBCR_SLOTS is %s"
                  % (soc, len(got["clk"]), slots.group(1)))

        # Every register offset must be inside THIS SoC's declared GCC window.
        window = GCC_WINDOW[soc]
        check(got["gcc_size"] == window,
              "%s declares gcc_size 0x%X; its registers need 0x%X"
              % (soc, got["gcc_size"], window))
        check(got["gdscr_off"] < got["gcc_size"],
              "%s.gdscr_off 0x%X is outside its own 0x%X GCC mapping"
              % (soc, got["gdscr_off"], got["gcc_size"]))
        for name, off in got["clk"].items():
            check(off < got["gcc_size"],
                  "%s clock %r 0x%X is outside its own 0x%X GCC mapping"
                  % (soc, name, off, got["gcc_size"]))

        # The two SoCs must not share a DWC3 address; sharing one would mean a
        # copy-paste error rather than a real configuration.
    if "MSM8998" in tables and "SM7150" in tables:
        check(tables["MSM8998"]["dwc3_pa"] != tables["SM7150"]["dwc3_pa"],
              "both SoCs use the same DWC3 address -- suspicious")
        # Note: the GDSC offset IS the same on both, by design -- see the
        # comment in the expectation table. Only the block size and the clock
        # branch offsets differ, and those are checked above.
        shared = (set(tables["MSM8998"]["clk"]) &
                  set(tables["SM7150"]["clk"]))
        differing = [n for n in shared
                     if tables["MSM8998"]["clk"][n] != tables["SM7150"]["clk"][n]]
        check(len(differing) >= 4,
              "only %d clock offsets differ between the SoCs; the tables look "
              "copied rather than derived" % len(differing))

    print()
    print("%d checks, %d failure(s)" % (checks, fails))
    if fails == 0:
        print("PASS")
    return 0 if fails == 0 else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
