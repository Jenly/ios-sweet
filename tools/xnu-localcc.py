#!/usr/bin/env python3
"""Compile XNU translation units on a non-macOS host.

The project's kernel build normally runs on a macOS GitHub runner, which means
every patch costs a CI round trip to find out whether it compiles. It does not
have to: clang can target Darwin from Linux (`-target arm64-apple-macos11`),
almost every header XNU needs is in the source tree, and the few things that
come from the SDK or from `mig` can be stubbed. This script assembles that
environment and compiles the files you name.

    tools/xnu-localcc.py TREE                  # check all Sweet sources
    tools/xnu-localcc.py TREE bsd/dev/chiron_touch.c
    tools/xnu-localcc.py TREE --all-touched    # only files the patch series edits
    tools/xnu-localcc.py TREE FILE --obj OUT.o # emit a real Mach-O object

TREE is a patched XNU source tree (patches applied, staging scripts run).

WHAT THIS PROVES: syntax, types, declarations, config macros, C/C++ language
rules --i.e. everything that makes a CI run fail with "error:" rather than
"Undefined symbols". It compiles the real headers from the real tree.

WHAT IT DOES NOT PROVE: linking. There is no linker, no cctools, and the
mig-generated message stubs are hand-written here (see MIG_STUBS) so anything
that actually calls a MACH_MSG routine is unverified. Treat a clean run as
"the patch will compile", not "the kernel will link".

Requires: clang with the Darwin target support (Ubuntu's clang-18 works).
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile

# --------------------------------------------------------------------------
# The kernel does not link against libc, so the C standard library only has to
# be *declared* for the headers to typecheck. These are declarations only.
# --------------------------------------------------------------------------
STRING_H = """\
#pragma once
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
void *memcpy(void *, const void *, size_t);
void *memmove(void *, const void *, size_t);
void *memset(void *, int, size_t);
int memcmp(const void *, const void *, size_t);
void *memchr(const void *, int, size_t);
size_t strlen(const char *);
size_t strnlen(const char *, size_t);
int strcmp(const char *, const char *);
int strncmp(const char *, const char *, size_t);
char *strcpy(char *, const char *);
char *strncpy(char *, const char *, size_t);
char *strlcpy(char *, const char *, size_t);
char *strcat(char *, const char *);
char *strlcat(char *, const char *, size_t);
char *strncat(char *, const char *, size_t);
char *strchr(const char *, int);
char *strrchr(const char *, int);
char *strstr(const char *, const char *);
char *strdup(const char *);
char *strerror(int);
int strcasecmp(const char *, const char *);
int strncasecmp(const char *, const char *, size_t);
char *strsep(char **, const char *);
int bcmp(const void *, const void *, size_t);
void bcopy(const void *, void *, size_t);
void bzero(void *, size_t);
#ifdef __cplusplus
}
#endif
"""

STDIO_H = """\
#pragma once
#include <stddef.h>
#include <stdarg.h>
#ifdef __cplusplus
extern "C" {
#endif
int printf(const char *, ...);
int snprintf(char *, size_t, const char *, ...);
int vsnprintf(char *, size_t, const char *, va_list);
int sprintf(char *, const char *, ...);
int vprintf(const char *, va_list);
#ifdef __cplusplus
}
#endif
"""

STDLIB_H = """\
#pragma once
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
void *malloc(size_t);
void free(void *);
long strtol(const char *, char **, int);
unsigned long strtoul(const char *, char **, int);
int atoi(const char *);
void abort(void);
#ifdef __cplusplus
}
#endif
"""

LIMITS_H = """\
#pragma once
#define CHAR_BIT 8
#define SCHAR_MAX 127
#define UCHAR_MAX 255
#define INT_MAX 2147483647
#define INT_MIN (-2147483647 - 1)
#define UINT_MAX 4294967295U
#define LONG_MAX 9223372036854775807L
#define LONG_MIN (-9223372036854775807L - 1)
#define ULONG_MAX 18446744073709551615UL
#define PATH_MAX 1024
"""


# Apple-clang builtins that upstream clang lacks (see the note above the
# definition). Only their *shape* matters for a typecheck.
APPLE_BUILTINS_H = """\
#pragma once
#ifndef __builtin_xnu_type_signature
#define __builtin_xnu_type_signature(...) ""
#endif
#ifndef __builtin_xnu_types_compatible
#define __builtin_xnu_types_compatible(...) 0
#endif
#ifndef __builtin_xnu_type_summary
#define __builtin_xnu_type_summary(...) 0
#endif
"""

# The SDK's TargetConditionals.h: XNU's headers use only the TARGET_* family.
TARGET_CONDITIONALS_H = """\
#pragma once
#define TARGET_OS_MAC               1
#define TARGET_OS_OSX               1
#define TARGET_OS_IPHONE            0
#define TARGET_OS_IOS               0
#define TARGET_OS_TV                0
#define TARGET_OS_WATCH             0
#define TARGET_OS_VISION            0
#define TARGET_OS_BRIDGE            0
#define TARGET_OS_DRIVERKIT         0
#define TARGET_OS_SIMULATOR         0
#define TARGET_OS_EMBEDDED          0
#define TARGET_OS_MACCATALYST       0
#define TARGET_OS_NANO              0
#define TARGET_CPU_ARM64            1
#define TARGET_CPU_X86_64           0
#define TARGET_RT_64_BIT            1
#define TARGET_RT_LITTLE_ENDIAN     1
#define TARGET_RT_BIG_ENDIAN        0
"""

# --------------------------------------------------------------------------
# mig output for mach/clock.defs and friends. Generated by hand from the .defs
# files in the tree: only the declarations the C++ sources need to typecheck.
# NOT the real MIG output -- no message structures, no stubs.
# Empty stubs for the remaining mig headers: the Sweet sources include them
# transitively but never call those routines, so a typecheck does not need
# their contents. A source that *does* call one fails loudly here.
# --------------------------------------------------------------------------
MIG_STUBS = {
    "mach/clock.h": """\
