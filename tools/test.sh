#!/bin/bash
# test.sh -- every host-side check, in one place.
#
# Why this exists: each test was added with its own invocation, and none of them
# ran in CI. A test that only runs when someone remembers to type its name does
# not stop a regression. This wrapper is what the kernel workflow calls, so a
# broken register table, a broken device path or a patch/source drift fails the
# build instead of being discovered later.
#
# Two kinds of check:
#
#   * self-contained host tests -- they compile a source file for the host and
#     assert on it. No kernel tree needed, so they run before the build.
#   * tree checks -- they need a patched XNU tree, so they run after it exists.
#
# Usage:
#   ./tools/test.sh                # self-contained checks only
#   ./tools/test.sh --tree DIR     # plus the checks that need a patched tree
set -uo pipefail
cd "$(dirname "$0")/.."

TREE=
while [ $# -gt 0 ]; do
	case "$1" in
	--tree) TREE=${2:?--tree needs a directory}; shift 2 ;;
	*) echo "unknown argument: $1" >&2; exit 2 ;;
	esac
done

fail=0
ran=0

run() {
	local label=$1
	shift
	ran=$((ran + 1))
	printf '%-34s ' "$label"
	if out=$("$@" 2>&1); then
		echo "ok"
	else
		echo "FAILED"
		echo "$out" | sed 's/^/    /'
		fail=$((fail + 1))
	fi
}

skipped=0
skip() {
	printf '%-34s skipped (%s)\n' "$1" "$2"
	skipped=$((skipped + 1))
}

echo "== self-contained host tests =="

# MDP5 register encoding, re-derived from the Linux bitfields.
if [ -x tools/test-mdss.sh ]; then
	run "MDP5 register encoding" ./tools/test-mdss.sh
else
	skip "MDP5 register encoding" "tools/test-mdss.sh missing"
fi

# PSHOLD preflight rule: the pure function the restart driver calls.
if [ -x tools/test-restart.sh ]; then
	run "PSHOLD preflight" ./tools/test-restart.sh
else
	skip "PSHOLD preflight" "tools/test-restart.sh missing"
fi

# USB per-SoC table, against expectations transcribed from the device trees.
if [ -f tools/test-usb-soc.py ]; then
	run "USB SoC table" python3 tools/test-usb-soc.py bsd/dev/sweet_usb.c
else
	skip "USB SoC table" "tools/test-usb-soc.py missing"
fi

# The touch wire protocol plus patch-vs-source drift.
if [ -x tools/test-chiron-touch.sh ]; then
	run "touch protocol + drift" ./tools/test-chiron-touch.sh
else
	skip "touch protocol + drift" "tools/test-chiron-touch.sh missing"
fi

# The shim's FDT handling.
if [ -f tools/test-shim-fdt.py ]; then
	run "shim FDT" python3 tools/test-shim-fdt.py
else
	skip "shim FDT" "tools/test-shim-fdt.py missing"
fi

echo
echo "== checks that need a patched tree =="

if [ -z "$TREE" ]; then
	skip "header export" "no --tree given"
	skip "compile touched files" "no --tree given"
	skip "link check" "no --tree given"
	skip "board header against ADTs" "no --tree given"
else
	if [ ! -d "$TREE" ]; then
		echo "  --tree $TREE does not exist" >&2
		fail=$((fail + 1))
	else
		# A header that exists in the tree is invisible to #include <dir/X.h>
		# until the owning Makefile exports it; this is the check added after
		# exactly that broke a build.
		run "header export" python3 tools/xnu-hdrcheck.py "$TREE" --all-touched
		run "compile touched files" python3 tools/xnu-localcc.py "$TREE" --all-touched
		run "link check" python3 tools/xnu-linkcheck.py "$TREE" --all-touched
	fi
fi

# ADT / geometry regression: does not need the patched tree, only the generator.
if [ -f tools/test-board-header.py ]; then
	python3 tools/mkadt.py /tmp/t-sweet.adt --device sweet 2>/dev/null
	python3 tools/mkadt.py /tmp/t-chiron.adt --device chiron 2>/dev/null
	if [ -f /tmp/t-sweet.adt ] && [ -f /tmp/t-chiron.adt ]; then
		run "board header vs ADTs" python3 tools/test-board-header.py \
		    /tmp/t-sweet.adt /tmp/t-chiron.adt
	else
		skip "board header vs ADTs" "mkadt.py failed"
	fi
else
	skip "board header vs ADTs" "tools/test-board-header.py missing"
fi

echo
echo "$ran check(s) ran, $skipped skipped, $fail failed"
if [ $fail -eq 0 ]; then
	echo "ALL HOST CHECKS PASSED"
	exit 0
fi
echo "HOST CHECKS FAILED"
exit 1
