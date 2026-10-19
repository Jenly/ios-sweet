#!/bin/bash
# test-mdss.sh -- host test of the MDP5 register encoding.
#
# The touch driver's STOP-bit bug was caught by a host test of the wire format,
# not by compiling. This does the same for bsd/dev/msm8998_mdss.c's constants.
set -uo pipefail
cd "$(dirname "$0")/.."

OUT=${OUT:-/tmp/sweet-mdss-test}
mkdir -p "$OUT"

echo "MDP5 register encoding test"
cc -O1 -std=c11 -Wall -Wextra -o "$OUT/mdss-test" tests/mdss_regs_test.c || {
	echo "build failed" >&2
	exit 1
}
"$OUT/mdss-test"
rc=$?

echo
echo "source/driver constant cross-check"
# The values the driver actually uses, read from the source rather than assumed.
for pair in "SRC_FORMAT_BGRA8888:0x000237FF" "SRC_UNPACK_BGRA8888:0x01000203"; do
	name=${pair%%:*}
	want=${pair##*:}
	got=$(grep -oE "#define $name +0x[0-9A-Fa-f]+" bsd/dev/msm8998_mdss.c | head -1 | grep -oE '0x[0-9A-Fa-f]+')
	if [ "$got" = "$want" ]; then
		echo "  $name = $got  ok"
	else
		echo "  $name = ${got:-missing}, expected $want" >&2
		rc=1
	fi
done

# Block bases must match msm8998-mdss.dtsi.
for pair in "OFF_DMA0_SSPP:0x00025000" "OFF_LM0:0x00045000" "OFF_WB:0x00066000" \
            "OFF_CTL0:0x00002000" "OFF_MDP_TOP:0x00001000"; do
	name=${pair%%:*}
	want=${pair##*:}
	got=$(grep -oE "#define $name +0x[0-9A-Fa-f]+" bsd/dev/msm8998_mdss.c | head -1 | grep -oE '0x[0-9A-Fa-f]+')
	if [ "$got" = "$want" ]; then
		echo "  $name = $got  ok"
	else
		echo "  $name = ${got:-missing}, expected $want" >&2
		rc=1
	fi
done

if [ $rc -eq 0 ]; then
	echo "mdss register test OK"
else
	echo "mdss register test FAILED" >&2
fi
exit $rc