#pragma once
#include <mach/mach_types.h>
#include <mach/clock_types.h>
kern_return_t clock_get_time(clock_serv_t clock_serv, mach_timespec_t *cur_time);
kern_return_t clock_get_attributes(clock_serv_t clock_serv, clock_flavor_t flavor,
    clock_attr_t clock_attr, mach_msg_type_number_t *clock_attrCnt);
kern_return_t clock_alarm(clock_serv_t clock_serv, alarm_type_t alarm_type,
    mach_timespec_t alarm_time, clock_reply_t alarm_port);
""",
    "mach/clock_priv.h": """\
#pragma once
#include <mach/mach_types.h>
#include <mach/clock_types.h>
kern_return_t clock_set_time(clock_ctrl_t clock_ctrl, mach_timespec_t new_time);
kern_return_t clock_set_attributes(clock_ctrl_t clock_ctrl, clock_flavor_t flavor,
    clock_attr_t clock_attr, mach_msg_type_number_t clock_attrCnt);
""",
    "mach/clock_reply_server.h": """\
#pragma once
/* mig output for clock_reply.defs; declarations only (see the module docstring) */
""",
    "mach/exc_server.h": """\
#pragma once
""",
    "mach/mach_exc_server.h": """\
#pragma once
""",
    "mach/notify_server.h": """\
#pragma once
""",
    "mach/host_priv.h": """\
#pragma once
""",
    "mach/host_security.h": """\
#pragma once
""",
    "mach/mach_host.h": """\
#pragma once
#include <mach/mach_types.h>
kern_return_t host_processor_info(host_t host, processor_flavor_t flavor,
    natural_t *out_processor_count, processor_info_array_t *out_processor_info,
    mach_msg_type_number_t *out_processor_infoCnt);
""",
    "mach/mach_port.h": """\
#pragma once
""",
    "mach/processor.h": """\
