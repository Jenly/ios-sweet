#!/bin/bash
# Unit tests for the wire-format helpers of the chiron (MSM8998) touch driver.
#
# bsd/dev/chiron_touch_proto.h holds the two error-prone conversions:
#   ct_pack_i2c_write()  I2C write  -> QUP v2 FIFO words
#   ct_decode_f12()      RMI4 report -> contact mask + coordinates
# The expectations below are derived by hand from the vendor driver's protocol
# (i2c-qup.c, synaptics_dsx_core.c), not from this implementation's output, so
# a mistake here is caught rather than blessed.
#
#   tools/test-chiron-touch.sh        (needs only a host gcc)
set -e
cd "$(dirname "$0")/.."
OUT=${TMPDIR:-/tmp}/chiron-touch-test
rm -rf "$OUT" && mkdir -p "$OUT"

cat > "$OUT/test.c" <<'EOF'
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "chiron_touch_proto.h"

static int failures;

static void
check(const char *what, int ok)
{
	printf("  %-42s %s\n", what, ok ? "ok" : "FAIL");
	if (!ok) {
		failures++;
	}
}

/* ------------------------------------------------------------------ writes */

static void
test_pack_write(void)
{
	uint32_t out[8];
	unsigned n;

	printf("ct_pack_i2c_write\n");
	/* RMI4 page select: addr 0x20, bytes {0xff, page}. Word layout is
	 * low half = first pair, high half = second pair (i2c-qup.c:269-300). */
	{
		const uint8_t buf[] = { 0xff, 0x00 };
		n = ct_pack_i2c_write(0x20, buf, 2, out);
		check("page select -> 2 words", n == 2);
		check("word0 = START|0x40 | (DATA|0xff)<<16",
		    out[0] == (((CT_TAG_DATA | 0xffu) << 16) | (CT_TAG_START | 0x40u)));
		check("word1 = STOP|0x00",
		    out[1] == (CT_TAG_STOP | 0x00u));
	}
	/* A single data byte: START and STOP share one word. */
	{
		const uint8_t one[] = { 0x42 };
		n = ct_pack_i2c_write(0x20, one, 1, out);
		check("single byte -> 1 word", n == 1);
		check("word0 = START|0x40 | (STOP|0x42)<<16",
		    out[0] == (((CT_TAG_STOP | 0x42u) << 16) | (CT_TAG_START | 0x40u)));
	}
	/* Three bytes: pairs (START|addr, DATA|b0), (DATA|b1, STOP|b2). */
	{
		const uint8_t three[] = { 0x01, 0x02, 0x03 };
		n = ct_pack_i2c_write(0x20, three, 3, out);
		check("three bytes -> 2 words", n == 2);
		check("word0 pair order",
		    out[0] == (((CT_TAG_DATA | 0x01u) << 16) | (CT_TAG_START | 0x40u)));
		check("word1 pair order",
		    out[1] == (((CT_TAG_STOP | 0x03u) << 16) | (CT_TAG_DATA | 0x02u)));
	}
	/* Address is shifted left by one (the R/W bit sits in bit 0). */
	{
		const uint8_t one[] = { 0x00 };
		(void)ct_pack_i2c_write(0x38, one, 1, out);
		check("addr 0x38 -> START|0x70", (out[0] & 0xffffu) == (CT_TAG_START | 0x70u));
	}
}

static void
test_pack_read(void)
{
	uint32_t w;

	printf("ct_pack_i2c_read\n");
	w = ct_pack_i2c_read(0x20, 8);
	check("START|R set in low half", (w & 0xffffu) == (CT_TAG_START | 0x41u));
	check("REC|len in high half", (w >> 16) == (CT_TAG_REC | 8u));
	w = ct_pack_i2c_read(0x38, 64);
	check("addr 0x38, len 64", (w & 0xffffu) == (CT_TAG_START | 0x71u) &&
	    (w >> 16) == (CT_TAG_REC | 64u));
}

/* ------------------------------------------------------------------ decode */

