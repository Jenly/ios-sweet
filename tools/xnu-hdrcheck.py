#!/usr/bin/env python3
"""Catch XNU's header-export trap before CI does.

The problem this solves, learned the hard way from a failed CI run: a header
that merely *exists* in the source tree is invisible to `#include <dir/X.h>`
unless it is also listed in the owning directory's Makefile export/install
lists. The build copies only those into

    build/xnu.obj/EXPORT_HDRS/<component>/<component>/

and that is the `-I` path every translation unit uses. So:

    pexpert/pexpert/SweetBoard.h exists, patch applies fine,
    but `#include <pexpert/SweetBoard.h>` fails with "file not found"
    until `SweetBoard.h` is added to EXPORT_MI_LIST.

tools/xnu-localcc.py compiled against `-I<tree>/pexpert` directly and so could
not see this. This script checks the export lists instead.

    tools/xnu-hdrcheck.py TREE [FILE...]
"""
import argparse
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import importlib.util

spec = importlib.util.spec_from_file_location("lc", os.path.join(HERE, "xnu-localcc.py"))
localcc = importlib.util.module_from_spec(spec)
spec.loader.exec_module(localcc)

# `#include <component/Name.h>` (angle form only: quoted form does not go
# through EXPORT_HDRS)
ANGLE_INC = re.compile(r'^\s*#\s*include\s*<([A-Za-z0-9_]+)/([A-Za-z0-9_./-]+\.h)>', re.M)

# Directories whose Makefile drives export, by the component prefix used in
# includes. A header `<osfmk/kern/x.h>` lives in osfmk/kern/x.h and is governed
# by osfmk/kern/Makefile.
COMPONENTS = ("osfmk", "bsd", "iokit", "libkern", "pexpert", "security", "libsa", "san")

# Makefile variables that make a header visible to `#include <...>`.
EXPORT_VARS = ("EXPORT_MI_LIST", "EXPORT_MD_LIST", "EXPORT_SF_MI_LCL_LIST",
               "INSTALL_MI_LIST", "INSTALL_KF_MD_LCL_LIST", "INSTALL_SF_MI_LCL_LIST")


def resolve_header(tree, comp, rest):
    """Where does `#include <comp/rest>` actually come from in this tree?

    The build exports  <tree>/<comp>/<comp>/<rest>  into
    EXPORT_HDRS/<comp>/<comp>/<rest>  and compiles with -I EXPORT_HDRS/<comp>,
    so the source file for <comp/rest> is <tree>/<comp>/<comp>/<rest>.
    (<comp>/<rest> is also tried, for components that export a flatter layout.)
    """
    candidates = (
        os.path.join(tree, comp, comp, rest),
        os.path.join(tree, comp, rest),
    )
    for c in candidates:
        if os.path.exists(c):
            return c
    return None


def _parse_makefile(path):
    """name -> raw value, with continuation lines joined."""
    try:
        text = open(path, encoding="utf-8", errors="ignore").read()
    except OSError:
        return {}
    text = text.replace("\\\n", " ")
    vals = {}
    for line in text.splitlines():
        m = re.match(r"^([A-Za-z_][A-Za-z0-9_]*)\s*=\s*(.*)$", line)
        if m:
            vals[m.group(1)] = m.group(2)
        # append form "VAR += value"
        m = re.match(r"^([A-Za-z_][A-Za-z0-9_]*)\s*\+=\s*(.*)$", line)
        if m:
            vals[m.group(1)] = vals.get(m.group(1), "") + " " + m.group(2)
    return vals


def _expand(vals, value, depth=0):
    """Replace ${VAR} / $(VAR) recursively."""
    if depth > 8:
        return value

    def sub(m):
        name = m.group(1) or m.group(2)
        return _expand(vals, vals.get(name, ""), depth + 1)

    return re.sub(r"\$\{([A-Za-z_][A-Za-z0-9_]*)\}|\$\(([A-Za-z_][A-Za-z0-9_]*)\)", sub, value)


def makefile_lists(tree, header_path):
    """Header basenames that the Makefile next to header_path makes visible."""
    d = os.path.dirname(header_path)
    mk = os.path.join(d, "Makefile")
    if not os.path.exists(mk):
        return None, set()
    vals = _parse_makefile(mk)
    names = set()
    for var in EXPORT_VARS:
        if var not in vals:
            continue
        for tok in _expand(vals, vals[var]).split():
            tok = tok.strip("\\")
            if tok.endswith(".h"):
                names.add(os.path.basename(tok))
    return mk, names


def main(argv):
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("tree")
    ap.add_argument("files", nargs="*")
    ap.add_argument("--all-touched", action="store_true")
    a = ap.parse_args(argv)

    tree = os.path.abspath(a.tree)
    files = a.files or localcc.TOUCHED

    problems = 0
    checked = 0
    for rel in files:
        src = os.path.join(tree, rel)
        if not os.path.exists(src):
            continue
        text = open(src, encoding="utf-8", errors="ignore").read()
        for comp, rest in ANGLE_INC.findall(text):
            if comp not in COMPONENTS:
                continue
            header = resolve_header(tree, comp, rest)
            if header is None:
                print("MISSING  %s: <%s/%s> not found anywhere in the tree"
                      % (rel, comp, rest))
                problems += 1
                continue
            mk, listed = makefile_lists(tree, header)
            checked += 1
            if mk is None:
                # no Makefile in that directory: cannot check, say so
                continue
            base = os.path.basename(header)
            if base not in listed:
                print("NOT EXPORTED  %s includes <%s/%s>" % (rel, comp, rest))
                print("              %s exists but %r is not in %s"
                      % (header.replace(tree + os.sep, ""), base, mk.replace(tree + os.sep, "")))
                print("              -> add it to EXPORT_MI_LIST in that Makefile")
                problems += 1

    print()
    print("%d angle includes checked across %d files, %d problem(s)"
          % (checked, len(files), problems))
    return 0 if problems == 0 else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