#pragma once
#include <mach/mach_types.h>
/* mig processor.defs; from osfmk/man/processor_start.html */
kern_return_t processor_start(processor_t processor);
kern_return_t processor_exit(processor_t processor);
kern_return_t processor_info(processor_t processor, processor_flavor_t flavor,
    host_t *host, processor_info_t processor_info_out,
    mach_msg_type_number_t *processor_info_outCnt);
""",
    "mach/processor_set.h": """\
#pragma once
""",
    "mach/task.h": """\
#pragma once
#include <mach/mach_types.h>
kern_return_t task_info(task_t target_task, task_flavor_t flavor,
    task_info_t task_info_out, mach_msg_type_number_t *task_info_outCnt);
""",
    "mach/thread_act.h": """\
#pragma once
#include <mach/mach_types.h>
/* mig thread_act.defs; kernel prototype from osfmk/man/thread_terminate.html */
kern_return_t thread_terminate(thread_t target_thread);
kern_return_t thread_suspend(thread_t target_thread);
kern_return_t thread_resume(thread_t target_thread);
""",
    "mach/upl.h": """\
#pragma once
""",
    "mach/vm_map.h": """\
#pragma once
""",
    # SDK header. libkern/c++/OSObject.h includes it, and the dispatch-variant
    # metaclass macros in OSMetaClass.h expand to a member of type
    # `<class>_Methods`, so that type has to exist. Empty is enough for a
    # typecheck; the sources never call Dispatch(). Class layout is NOT the
    # SDK's, so do not trust offsets/sizes from a local compile.
    # The per-class <Class>_Methods macros are generated by prepare() from
    # the tree itself; see gen_methods_header().
    "DriverKit/OSObject.h": """\
#pragma once
""",
    "DriverKit/IOMemoryDescriptor.h": """\
#pragma once
""",
    "DriverKit/IOMemoryMap.h": """\
#pragma once
""",
}

# Headers that the build generates from config/MASTER.arm64.sweet. A local tree
# usually lacks them; the config options they define come from the patch series.
GENERATED_H = {
    "iokit/kext-config.h": "/* generated by the CI build; nothing needed here */\n",
    "iokit/kext-config-arm64.h": "/* generated by the CI build */\n",
    "iokit/kext-config-arm64-sweet.h": "/* generated */\n",
    "pexpert/pexpert/arm64/board_config.h": None,   # must come from the tree
}

# Files the Sweet patch series touches; --all-touched compiles exactly these.
TOUCHED = [
    "bsd/dev/chiron_touch.c",
    "bsd/dev/sweet_keys.c",
    "bsd/dev/sweet_touch.c",
    "bsd/dev/sweet_usb.c",
    "iokit/Kernel/arm/SweetBattery.cpp",
    "iokit/Kernel/arm/SweetFramebuffer.cpp",
    "iokit/Kernel/arm/SweetPlatformExpert.cpp",
    "iokit/Kernel/arm/SweetCPU.cpp",
    "iokit/Kernel/arm/hid/SweetHID.cpp",
    "iokit/KernelConfigTables.cpp",
]



METHODS_MACRO = re.compile(
    r"OSDeclare(?:Default|Final|Abstract)?Structors(?:WithDispatch(?:AndNoOperators)?)?"
    r"\s*\(\s*([A-Za-z_]\w*)\s*\)")
DISPATCH_INC = re.compile(r'#include\s*<DriverKit/([A-Za-z0-9_.]+\.h)>')


def gen_methods_header(tree):
    """Declare an empty macro for every <Class>_Methods / _KernelMethods the
    tree's own headers will expand to."""
    classes = set()
    for root, _dirs, files in os.walk(tree):
        for name in files:
            if not name.endswith((".h", ".hpp", ".iig")):
                continue
            try:
                text = open(os.path.join(root, name), encoding="utf-8",
                            errors="ignore").read()
            except OSError:
                continue
            classes.update(METHODS_MACRO.findall(text))
    lines = ["#pragma once",
             "/* Generated by xnu-localcc.py: empty stand-ins for the",
             " * .iig-generated <Class>_Methods members. See the module docs. */"]
    for c in sorted(classes):
        lines.append("#define %s_Methods" % c)
        lines.append("#define %s_KernelMethods" % c)
    return "\n".join(lines) + "\n", len(classes)


