#!/usr/bin/env python3
"""reconstruct_file.py -- rebuild a file the way the patch series does.

The drift check used to reconstruct a source file from the '+' lines of a single
patch. That stopped working the moment a second patch touched the same file (0085
adds the PMIC power-up calls to chiron_touch.c).

This applies the hunks of every patch that mentions the file, in series order,
starting from nothing. A file created by one patch and modified by later ones
comes out exactly as `git apply` would leave the working tree -- which is the
thing the drift check actually wants to compare against.

Usage: reconstruct_file.py TREE_PATH PATCH_DIR
       (prints the reconstructed file to stdout; exit 1 on a bad hunk)
"""
import os
import re
import sys


def hunks_for(patch_text, target):
    """Yield (old_start, old_count, new_count, body_lines) for `target`."""
    cur = None
    in_hunk = False
    body = []
    start = old_count = new_count = 0

    for line in patch_text.splitlines():
        if line.startswith("diff --git "):
            if in_hunk and cur == target:
                yield start, old_count, new_count, body
            in_hunk = False
            body = []
            m = re.match(r"diff --git a/(\S+) b/(\S+)", line)
            cur = m.group(2) if m else None
            continue
        if cur != target:
            continue
        if line.startswith("@@"):
            if in_hunk:
                yield start, old_count, new_count, body
            m = re.match(r"@@ -(\d+)(?:,(\d+))? \+(\d+)(?:,(\d+))? @@", line)
            if not m:
                raise SystemExit("unparseable hunk header: %r" % line)
            start = int(m.group(1))
            old_count = int(m.group(2)) if m.group(2) is not None else 1
            new_count = int(m.group(4)) if m.group(4) is not None else 1
            body = []
            in_hunk = True
            continue
        if in_hunk:
            # stop at trailing mail signature noise
            if line.startswith("-- ") and line.strip() == "-- ":
                continue
            body.append(line)
    if in_hunk and cur == target:
        yield start, old_count, new_count, body


def apply_hunk(lines, start, old_count, new_count, body):
    """Apply one hunk to `lines` (list without newlines). Returns new list."""
    # The hunk's old text starts at `start` (1-based) or is empty when creating.
    idx = max(0, start - 1) if old_count else start

    old_block = []
    new_block = []
    for line in body:
        if not line:
            # a truly empty line in a diff means an empty context line
            old_block.append("")
            new_block.append("")
            continue
        tag, rest = line[0], line[1:]
        if tag == " ":
            old_block.append(rest)
            new_block.append(rest)
        elif tag == "-":
            old_block.append(rest)
        elif tag == "+":
            new_block.append(rest)
        elif tag == "\\":
            continue        # "\ No newline at end of file"
        else:
            raise SystemExit("bad hunk line: %r" % line)

    if old_count == 0 and not old_block:
        # pure insertion
        return lines[:idx] + new_block + lines[idx:]

    actual = lines[idx:idx + len(old_block)]
    if actual != old_block:
        # try a small search window, the way patch(1) does with fuzz
        window = range(max(0, idx - 40), min(len(lines), idx + 40))
        found = None
        for cand in window:
            if lines[cand:cand + len(old_block)] == old_block:
                found = cand
                break
        if found is None:
            raise SystemExit(
                "hunk does not apply at line %d:\n  expected %r\n  found    %r"
                % (start, old_block[:2], actual[:2]))
        idx = found
    return lines[:idx] + new_block + lines[idx + len(old_block):]


def main(argv):
    if len(argv) != 2:
        raise SystemExit(__doc__)
    target, patch_dir = argv

    lines = []
    patches = sorted(p for p in os.listdir(patch_dir) if p.endswith(".patch"))
    applied = 0
    for name in patches:
        text = open(os.path.join(patch_dir, name), encoding="utf-8",
                    errors="replace").read()
        if ("b/" + target) not in text:
            continue
        for start, oc, nc, body in hunks_for(text, target):
            lines = apply_hunk(lines, start, oc, nc, body)
            applied += 1

    if applied == 0:
        raise SystemExit("%s is not produced by any patch in %s"
                         % (target, patch_dir))

    sys.stdout.write("\n".join(lines) + ("\n" if lines else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
