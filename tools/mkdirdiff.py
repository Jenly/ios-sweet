#!/usr/bin/env python3
"""Build a git-style unified patch from the difference between two trees.

Why not `git diff`: the trees here are copies of a pristine XNU checkout with
patches applied by `git apply` and nothing committed, so `git diff` inside them
reports every earlier patch as well. Diffing two directory trees keeps patch
0084 limited to its own changes.

Usage: mkdirdiff.py BASE NEW OUT [git-prefix]
"""
import difflib
import os
import subprocess
import sys


def walk(root):
    out = {}
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = [d for d in dirnames if d != ".git"]
        for name in filenames:
            full = os.path.join(dirpath, name)
            rel = os.path.relpath(full, root).replace(os.sep, "/")
            out[rel] = full
    return out


def main(argv):
    base, new, out = argv[0], argv[1], argv[2]
    b = walk(base)
    n = walk(new)

    changed = []
    for rel in sorted(set(b) | set(n)):
        if rel not in b:
            changed.append(("new", rel))
        elif rel not in n:
            changed.append(("gone", rel))
        else:
            with open(b[rel], "rb") as f:
                old = f.read()
            with open(n[rel], "rb") as f:
                cur = f.read()
            if old != cur:
                changed.append(("mod", rel))

    chunks = []
    for kind, rel in changed:
        oldp = b.get(rel)
        newp = n.get(rel)
        try:
            old = open(oldp, encoding="utf-8").read().splitlines(True) if oldp else []
            cur = open(newp, encoding="utf-8").read().splitlines(True) if newp else []
        except UnicodeDecodeError:
            print("skipping binary file:", rel)
            continue

        head = "diff --git a/%s b/%s\n" % (rel, rel)
        if kind == "new":
            head += "new file mode 100644\n"
            a_name, b_name = "/dev/null", "b/" + rel
        elif kind == "gone":
            head += "deleted file mode 100644\n"
            a_name, b_name = "a/" + rel, "/dev/null"
        else:
            a_name, b_name = "a/" + rel, "b/" + rel

        body = "".join(difflib.unified_diff(old, cur, a_name, b_name, n=3))
        if body:
            chunks.append(head + body)
            print("%-6s %s" % (kind, rel))

    with open(out, "w", encoding="utf-8") as f:
        f.write("".join(chunks))
    print("\nwrote %s (%d files, %d lines)" % (out, len(chunks),
          sum(c.count("\n") for c in chunks)))


if __name__ == "__main__":
    main(sys.argv[1:])
