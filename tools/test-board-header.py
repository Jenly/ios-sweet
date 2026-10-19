#!/usr/bin/env python3
"""Host test for pexpert/pexpert/SweetBoard.h against generated ADTs.

Builds a stub SecureDT* implementation over the bytes that tools/mkadt.py
produced (so the inline helpers in the header see exactly what the kernel
would see), then executes the new logic for every device.  This is the local
stand-in for the CI kernel build: it does not compile XNU, but it does run
the code that decides the presented display size, the panel crop and which
SoC-specific drivers are allowed to touch their registers.

    python3 tools/mkadt.py /tmp/sweet.adt --device sweet
    python3 tools/mkadt.py /tmp/chiron.adt --device chiron
    python3 tools/test-board-header.py /tmp/sweet.adt /tmp/chiron.adt
"""
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import adt  # noqa: E402

STUBS = {
    "pexpert/pexpert.h": """
#pragma once
#include <stdint.h>
int PE_parse_boot_argn(const char *arg_string, void *arg_ptr, int max_arg_size);
""",
    "pexpert/device_tree.h": """
#pragma once
#include <stdint.h>
typedef enum { kSuccess = 0, kError = -1 } DTResult;
typedef struct DTEntryStub { int node; } *DTEntry;
int SecureDTLookupEntry(const DTEntry searchPoint, const char *pathName, DTEntry *foundEntry);
int SecureDTFindEntry(const char *propName, const char *propValue, DTEntry *foundEntry);
int SecureDTGetProperty(DTEntry entry, const char *propertyName,
    void const **propertyValue, unsigned int *propertySize);
""",
    "libkern/libkern.h": """
#pragma once
#include <string.h>
""",
}

TEST = r"""
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pexpert/SweetBoard.h>

/* boot-args, as a kernel would have them */
static const char *g_bootargs[8];
static int g_nbootargs;

int PE_parse_boot_argn(const char *name, void *ptr, int max)
{
	for (int i = 0; i < g_nbootargs; i++) {
		const char *a = g_bootargs[i];
		size_t l = strlen(name);
		if (strncmp(a, name, l) == 0 && a[l] == '=') {
			int v = atoi(a + l + 1);
			memcpy(ptr, &v, sizeof(v) < (size_t)max ? sizeof(v) : (size_t)max);
			return 1;
		}
	}
	return 0;
}

int main(int argc, char **argv)
{
	struct sweet_display_geometry g;
	uint32_t pw = 0, ph = 0;

	if (argc > 1 && strcmp(argv[1], "native") == 0) {
		g_bootargs[g_nbootargs++] = "sweetfb_native=1";
	}
	printf("soc: sm7150=%d msm8998=%d is_sweet=%d is_chiron=%d\n",
	    sweet_board_has_soc(SWEET_BOARD_SM7150),
	    sweet_board_has_soc(SWEET_BOARD_MSM8998),
	    sweet_board_is_sweet(), sweet_board_is_chiron());
	sweet_panel_size(&pw, &ph);
	printf("panel %ux%u\n", pw, ph);
	sweet_display_geometry_for(pw, ph, &g);
	printf("presented %ux%u at %d,%d, %u ppi\n",
	    g.logical_w, g.logical_h, g.off_x, g.off_y, g.dots_per_inch);
	printf("panel shows logical x %d..%d, y %d..%d\n",
	    g.off_x, g.off_x + (int)g.panel_w - 1,
	    g.off_y, g.off_y + (int)g.panel_h - 1);
	return 0;
}
"""

DT_IMPL = r"""
#include <string.h>
#include <pexpert/device_tree.h>
#include "dt_data.h"

int SecureDTLookupEntry(const DTEntry searchPoint, const char *pathName, DTEntry *foundEntry)
{
	(void)searchPoint;                        /* only "/" is looked up here */
	if (strcmp(pathName, "/") != 0) {
		return kError;
	}
	*foundEntry = (DTEntry)(uintptr_t)1;      /* node id 1 is the root */
	return kSuccess;
}

int SecureDTFindEntry(const char *propName, const char *propValue, DTEntry *foundEntry)
{
	if (strcmp(propName, "name") != 0) {
		return kError;
	}
	for (unsigned i = 0; i < dt_num_props; i++) {
		if (strcmp(dt_props[i].name, "name") == 0 &&
		    strcmp((const char *)dt_props[i].value, propValue) == 0) {
			*foundEntry = (DTEntry)(uintptr_t)dt_props[i].node;
			return kSuccess;
		}
	}
	return kError;
}

int SecureDTGetProperty(DTEntry entry, const char *propertyName,
    void const **propertyValue, unsigned int *propertySize)
{
	int node = (int)(uintptr_t)entry;

	for (unsigned i = 0; i < dt_num_props; i++) {
		if (dt_props[i].node == node && strcmp(dt_props[i].name, propertyName) == 0) {
			*propertyValue = dt_props[i].value;
			*propertySize = dt_props[i].size;
			return kSuccess;
		}
	}
	return kError;
}
"""