def gen_driverkit_stubs(tree, stubdir):
    """Every <DriverKit/X.h> the tree includes but does not ship."""
    wanted = set()
    for root, _dirs, files in os.walk(tree):
        for name in files:
            if not name.endswith((".h", ".hpp", ".c", ".cpp", ".iig")):
                continue
            try:
                text = open(os.path.join(root, name), encoding="utf-8",
                            errors="ignore").read()
            except OSError:
                continue
            wanted.update(DISPATCH_INC.findall(text))
    made = 0
    for rel in sorted(wanted):
        target = os.path.join(stubdir, "DriverKit", rel)
        if os.path.exists(os.path.join(tree, "iokit", "DriverKit", rel)):
            continue
        os.makedirs(os.path.dirname(target), exist_ok=True)
        if not os.path.exists(target):
            with open(target, "w") as f:
                f.write("#pragma once\n/* SDK stub generated by xnu-localcc.py */\n")
            made += 1
    return made


def prepare(tmp):
    """Write the stub headers into a private include dir."""
    for name, body in (
        ("string.h", STRING_H),
        ("stdio.h", STDIO_H),
        ("stdlib.h", STDLIB_H),
        ("limits.h", LIMITS_H),
        ("inttypes.h", "#pragma once\n#include <stdint.h>\n"),
        ("TargetConditionals.h", TARGET_CONDITIONALS_H),
        ("xnu_apple_builtins.h", APPLE_BUILTINS_H),
    ):
        with open(os.path.join(tmp, name), "w") as f:
            f.write(body)
    for rel, body in MIG_STUBS.items():
        path = os.path.join(tmp, rel)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w") as f:
            f.write(body)
    tree = getattr(prepare, "_tree", None)
    if tree:
        header, nclasses = gen_methods_header(tree)
        with open(os.path.join(tmp, "xnu_methods.h"), "w") as f:
            f.write(header)
        nstubs = gen_driverkit_stubs(tree, tmp)
        prepare.last_report = (nclasses, nstubs)
    return tmp



# Headers/symbols that only exist in Apple's SDK. A translation unit that stops
# on one of these is *not* proven broken -- it simply cannot be checked here.
# Everything else counts as a real error.
SDK_MARKERS = (
    "DriverKit/OSObject.h",
    "OSObject_Methods",
    "OSMetaClassBase_Methods",
    "IOService_Methods",
    "iig",
)


def classify(stderr):
    """-> 'sdk' if the first error is an Apple-SDK dependency, else 'fail'."""
    errs = [l for l in stderr.splitlines() if "error:" in l]
    if not errs:
        return "fail"
    first = errs[0]
    if any(m in first for m in SDK_MARKERS):
        return "sdk"
    return "fail"


