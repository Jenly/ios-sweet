#!/usr/bin/env python3
"""The 0094 log macro must not keep lr in a caller-saved register.

WHY. The first version saved lr in x16. x16/x17 are intra-procedure-call scratch
in AAPCS64, so sweet_early_mark() was free to overwrite them -- and did. The caller
then returned to a garbage address and the next function's `stp` faulted writing to
0xfffffe00080bbfd0, inside __BOOTDATA, with ESR 0x96000040 (translation fault,
level 0).

That is the same shape as 0091's bug: a probe sampling unverified register state.
So this checks the rule rather than the instance.

x0-x18 are caller-saved. x19-x28, x29/fp, x30/lr and sp survive a call.
"""
import io
import os
import re
import sys

ROOT = os.environ.get("XNU_TREE", "/root/src/xnuverify84")
path = os.path.join(ROOT, "osfmk/arm64/start.s")
s = io.open(path, encoding="utf-8").read()

m = re.search(r"\.macro SWEET_LOG_MARK.*?\.endm", s, re.S)
if not m:
    sys.exit("SWEET_LOG_MARK macro not found")
body = m.group(0)

# Every register the macro saves lr into.
saves = re.findall(r"\bmov\s+x(\d+), lr\b", body)
restores = re.findall(r"\bmov\s+lr, x(\d+)\b", body)
print("saves lr into : x%s" % saves)
print("restores  from: x%s" % restores)

CALLEE_SAVED = set(range(19, 29)) | {29}
fails = []
if not saves:
    fails.append("macro does not save lr at all")
for r in saves:
    if int(r) not in CALLEE_SAVED:
        fails.append("lr saved in x%s, which is caller-saved and will be clobbered by the call" % r)
if saves != restores:
    fails.append("save/restore registers differ: %r vs %r" % (saves, restores))

# The macro must also actually call the console.
if "bl\t\tEXT(sweet_early_mark)" not in body:
    fails.append("macro does not call sweet_early_mark")

print()
if fails:
    for f in fails:
        print("  FAIL  " + f)
    sys.exit(1)
print("PASS: lr is kept in a callee-saved register across the call")
