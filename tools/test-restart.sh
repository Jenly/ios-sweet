#!/bin/bash
# test-restart.sh -- host test of the PSHOLD preflight.
#
# The driver writes zero to PS_HOLD to reset the SoC. If the preflight ever
# accepts a register that is not really PS_HOLD, that write goes somewhere
# unknown. This test is about the gate, not the reset: it checks that every
# implausible value is rejected and only an asserted hold is accepted.
set -uo pipefail
cd "$(dirname "$0")/.."

OUT=${OUT:-/tmp/sweet-restart-test}
mkdir -p "$OUT"

echo "sweet_restart preflight test"
cc -O1 -std=gnu11 -Wall -Wextra -Wno-unused-function -Wno-unused-variable \
	-o "$OUT/restart-test" tests/restart_preflight_test.c || {
	echo "build failed" >&2
	exit 1
}
"$OUT/restart-test"
rc=$?

if [ $rc -eq 0 ]; then
	echo "restart preflight test OK"
else
	echo "restart preflight test FAILED" >&2
fi
exit $rc
