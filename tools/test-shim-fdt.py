#!/usr/bin/env python3
"""Host test for the shim's device-tree reader (shim/fdt.c).

The shim gets the payload location and, since the chiron port, the panel
memory layout from /chosen of the bootloader's FDT:

    linux,initrd-start / linux,initrd-end   where the payload is
    ios-sweet,fb-base / -width / -height / -stride   override the panel
                                                    address from devcfg.h

This builds a real DTB with dtc, compiles shim/fdt.c for the host and checks
that each property is read back (and that a missing one leaves the default
alone, which is what loader.c relies on).

    python3 tools/test-shim-fdt.py
"""
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
SHIM = os.path.join(HERE, "..", "shim")

DTS = """/dts-v1/;

/ {
	#address-cells = <2>;
	#size-cells = <2>;
	model = "test";
	compatible = "test,board";

	chosen {
		linux,initrd-start = <0x43400000>;
		linux,initrd-end = <0x4364f2d2>;
		ios-sweet,fb-base = <0x9d400000>;
		ios-sweet,fb-width = <1080>;
		ios-sweet,fb-height = <2160>;
		ios-sweet,fb-stride = <4320>;
	};

	memory@40000000 {
		device_type = "memory";
		reg = <0x0 0x40000000 0x0 0xc0000000>;
	};
};
"""

MAIN = r"""
#include <stdint.h>
#include <stdio.h>
#include "fdt.h"

static int failures;

static void want_u64(uintptr_t dtb, const char *prop, uint64_t expect)
{
	uint64_t v = 0xdeadbeefdeadbeefULL;
	int got = fdt_chosen_u64(dtb, prop, &v);

	if (!got) {
		printf("  %-20s MISSING (expected %#llx)\n", prop, (unsigned long long)expect);
		failures++;
		return;
	}
	printf("  %-20s %#llx %s\n", prop, (unsigned long long)v, v == expect ? "ok" : "WRONG");
	if (v != expect) {
		failures++;
	}
}

static void want_default(uintptr_t dtb, const char *prop, uint64_t def)
{
	uint64_t v = def;
	int got = fdt_chosen_u64(dtb, prop, &v);

	if (got) {
		printf("  %-20s unexpectedly present (%#llx)\n", prop, (unsigned long long)v);
		failures++;
		return;
	}
	printf("  %-20s absent, default %#llx kept\n", prop, (unsigned long long)v);
	if (v != def) {
		failures++;
	}
}

int main(int argc, char **argv)
{
	FILE *f;
	static char buf[1 << 16];
	size_t n;

	(void)argc;
	f = fopen(argv[1], "rb");
	n = fread(buf, 1, sizeof(buf), f);
	fclose(f);

	printf("dtb %s (%zu bytes)\n", argv[1], n);
	want_u64((uintptr_t)buf, "linux,initrd-start", 0x43400000);
	want_u64((uintptr_t)buf, "linux,initrd-end", 0x4364f2d2);
	want_u64((uintptr_t)buf, "ios-sweet,fb-base", 0x9d400000);
	want_u64((uintptr_t)buf, "ios-sweet,fb-width", 1080);
	want_u64((uintptr_t)buf, "ios-sweet,fb-height", 2160);
	want_u64((uintptr_t)buf, "ios-sweet,fb-stride", 4320);
	/* loader.c keeps the devcfg.h values when a property is missing */
	want_default((uintptr_t)buf, "ios-sweet,fb-absent", 0x9c000000ULL);
	/* a non-FDT buffer must not be read as one */
	{
		uint64_t v = 42;
		if (fdt_chosen_u64((uintptr_t)"not a dtb", "ios-sweet,fb-base", &v)) {
			printf("  garbage buffer accepted as an FDT\n");
			failures++;
		} else {
			printf("  garbage buffer rejected, value untouched (%#llx)\n",
			    (unsigned long long)v);
		}
	}
	printf(failures ? "FAIL (%d)\n" : "PASS\n", failures);
	return failures != 0;
}
"""


def main():
    if not os.path.exists(os.path.join(SHIM, "fdt.c")):
        raise SystemExit("run from the repo root (shim/fdt.c not found)")
    with tempfile.TemporaryDirectory() as tmp:
        dts = os.path.join(tmp, "test.dts")
        dtb = os.path.join(tmp, "test.dtb")
        open(dts, "w").write(DTS)
        subprocess.run(["dtc", "-q", "-I", "dts", "-O", "dtb", "-o", dtb, dts], check=True)
        main_c = os.path.join(tmp, "main.c")
        open(main_c, "w").write(MAIN)
        exe = os.path.join(tmp, "test_fdt")
        subprocess.run(["gcc", "-O1", "-Wall", "-Wextra", "-Werror", "-I", SHIM,
                        main_c, os.path.join(SHIM, "fdt.c"), "-o", exe], check=True)
        return subprocess.run([exe, dtb]).returncode


if __name__ == "__main__":
    sys.exit(main())
