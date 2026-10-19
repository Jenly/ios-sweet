/*
 * sweet_spmi_v1.h -- SPMI PMIC-Arbiter v1 access for MSM8998 (chiron).
 *
 * WHY A SECOND SPMI IMPLEMENTATION
 * sweet_spmi.c speaks the **v5** arbiter protocol used by SM7150/sweet:
 *
 *     core  @ 0x0c440000, observer @ 0x0e600000, config @ 0x0c40a000
 *     APID map at core + 0x900 + 4*n, command ignores SID
 *     (pmic_arb_channel_map_offset_v5 / pmic_arb_fmt_cmd_v2)
 *
 * MSM8998 uses **v1** (arch/arm64/boot/dts/qcom/msm8998.dtsi:593):
 *
 *     core  @ 0x0800f000  (0x1000)   <- the arbiter registers
 *     chnls @ 0x8400000   (0x1000000)
 *     obsrvr@ 0x9400000   (0x1000000)
 *     intr  @ 0xa400000
 *     cnfg  @ 0x0800a000  (0x3000)
 *
 *     observer channel stride : 0x800 + 0x80 * channel
 *                               (pmic_arb_offset_v1, spmi-pmic-arb.c:890)
 *     APID map at core + 0x800 + 4*n
 *                               (pmic_arb_channel_map_offset_v2, used by the
 *                                v1 ops table at spmi-pmic-arb.c:1181-1193)
 *     command word            : (opc << 27) | ((sid & 0xf) << 20) | (addr << 4) | bc
 *                               (pmic_arb_fmt_cmd_v1, spmi-pmic-arb.c:1096)
 *
 * Pointing the v5 code at MSM8998 would read the wrong registers entirely: the
 * observer base would land in the "chnls" window and the APID search would walk
 * memory that is not a mapping table. That is why this is a separate file
 * rather than a #define change.
 *
 * WHAT IS SHARED
 * The per-channel register block itself is identical in both generations
 * (spmi-pmic-arb.c:37-43):
 *
 *     +0x00 CMD   +0x04 CONFIG   +0x08 STATUS
 *     +0x10 WDATA0 +0x14 WDATA1  +0x18 RDATA0  +0x1C RDATA1
 *
 *     STATUS bit0 DONE, bit1 FAILURE, bit2 DENIED, bit3 DROPPED
 *
 * so only the addressing and the command encoding differ.
 *
 * READ-ONLY BY CONSTRUCTION
 * This port only ever issues EXT_READL (opcode 1). There is no write path, no
 * interrupt clear, and no peripheral configuration -- the same constraint
 * sweet_spmi.c documents, kept because the PMIC also powers the CPU rails and a
 * stray write is not recoverable from software.
 */

#ifndef SWEET_SPMI_V1_H
#define SWEET_SPMI_V1_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Map the arbiter. Idempotent; 0 on success. */
int sweet_spmi_v1_init(void);

/*
 * Read one byte from (sid, addr). Thread context only.
 * 0 on success, -1 on failure; *out is untouched on failure.
 */
int sweet_spmi_v1_read(unsigned sid, unsigned addr, uint8_t *out);

/* Which arbiter generation was found at runtime: 0 = none, 1 = v1, 5 = v5. */
int sweet_spmi_arb_version(void);

/* Human-readable one-liner for the boot log. */
const char *sweet_spmi_arb_name(void);

#ifdef __cplusplus
}
#endif

#endif /* SWEET_SPMI_V1_H */