def compile_one(tree, stubdir, rel, obj=None, verbose=False):
    src = os.path.join(tree, rel)
    if not os.path.exists(src):
        return ("missing", f"{rel}: not in this tree", "")
    is_cxx = rel.endswith((".cpp", ".cc", ".cxx"))
    cmd = [
        "clang", "-target", "arm64-apple-macos11",
        "-std=gnu++17" if is_cxx else "-std=gnu99",
        "-O1", "-ffreestanding", "-nostdlibinc", "-isystem", stubdir,
        "-DARM64_BOARD_CONFIG_SWEET=1", "-DKERNEL=1", "-DKERNEL_PRIVATE=1",
        # XNU_KERNEL_PRIVATE gates a lot of declarations (delay() among them);
        # BSD_KERNEL_PRIVATE the BSD-internal ones. The real build defines both.
        "-DXNU_KERNEL_PRIVATE=1", "-DBSD_KERNEL_PRIVATE=1",
        # PRIVATE gates BSD-internal declarations (kqueue_id_t and friends)
        # that are then used outside the guard. The real build defines it.
        "-DPRIVATE=1",
        "-DXNU_KERNEL_PRIVATE=1", "-DBSD_KERNEL_PRIVATE=1",
        "-DXNU_TARGET_OS_OSX=1", "-D__arm64__=1", "-D__APPLE__=1",
        # silence the tree's own noise so real problems stand out
        "-Wno-visibility", "-Wno-typedef-redefinition",
        "-include", os.path.join(stubdir, "xnu_apple_builtins.h"),
        "-include", os.path.join(stubdir, "xnu_methods.h"),
        "-Wno-deprecated-declarations", "-Wno-unused-command-line-argument",
    ]
    for inc in ("osfmk", "bsd", "libkern", "pexpert", "iokit", "EXTERNAL_HEADERS"):
        cmd += ["-I", os.path.join(tree, inc)]
    cmd += ["-I", stubdir]
    if is_cxx:
        cmd += ["-x", "c++"]
    cmd += ["-c" if obj else "-fsyntax-only", src]
    if obj:
        cmd += ["-o", obj]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode == 0:
        return ("ok", "", r.stderr)
    errs = [l for l in r.stderr.splitlines()
            if ("error:" in l or "fatal error:" in l)]
    return (classify(r.stderr), "\n".join(errs[:12]), r.stderr)


def main(argv):
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("tree", help="a patched XNU source tree")
    ap.add_argument("files", nargs="*", help="paths relative to TREE")
    ap.add_argument("--all-touched", action="store_true",
                    help="check only the files the Sweet patches modify")
    ap.add_argument("--obj", help="emit a real Mach-O object here (single file)")
    ap.add_argument("--keep-stubs", action="store_true",
                    help="write the stub include dir to ./xnu-localcc-includes")
    a = ap.parse_args(argv)

    if not shutil.which("clang"):
        raise SystemExit("error: clang not found (apt install clang)")

    tree = os.path.abspath(a.tree)
    if not os.path.isdir(os.path.join(tree, "osfmk")):
        raise SystemExit(f"error: {tree} does not look like an XNU tree")

    if a.keep_stubs:
        stubdir = os.path.abspath("xnu-localcc-includes")
        os.makedirs(stubdir, exist_ok=True)
        prepare._tree = tree
        prepare(stubdir)
        tmp = None
    else:
        tmp = tempfile.mkdtemp(prefix="xnu-localcc-")
        prepare._tree = tree
        stubdir = prepare(tmp)

    try:
        files = list(a.files)
        if a.all_touched or not files:
            files = TOUCHED
        if a.obj and len(files) != 1:
            raise SystemExit("error: --obj needs exactly one file")

        ok = fail = miss = sdk = 0
        for rel in files:
            status, detail, _raw = compile_one(tree, stubdir, rel, a.obj)
            mark = {"ok": "ok  ", "fail": "FAIL", "sdk": "SDK ",
                    "missing": "----"}[status]
            print(f"{mark} {rel}")
            if status == "ok":
                ok += 1
            elif status == "missing":
                miss += 1
            elif status == "sdk":
                sdk += 1
                print("     needs Apple SDK headers (cannot be checked here): "
                      + detail.splitlines()[0].strip() if detail else "")
            else:
                fail += 1
                for line in detail.splitlines():
                    print(f"     {line}")
        print(f"\n{ok} compiled, {fail} failed, {sdk} SDK-blocked, "
              f"{miss} not in this tree")
        if sdk:
            print("SDK-blocked files are C++ sources whose headers live in Xcode; "
                  "a macOS runner is still needed for those.")
        if a.obj and ok == 1:
            print(f"object: {a.obj}")
        return 0 if fail == 0 else 1
    finally:
        if tmp:
            shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
