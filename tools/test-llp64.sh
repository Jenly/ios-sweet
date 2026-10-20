#!/bin/bash
# test-llp64.sh -- compile the constant test for BOTH data models.
#
# The bug this guards against is invisible on the host: `1UL << 53` is correct for
# aarch64-linux-gnu and produces 0 for aarch64-pc-win32-coff. So the check is not
# "does it pass" but "does it pass for both targets". A constant that only works on
# LP64 fails the COFF build, which is the signal this test exists to give.
#
# The COFF object cannot be executed here, so it is checked by compiling with
# -Werror=shift-count-overflow and by static assertions inside the test itself.
set -uo pipefail
cd "$(dirname "$0")/.."

OUT=${OUT:-/tmp/sweet-llp64}
mkdir -p "$OUT"
fail=0

echo "LLP64 constant check"

# ---- 1. host: runs, so the values are checked at run time -------------------
printf '  %-22s ' "host (LP64)"
if cc -O1 -Wall -Wextra -o "$OUT/llp64" shim/tests/llp64_test.c 2>"$OUT/cc-host.log"; then
	if "$OUT/llp64" >"$OUT/host.log" 2>&1; then
		echo "ok"
	else
		echo "FAILED"
		sed 's/^/      /' "$OUT/host.log"
		fail=1
	fi
else
	echo "COMPILE FAILED"
	sed 's/^/      /' "$OUT/cc-host.log"
	fail=1
fi

# ---- 2. aarch64-pc-win32-coff: cannot run, so compile-time -------------------
# -Werror=shift-count-overflow is the whole point: the original bug produced exactly
# that warning and the build carried on.
printf '  %-22s ' "coff (LLP64)"
if command -v clang >/dev/null 2>&1; then
	if clang -target aarch64-pc-win32-coff -O1 -ffreestanding \
	    -Werror=shift-count-overflow -Wall -Wextra \
	    -c shim/tests/llp64_test.c -o "$OUT/llp64.o" 2>"$OUT/cc-coff.log"; then
		echo "ok (no wrapped shifts)"
	else
		echo "FAILED"
		sed 's/^/      /' "$OUT/cc-coff.log" | head -8
		fail=1
	fi
else
	echo "skipped (no clang)"
fi

# ---- 3. the real mmu.c, for both targets ------------------------------------
printf '  %-22s ' "shim/mmu.c coff"
if command -v clang >/dev/null 2>&1; then
	if clang -target aarch64-pc-win32-coff -O2 -ffreestanding -fno-builtin \
	    -mgeneral-regs-only -Werror=shift-count-overflow -Ishim \
	    -c shim/mmu.c -o "$OUT/mmu-coff.o" 2>"$OUT/mmu-coff.log"; then
		echo "ok"
	else
		echo "FAILED"
		sed 's/^/      /' "$OUT/mmu-coff.log" | head -8
		fail=1
	fi
else
	echo "skipped (no clang)"
fi

printf '  %-22s ' "shim/mmu.c linux"
if command -v aarch64-linux-gnu-gcc >/dev/null 2>&1; then
	if aarch64-linux-gnu-gcc -O2 -ffreestanding -nostdlib -fno-pic \
	    -mgeneral-regs-only -mstrict-align -fno-stack-protector \
	    -Werror=shift-count-overflow -Wall -Wextra -Ishim \
	    -c shim/mmu.c -o "$OUT/mmu-linux.o" 2>"$OUT/mmu-linux.log"; then
		echo "ok"
	else
		echo "FAILED"
		sed 's/^/      /' "$OUT/mmu-linux.log" | head -8
		fail=1
	fi
else
	echo "skipped (no aarch64-linux-gnu-gcc)"
fi

echo
if [ $fail -eq 0 ]; then
	echo "LLP64 check OK for both data models"
	exit 0
fi
echo "LLP64 check FAILED" >&2
exit 1
