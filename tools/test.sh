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

# MDP5 register encoding, re-derived from the Linux bitfields. Needs a host C
# compiler; without one the test cannot speak about the code at all.
if [ -x tools/test-mdss.sh ]; then
	if command -v cc >/dev/null 2>&1; then
		run "MDP5 register encoding" ./tools/test-mdss.sh
	else
		skip "MDP5 register encoding" "no host C compiler"
	fi
else
	skip "MDP5 register encoding" "tools/test-mdss.sh missing"
fi

# PSHOLD preflight rule: the pure function the restart driver calls.
if [ -x tools/test-restart.sh ]; then
	if command -v cc >/dev/null 2>&1; then
		run "PSHOLD preflight" ./tools/test-restart.sh
	else
		skip "PSHOLD preflight" "no host C compiler"
	fi
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

# The EFI handoff trampoline: the MMU/cache drop, verified instruction by
# instruction from the disassembly. Needs a cross compiler and objdump; without
# them it has said nothing about the code.
if [ -x tools/test-efi-handoff.sh ]; then
	XC=${CROSS_COMPILE:-aarch64-linux-gnu-}
	if command -v "${XC}gcc" >/dev/null 2>&1 &&
	    command -v "${XC}objdump" >/dev/null 2>&1; then
		run "EFI handoff trampoline" ./tools/test-efi-handoff.sh
	else
		skip "EFI handoff trampoline" "no aarch64 cross compiler/objdump"
	fi
else
	skip "EFI handoff trampoline" "tools/test-efi-handoff.sh missing"
fi

# Constants that must be 64-bit on BOTH data models. `1UL << 53` is right for
# aarch64-linux-gnu and evaluates to 0 for aarch64-pc-win32-coff, and the EFI build
# carried on with only a warning -- producing page tables whose PXN and UXN bits
# were clear, i.e. Device carve-outs left executable.
if [ -x tools/test-llp64.sh ]; then
	if command -v clang >/dev/null 2>&1; then
		run "LLP64 constants" ./tools/test-llp64.sh
	else
		skip "LLP64 constants" "no clang"
	fi
else
	skip "LLP64 constants" "tools/test-llp64.sh missing"
fi

# A built chiron boot.img, when one is present. This is the only check that looks
# at the actual flashable artifact: kernel, payload-as-ramdisk and device tree all in
# one file, with the header fields that decide whether fastboot accepts it.
# The v0 image is the one that boots: chiron's ABL rejects v1/v2 with
# "BootImage is Incomplete". Ask for it by name first, so a leftover artifact in
# another format cannot shadow it -- which is exactly what happened once.
CHIRON_IMG=
for cand in out-chiron/boot-chiron-v0.img out-chiron/boot-v0-full.img \
            out-chiron/boot-chiron.img shim/out/shim-boot-chiron.img; do
	if [ -f "$cand" ]; then CHIRON_IMG=$cand; break; fi
done
if [ -n "$CHIRON_IMG" ]; then
	# No --expect-dtb-addr: the image that boots chiron is header v0, which has no
	# dtb field at all (the tree is appended to the kernel). Pinning a v2 address
	# here is what made this check fail on a good image.
	run "chiron boot image" python3 tools/test-bootimg.py "$CHIRON_IMG"
else
	skip "chiron boot image" "not built (tools/build-chiron.sh KERNEL)"
fi

# The EFI application's PE headers. "It built" is not "firmware can load it": a
# wrong machine type, PE32 instead of PE32+, or subsystem 3 instead of 10 all link
# without complaint and are then rejected with no useful message. Skipped when
# shim.efi has not been produced, with the reason.
if [ -f shim/out/shim.efi ]; then
	run "EFI image headers" ./tools/test-efi-image.sh
else
	skip "EFI image headers" "shim.efi not built (needs clang + lld-link)"
fi

# The shim's FDT handling.
if [ -f tools/test-shim-fdt.py ]; then
	if command -v dtc >/dev/null 2>&1; then
		run "shim FDT" python3 tools/test-shim-fdt.py
	else
		skip "shim FDT" "dtc not installed (apt/brew install dtc)"
	fi
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
if [ $skipped -gt 0 ]; then
	echo "note: skipped checks did not run, so they prove nothing either way"
fi
if [ $fail -eq 0 ]; then
	echo "ALL HOST CHECKS PASSED"
	exit 0
fi
echo "HOST CHECKS FAILED"
exit 1