static void
test_decode_f12(void)
{
	uint8_t status[3];
	uint8_t blocks[5 * CT_F12_FINGER_LEN];
	uint32_t mask = 0;
	int x[10], y[10];

	printf("ct_decode_f12\n");
	memset(status, 0, sizeof(status));
	memset(blocks, 0, sizeof(blocks));
	memset(x, 0, sizeof(x));
	memset(y, 0, sizeof(y));

	check("no contacts -> mask 0",
	    ct_decode_f12(status, blocks, 5, CT_F12_FINGER_LEN, 1080, 2160,
	    &mask, x, y) == 0 && mask == 0);

	/* Contact 0 down: status pair 0 = 0b01. Block bytes:
	 * [0] type/status, [1] x_lsb, [2] x_msb, [3] y_lsb, [4] y_msb */
	status[0] = 0x01;
	blocks[0] = 0x01;
	blocks[1] = 0x10; blocks[2] = 0x02;      /* x = 0x0210 = 528 */
	blocks[3] = 0x20; blocks[4] = 0x04;      /* y = 0x0420 = 1056 */
	check("one finger -> mask 0x1",
	    ct_decode_f12(status, blocks, 5, CT_F12_FINGER_LEN, 1080, 2160,
	    &mask, x, y) == 0 && mask == 0x1u);
	check("x == 528", x[0] == 528);
	check("y == 1056", y[0] == 1056);

	/* Second contact: status pair 1 sits at bit 2. */
	status[0] = 0x01 | (0x01 << 2);
	blocks[CT_F12_FINGER_LEN + 0] = 0x01;
	blocks[CT_F12_FINGER_LEN + 1] = 0x00; blocks[CT_F12_FINGER_LEN + 2] = 0x00;
	blocks[CT_F12_FINGER_LEN + 3] = 0x01; blocks[CT_F12_FINGER_LEN + 4] = 0x00;
	check("two fingers -> mask 0x3",
	    ct_decode_f12(status, blocks, 5, CT_F12_FINGER_LEN, 1080, 2160,
	    &mask, x, y) == 0 && mask == 0x3u);
	check("contact 1 at (0,1)", x[1] == 0 && y[1] == 1);

	/* Status 10 (stylus/palm-like) must not be delivered as a finger. */
	status[0] = 0x02;
	check("status 10 ignored",
	    ct_decode_f12(status, blocks, 5, CT_F12_FINGER_LEN, 1080, 2160,
	    &mask, x, y) == 0 && mask == 0);
	status[0] = 0x03;
	check("status 11 ignored",
	    ct_decode_f12(status, blocks, 5, CT_F12_FINGER_LEN, 1080, 2160,
	    &mask, x, y) == 0 && mask == 0);

	/* A coordinate outside the panel means a bad read: the report is
	 * rejected rather than turned into a phantom touch. */
	status[0] = 0x01;
	blocks[1] = 0xff; blocks[2] = 0xff;       /* x = 65535 */
	check("x out of panel rejected",
	    ct_decode_f12(status, blocks, 5, CT_F12_FINGER_LEN, 1080, 2160,
	    &mask, x, y) == -1);

	/* chiron's panel is 2160 tall: 2160 is out, 2159 is in. */
	blocks[1] = 0x00; blocks[2] = 0x00;
	blocks[3] = 0x70; blocks[4] = 0x08;       /* y = 0x0870 = 2160 */
	check("y == 2160 rejected",
	    ct_decode_f12(status, blocks, 5, CT_F12_FINGER_LEN, 1080, 2160,
	    &mask, x, y) == -1);
	blocks[3] = 0x6f; blocks[4] = 0x08;       /* y = 2159 */
	check("y == 2159 accepted",
	    ct_decode_f12(status, blocks, 5, CT_F12_FINGER_LEN, 1080, 2160,
	    &mask, x, y) == 0 && mask == 1 && y[0] == 2159);

	/* Only `nfinger` contacts are looked at, even if the status bytes have
	 * more bits set: the IC's config decides the count, not the data. */
	status[1] = 0xff;
	check("contacts beyond nfinger ignored",
	    ct_decode_f12(status, blocks, 5, CT_F12_FINGER_LEN, 1080, 2160,
	    &mask, x, y) == 0 && mask == 1);
}

int
main(void)
{
	test_pack_write();
	test_pack_read();
	test_decode_f12();
	printf(failures ? "\nFAILED (%d)\n" : "\nPASSED\n", failures);
	return failures != 0;
}
EOF

gcc -std=gnu99 -O2 -Wall -Wextra -Werror -Ibsd/dev -I"$OUT" \
    "$OUT/test.c" -o "$OUT/test"
"$OUT/test"

# ---------------------------------------------------------------------------
# Drift check: bsd/dev/ is the authoritative copy of the driver, and
# xnu/patches/ carries it into the kernel tree. If the two ever disagree the
# unit test above would be validating code that is not the code that ships.
# ---------------------------------------------------------------------------
echo
echo "patch/src drift check"
for src in bsd/dev/chiron_touch.c bsd/dev/chiron_touch_proto.h \
           bsd/dev/sweet_spmi_v1.c bsd/dev/sweet_spmi_v1.h \
           bsd/dev/sweet_pm8998_reg.c bsd/dev/sweet_pm8998_reg.h \
           bsd/dev/msm8998_mdss.c bsd/dev/msm8998_mdss.h \
           bsd/dev/chiron_keys.c \
           bsd/dev/sweet_hwprobe.c; do
	[ -f "$src" ] || { echo "  $src missing" >&2; exit 1; }
	# Rebuild the file the way the whole series does, not from one patch:
	# a file created by 0082 and modified by 0085 has to match the pair.
	if ! python3 "$(dirname "$0")/reconstruct_file.py" "$src" xnu/patches \
	    > "$OUT/from-patch.c" 2>"$OUT/err"; then
		echo "  $src: reconstruct failed" >&2
		cat "$OUT/err" >&2
		exit 1
	fi
	if diff -q "$src" "$OUT/from-patch.c" >/dev/null 2>&1; then
		echo "  $src matches the patch series"
	else
		echo "  MISMATCH: $src differs from what the patches install" >&2
		diff "$src" "$OUT/from-patch.c" | head -20 >&2
		exit 1
	fi
done
echo "USB SoC table check"
if python3 "$(dirname "$0")/test-usb-soc.py" bsd/dev/sweet_usb.c \
    > "$OUT/usb-soc.txt" 2>&1; then
	tail -3 "$OUT/usb-soc.txt"
else
	cat "$OUT/usb-soc.txt" >&2
	echo "  USB SoC table check FAILED" >&2
	exit 1
fi
echo "drift check OK"