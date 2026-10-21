#!/usr/bin/env python3
"""The 0091 marker must read boot_args only after x20 is loaded with it.

WHY THIS TEST EXISTS. Patch 0091 puts a probe on the kernel's very first executed
instruction. It was inserted at `LEXT(start_first_cpu)`, but `mov x20, x0` -- the
instruction that makes boot_args addressable -- is two instructions *later*. The probe
therefore ran with a stale x20 and took an alignment fault at 0x5357454834 on chiron
(ESR 0x96000021), proving the kernel executed only by crashing inside the probe.

A test that only looked for `BA_MARKER_PA` in start.s would have passed on the broken
patch. This one asserts the *ordering* that matters, in the file the build actually
compiles AND in the patch that produces it.

Checked:
  1. in start.s, `mov x20, x0` precedes the `ldr ...[x20, BA_MARKER_PA]` read;
  2. that `mov` is the one inside start_first_cpu (preceded by DAIFSet), not some
     other function's;
  3. the marker block is not sitting at LEXT(start_first_cpu) bare;
  4. the patch's start.s hunk also places the block after the mov (context lines
     above the added block include `mov x20, x0`).
"""
import io
import os
import re
import sys

ROOT = os.environ.get("XNU_TREE", "")
if not ROOT:
    for cand in ("/root/src/xnuverify84", "/root/src/xnu91"):
        if os.path.isdir(cand):
            ROOT = cand
            break
if not ROOT:
    sys.exit("no XNU tree: set XNU_TREE")

fails = []
def check(cond, msg):
    print(("  ok    " if cond else "  FAIL  ") + msg)
    if not cond:
        fails.append(msg)

start_s = io.open(os.path.join(ROOT, "osfmk/arm64/start.s"),
                  encoding="utf-8").read()

# ---- 1/2: the mov inside start_first_cpu precedes the marker read ----
i_fn = start_s.find("LEXT(start_first_cpu)")
check(i_fn != -1, "start_first_cpu exists")

mov_positions = [m.start() for m in re.finditer(r"\tmov\s+x20, x0", start_s)]
# The relevant one is the first after start_first_cpu's entry.
m_in_fn = [p for p in mov_positions if p > i_fn]
check(bool(m_in_fn), "start_first_cpu contains `mov x20, x0`")

read_positions = [m.start() for m in re.finditer(r"BA_MARKER_PA", start_s)]
check(bool(read_positions), "the marker read exists")

if m_in_fn and read_positions:
    check(m_in_fn[0] < read_positions[0],
          "mov x20, x0 (%d) precedes BA_MARKER_PA (%d)" % (m_in_fn[0], read_positions[0]))

# ---- 3: not bare at the function label ----
if i_fn != -1 and read_positions:
    gap = start_s[i_fn:read_positions[0]]
    # Tolerate the label, blank lines and the DAIF/OSLAR pair, but the mov must be in it.
    check("mov" in gap and "x20, x0" in gap,
          "the marker is not the first instruction after LEXT(start_first_cpu)")

# ---- 3b: DAIFSet must also precede it (same reasoning: it runs first) ----
d_positions = [p for p in (m.start() for m in re.finditer(r"DAIFSet", start_s)) if p > i_fn]
if d_positions and read_positions:
    check(d_positions[0] < read_positions[0],
          "DAIFSet precedes the marker (interrupts masked first)")

# ---- 4: the patch agrees ----
patch_path = "xnu/patches/0091-sweet-first-instruction-marker.patch"
if os.path.exists(patch_path):
    lines = io.open(patch_path, encoding="utf-8").read().split("\n")
    # Find the start.s section, then the added marker line.
    try:
        i_sec = next(i for i, l in enumerate(lines) if l.startswith("diff --git") and "start.s" in l)
    except StopIteration:
        i_sec = None
    if i_sec is not None:
        i_add = next((i for i in range(i_sec, len(lines))
                      if lines[i].startswith("+") and "BA_MARKER_PA" in lines[i]), None)
        check(i_add is not None, "patch adds BA_MARKER_PA in the start.s hunk")
        if i_add is not None:
            above = lines[i_sec:i_add]
            ctx_mov = [l for l in above if l.startswith(" ") and re.match(r"\s+mov\s+x20, x0", l)]
            check(bool(ctx_mov),
                  "the patch's context above the marker includes `mov x20, x0`")
else:
    print("  skip  patch not present (running against an installed tree)")

print()
if fails:
    print("FAILED %d check(s)" % len(fails))
    sys.exit(1)
print("PASS: the 0091 marker reads boot_args only after x20 is loaded")
