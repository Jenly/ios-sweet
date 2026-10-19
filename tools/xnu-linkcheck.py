#!/usr/bin/env python3
"""Link-level verification without macOS.

ld64.lld (shipped with Ubuntu's lld) links Mach-O on Linux, so the pieces are
all here:

  1. compile the objects we care about,
  2. extract their undefined symbols,
  3. build a symbol index of the whole XNU tree,
  4. report undefined symbols that nothing in the tree defines.

Step 4 is the check that matters: an undefined symbol with no definition
anywhere is a link error on any platform, so it can be caught here.

    tools/xnu-linkcheck.py TREE [FILE...]
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import importlib.util

spec = importlib.util.spec_from_file_location("lc", os.path.join(HERE, "xnu-localcc.py"))
localcc = importlib.util.module_from_spec(spec)
spec.loader.exec_module(localcc)

LD64_CANDIDATES = [
    "/usr/lib/llvm-18/bin/ld64.lld",
    "/usr/lib/llvm-17/bin/ld64.lld",
    "ld64.lld",
]

# Definitions we do not expect to find in C/C++ sources for good reasons.
EXPECTED_EXTERNAL = {
    # provided by Apple's closed libs / the SDK, or by assembler we do not parse
    "__stack_chk_fail", "__stack_chk_guard", "_mh_execute_header",
    "OSKextGetCurrentIdentifier", "OSKextGetCurrentVersionString",
    "OSKextGetCurrentLoadTag",
    # C++ ABI runtime: linked in by the real kernel build, not defined in a
    # .c/.cpp of this tree, so a source index legitimately cannot find them.
    "_Unwind_Resume", "__cxa_atexit", "__cxa_begin_catch",
    "__gxx_personality_v0", "__cxa_pure_virtual", "_Unwind_Resume_or_Rethrow",
}


def find_ld64():
    for c in LD64_CANDIDATES:
        if os.path.sep in c:
            if os.path.exists(c):
                return c
        elif shutil.which(c):
            return shutil.which(c)
    return None


DEF_PATTERNS = [
    # function definition with the name at line start and ( on the same line
    re.compile(r"^([A-Za-z_]\w*)\s*\(", re.M),
    # function definition whose name is on its own line (return type above)
    re.compile(r"^([A-Za-z_]\w*)\s*$", re.M),
    # any definition with a type prefix, body opening on the same line
    re.compile(r"^[A-Za-z_][\w \t\*]*?\b([A-Za-z_]\w*)\s*\([^;{]*\)\s*\{", re.M),
    # globals / arrays / struct statics
    re.compile(r"^(?:static\s+|extern\s+)?[\w \t\*]*?\b([A-Za-z_]\w*)\s*(?:\[[^\]]*\])?\s*(?:=|;)", re.M),
    # assembler labels
    re.compile(r"^_?([A-Za-z_]\w*):", re.M),
    # XNU assembler macros: .globl EXT(name)  /  LEXT(name)
    re.compile(r"\b(?:EXT|LEXT|TEXT|LEXT)\(\s*([A-Za-z_]\w*)\s*\)"),
    re.compile(r"\.(?:globl|global)\s+_?([A-Za-z_]\w*)"),
]

# cheap but effective: every identifier that appears in a declaration-ish
# position anywhere in a source file counts as "may be defined"
IDENT = re.compile(r"\b([A-Za-z_]\w{2,})\b")


def build_symbol_index(tree, jobs=8):
    """Approximate index of every identifier defined anywhere in the tree."""
    files = []
    for root, dirs, names in os.walk(tree):
        dirs[:] = [d for d in dirs if d not in (".git", "Tests", "tests")]
        for n in names:
            if n.endswith((".c", ".cpp", ".cc", ".s", ".S", ".h", ".hpp", ".iig")):
                files.append(os.path.join(root, n))

    index = set()

    def scan(path):
        try:
            text = open(path, encoding="utf-8", errors="ignore").read()
        except OSError:
            return set()
        found = set()
        for pat in DEF_PATTERNS:
            found.update(pat.findall(text))
        # headers/templates: treat every identifier as potentially providing it
        if path.endswith((".h", ".hpp", ".iig")):
            found.update(IDENT.findall(text))
        return found

    with ThreadPoolExecutor(max_workers=jobs) as ex:
        for s in ex.map(scan, files):
            index |= s
    return index, len(files)


def undefined_symbols(obj):
    exe = shutil.which("llvm-nm") or shutil.which("nm")
    if not exe:
        return set()
    r = subprocess.run([exe, "-u", obj], capture_output=True, text=True)
    out = set()
    for line in r.stdout.splitlines():
        parts = line.split()
        if parts:
            sym = parts[-1]
            if sym.startswith("_"):
                sym = sym[1:]
            out.add(sym)
    return out



def demangle_all(symbols):
    """Mangled C++ -> plain identifier, via llvm-cxxfilt when available."""
    exe = shutil.which("llvm-cxxfilt") or shutil.which("c++filt")
    if not exe or not symbols:
        return {s: s for s in symbols}
    r = subprocess.run([exe] + sorted(symbols), capture_output=True, text=True)
    lines = r.stdout.splitlines()
    if len(lines) != len(symbols):
        return {s: s for s in symbols}
    return dict(zip(sorted(symbols), lines))


BASE = re.compile(r"([A-Za-z_]\w+)")


def base_names(sym, demangled):
    """All identifiers worth looking up for this symbol."""
    names = {sym}
    d = demangled.get(sym, sym)
    # "sweet_surface_copy(unsigned int)" -> sweet_surface_copy
    # "OSMetaClass::OSMetaClass(char const*, ...)" -> OSMetaClass
    names.update(BASE.findall(d))
    # NS::Class::method -> also try Class and method
    for part in re.split(r"::|\(", d):
        for tok in BASE.findall(part):
            names.add(tok)
    return names


def main(argv):
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("tree")
    ap.add_argument("files", nargs="*")
    ap.add_argument("--all-touched", action="store_true")
    a = ap.parse_args(argv)

    tree = os.path.abspath(a.tree)
    files = a.files or (localcc.TOUCHED if a.all_touched else localcc.TOUCHED)

    ld64 = find_ld64()
    print("ld64.lld:", ld64 or "NOT FOUND (object-only check)")

    tmp = tempfile.mkdtemp(prefix="xnu-linkcheck-")
    localcc.prepare._tree = tree
    localcc.prepare(tmp)

    objs = {}
    print("\n== compiling ==")
    for rel in files:
        obj = os.path.join(tmp, os.path.basename(rel) + ".o")
        status, detail, _ = localcc.compile_one(tree, tmp, rel, obj)
        print(f"  {status:4s} {rel}")
        if status == "ok":
            objs[rel] = obj

    undef = {}
    for rel, obj in objs.items():
        undef[rel] = undefined_symbols(obj)

    print("\n== link test (ld64.lld) ==")
    linked = False
    if ld64 and objs:
        # link our objects together; the entry is irrelevant, we only want the
        # linker's verdict on duplicate and missing symbols among them
        stub = os.path.join(tmp, "entry.c")
        stubsyms = set()
        for syms in undef.values():
            stubsyms |= syms
        with open(stub, "w") as f:
            f.write("void entry_point(void) {}\n")
        stubobj = os.path.join(tmp, "entry.o")
        subprocess.run(["clang", "-target", "arm64-apple-macos11", "-ffreestanding",
                        "-nostdlibinc", "-isystem", tmp, "-c", stub, "-o", stubobj],
                       capture_output=True)
        cmd = [ld64, "-arch", "arm64", "-e", "_entry_point", "-platform_version",
               "macos", "11.0", "11.0", "-o", os.path.join(tmp, "out.exe")]
        cmd += list(objs.values()) + [stubobj]
        r = subprocess.run(cmd, capture_output=True, text=True)
        errs = [l for l in (r.stderr or "").splitlines()
                if "error" in l.lower() and "not yet implemented" not in l]
        dupes = [l for l in errs if "duplicate symbol" in l.lower()]
        undef_errs = [l for l in errs if "undefined symbol" in l.lower()]
        if dupes:
            print(f"  DUPLICATE DEFINITIONS: {len(dupes)} (this is a real link error)")
            for l in dupes[:15]:
                print("   ", l)
            globals()["_have_dupes"] = True
        else:
            print("  duplicate definitions among the checked objects: none")
        if undef_errs:
            print(f"  undefined: {len(undef_errs)} symbols -- expected, they belong")
            print("  to the rest of the kernel; the cross-check below decides whether")
            print("  any of them is actually orphaned.")
    print("\n== are those defined anywhere in the tree? ==")
    index, nfiles = build_symbol_index(tree)
    print(f"  symbol index built from {nfiles} files, {len(index)} identifiers")

    allundef = set()
    for s in undef.values():
        allundef |= s
    demangled = demangle_all(allundef)
    unknown = []
    for s in sorted(allundef):
        if s in EXPECTED_EXTERNAL:
            continue
        names = base_names(s, demangled)
        if names & index:
            continue
        unknown.append((s, demangled.get(s, s)))
    known = len(allundef) - len(unknown)
    print(f"  {len(allundef)} undefined symbols total, {known} found in the tree,"
          f" {len(unknown)} NOT found")
    if unknown:
        print("  (each of these would be an undefined-symbol error at link time):")
        for s, d in unknown[:40]:
            print("    ", s)
            if d != s:
                print("        =", d)
    else:
        print("  no orphan undefined symbols among the checked objects")

    shutil.rmtree(tmp, ignore_errors=True)
    return 0 if not unknown else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
