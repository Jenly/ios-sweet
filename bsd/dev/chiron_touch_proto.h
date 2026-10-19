/*
 * Wire-format helpers for the chiron (MSM8998) touch driver.
 *
 * These are the parts of bsd/dev/chiron_touch.c that are pure data
 * transformation: turning an I2C write into QUP v2 FIFO words, and turning an
 * RMI4 F12 report into a contact mask and coordinates. They live in their own
 * header with no kernel dependency so tools/test-chiron-touch.sh can run them
 * on the host against expectations derived from the vendor driver.
 *
 * Sources, both in LineageOS/android_kernel_xiaomi_msm8998:
 *   drivers/i2c/busses/i2c-qup.c        qup_i2c_issue_write / qup_i2c_issue_read
 *   drivers/input/touchscreen/synaptics_dsx/synaptics_dsx_core.c
 *                                       synaptics_rmi4_f12_abs_report
 */
#ifndef _CHIRON_TOUCH_PROTO_H
#define _CHIRON_TOUCH_PROTO_H

#include <stdint.h>

/* QUP v2 FIFO tags (i2c-qup.c). Each 16-bit half of a FIFO word is tag | byte. */
#define CT_TAG_START (1u << 8)
#define CT_TAG_DATA  (2u << 8)
#define CT_TAG_STOP  (3u << 8)
#define CT_TAG_REC   (4u << 8)
#define CT_MSW_SHIFT 16u        /* I2C_N_VAL + 1 = 16: second byte of the word */

/* Length of one F12 contact block and the status bits per contact. */
#define CT_F12_FINGER_LEN 8u
#define CT_F12_STATUS_FINGER 1u

/*
 * Pack one I2C write into QUP OUT-FIFO words.
 *
 * The FIFO takes 32-bit words holding two tag|byte pairs, low half first. The
 * first pair is START|addr<<1, every data byte carries DATA, the last carries
 * STOP. Writes out at most (len + 2) / 2 words and returns that count.
 */
static inline unsigned
ct_pack_i2c_write(uint8_t addr, const uint8_t *buf, unsigned len, uint32_t *out)
{
	uint32_t val = CT_TAG_START | ((uint32_t)addr << 1);
	unsigned idx = 1, pos = 0, nwords = 0;

	while (pos < len) {
		uint32_t tag = (pos == len - 1) ? CT_TAG_STOP : CT_TAG_DATA;

		if (idx & 1) {
			val |= (tag | buf[pos]) << CT_MSW_SHIFT;
		} else {
			val = tag | buf[pos];
		}
		/*
		 * A word is complete when the high half is filled. On the last byte
		 * it must be written even if it landed in the low half -- otherwise
		 * the STOP tag (and with it the end of the transaction) is dropped,
		 * which is exactly the bug the unit test in
		 * tools/test-chiron-touch.sh exists to catch.
		 */
		if ((idx & 1) || pos == len - 1) {
			out[nwords++] = val;
		}
		pos++;
		idx++;
	}
	return nwords;
}

/* The single word that requests a read: REC|len in the high half, START|R low. */
static inline uint32_t
ct_pack_i2c_read(uint8_t addr, unsigned len)
{
	return ((CT_TAG_REC | (len & 0xffu)) << CT_MSW_SHIFT) |
	    CT_TAG_START | (((uint32_t)addr << 1) | 1u);
}

/*
 * Decode one F12 report.
 *
 *   status   2 bits per contact: 00 absent, 01 finger, 10/11 other object types
 *   blocks   8 bytes per contact:
 *            [0] object type / status, [1] x_lsb, [2] x_msb,
 *            [3] y_lsb, [4] y_msb, [5] z, [6] wx, [7] wy
 *
 * Contacts that are not fingers are skipped (never delivered as touches).
 * A coordinate outside panel_w x panel_h is treated as a bad read: the whole
 * report is rejected with -1 so the caller can count the error instead of
 * putting a finger at a nonsense position.
 */
static inline int
ct_decode_f12(const uint8_t *status, const uint8_t *blocks, unsigned nfinger,
    unsigned blocklen, uint32_t panel_w, uint32_t panel_h, uint32_t *mask_out,
    int *x, int *y)
{
	uint32_t mask = 0;

	for (unsigned finger = 0; finger < nfinger; finger++) {
		unsigned shift = (finger & 3u) * 2u;
		unsigned st = (status[finger / 4] >> shift) & 0x3u;
		const uint8_t *p = blocks + finger * blocklen;
		unsigned px, py;

		if (st != CT_F12_STATUS_FINGER) {
			continue;
		}
		px = ((unsigned)p[2] << 8) | p[1];
		py = ((unsigned)p[4] << 8) | p[3];
		if (px >= panel_w || py >= panel_h) {
			return -1;
		}
		mask |= 1u << finger;
		x[finger] = (int)px;
		y[finger] = (int)py;
	}
	*mask_out = mask;
	return 0;
}

#endif /* _CHIRON_TOUCH_PROTO_H */