class IDS:
    def __init__(self):
        self.next = 1

    def take(self):
        v = self.next
        self.next += 1
        return v


def walk(tree, node_id, out, ids):
    for k, flags, v in tree.props:
        if flags:            # skip iBoot placeholders, like a real ADT reader
            continue
        out.append((node_id, k, v))
    for c in tree.children:
        walk(c, ids.take(), out, ids)


def emit_dt_data(tree, path):
    props = []
    ids = IDS()
    walk(tree, ids.take(), props, ids)
    width = max(8, max(len(v) for _n, _k, v in props))
    lines = ["#pragma once",
             "typedef struct { int node; const char *name; const void *value; unsigned size; } DTPropStub;",
             "static const unsigned char dt_values[][%d] = {" % width]
    for _n, _k, v in props:
        data = v + b"\0" * (width - len(v))
        lines.append("  {%s}," % ",".join(str(b) for b in data))
    lines.append("};")
    lines.append("static const DTPropStub dt_props[] = {")
    for i, (n, k, v) in enumerate(props):
        lines.append('  {%d, "%s", dt_values[%d], %d},' % (n, k, i, len(v)))
    lines.append("};")
    lines.append("static const unsigned dt_num_props = %d;" % len(props))
    with open(path, "w") as f:
        f.write("\n".join(lines) + "\n")
    return len(props)


def header_path():
    for cand in (os.path.join(HERE, "..", "xnu", "SweetBoard.h"),
                 os.path.join(HERE, "SweetBoard.h")):
        if os.path.exists(cand):
            return cand
    raise SystemExit("SweetBoard.h not found")


def main(argv):
    if len(argv) < 2:
        raise SystemExit(__doc__)
    ok = True
    with tempfile.TemporaryDirectory() as tmp:
        for name, body in STUBS.items():
            full = os.path.join(tmp, name)
            os.makedirs(os.path.dirname(full), exist_ok=True)
            open(full, "w").write(body)
        os.makedirs(os.path.join(tmp, "pexpert"), exist_ok=True)
        open(os.path.join(tmp, "pexpert", "SweetBoard.h"), "w").write(open(header_path()).read())
        open(os.path.join(tmp, "test_main.c"), "w").write(TEST)
        open(os.path.join(tmp, "dt_stub.c"), "w").write(DT_IMPL)

        for adt_path in argv[1:]:
            tree, _ = adt.parse(open(adt_path, "rb").read())
            nprops = emit_dt_data(tree, os.path.join(tmp, "dt_data.h"))
            exe = os.path.join(tmp, "test_board")
            r = subprocess.run(["gcc", "-std=gnu99", "-Wall", "-Wextra", "-Werror", "-O1",
                                "-I", tmp, os.path.join(tmp, "test_main.c"),
                                os.path.join(tmp, "dt_stub.c"), "-o", exe],
                               capture_output=True, text=True)
            if r.returncode != 0:
                print(r.stdout + r.stderr)
                print("COMPILE FAILED for", adt_path)
                ok = False
                continue
            print("=== %s (%d properties)" % (adt_path, nprops))
            for mode in ([], ["native"]):
                out = subprocess.run([exe] + mode, capture_output=True, text=True)
                print(out.stdout.rstrip() + "   [%s]" % (mode[0] if mode else "default"))
            r = subprocess.run(["g++", "-std=gnu++17", "-Wall", "-Wextra", "-Werror",
                                "-fsyntax-only", "-I", tmp, "-x", "c++",
                                os.path.join(tmp, "test_main.c")],
                               capture_output=True, text=True)
            if r.returncode != 0:
                print(r.stdout + r.stderr)
                print("C++ PARSE FAILED")
                ok = False
            else:
                print("C++ parse (SweetFramebuffer/SweetHID include path): OK")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
