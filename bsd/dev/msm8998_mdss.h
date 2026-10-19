/*
 * msm8998_mdss.h -- MDP5 hardware blit for the ios-sweet XNU port.
 *
 * Scope: MSM8998 (chiron / Mi MIX 2) only. That SoC's board file includes
 * msm8998-mdss.dtsi, whose MDP node is `compatible = "qcom,mdss_mdp"` -- the
 * MDP5 generation, not SDE. Verified at
 * arch/arm64/boot/dts/qcom/xiaomi/chiron-msm8998-mtp.dtsi.
 *
 * What it does: copies a 32-bpp rectangle from one physical address to another
 * through the MDP5 writeback path (DMA0 SSPP -> LM0 -> WB), replacing the
 * per-row CPU memcpy in AppleCLCD::present(). The CPU then writes ~25
 * registers per frame instead of touching every pixel.
 *
 * SAFETY MODEL -- read before enabling.
 *   * Off by default. sweetmdss absent or 0: the block is never mapped.
 *   * sweetmdss=1 probes only: maps MDSS, reads MDP_HW_VERSION, logs it, and
 *     performs no writes at all. This is how the first boot learns whether the
 *     block is clocked and whether the mapping is right.
 *   * sweetmdss=2 enables the blit, but the CALLER MUST VERIFY it: copy the
 *     first frame with the CPU, keep a reference, blit with hardware, compare.
 *     On mismatch call sweet_mdss_disable() with the reason. The driver has no
 *     copy of the pixels and cannot verify itself.
 *
 * WHY ADDRESSES MUST BE BELOW 4 GiB:
 *   MDP5's source and destination address registers are single 32-bit words
 *   (MDP5_PIPE_SRC0_ADDR, MDP5_WB_DST0_ADDR); there is no _HI companion. Both
 *   come from msm8998's low window (RAM 0xa0000000 + 0x60000000, panel memory
 *   0x9d400000), so this holds by construction.
 *
 * WHAT IS UNKNOWN AND THEREFORE PROBED, NOT ASSUMED:
 *   * Whether the bootloader leaves the MDSS SMMU bypassed. If MDP5 fetches
 *     through an unset translation the blit writes garbage -- which the
 *     caller's comparison catches.
 *   * The WB output channel order. WB_DST_FORMAT's DSTC0..3 fields (2 bits
 *     each) select which component reaches each output byte; the kernel in
 *     this workspace has no MDP5 writeback driver to copy the encoding from
 *     (drivers/gpu/drm/msm/mdp/mdp5 has the register names only -- __offset_WB
 *     is compiled out with `#if 0`, and qcom,mdss-wb-off is never parsed), so
 *     the encoding is discovered by trying candidates and keeping the one that
 *     reproduces the reference bytes.
 *   * Whether `qcom,mdss-wfd-mode = "intf"` (msm8998-mdss.dtsi) means WB must
 *     be fed from the interface rather than a layer mixer. If so this will not
 *     produce the rectangle and verification will disable it -- which rules the
 *     approach out cleanly rather than silently corrupting the panel.
 */

#ifndef _BSD_DEV_MSM8998_MDSS_H_
#define _BSD_DEV_MSM8998_MDSS_H_

#include <sys/types.h>

/* sweet_mdss_state() values */
#define SWEET_MDSS_OFF         0  /* not mapped, not touched            */
#define SWEET_MDSS_PROBE_ONLY  1  /* mapped, HW_VERSION read, no writes */
#define SWEET_MDSS_ACTIVE      2  /* blit permitted                     */
#define SWEET_MDSS_FAILED      3  /* mapped but permanently disabled    */

/*
 * Map MDSS and read MDP_HW_VERSION. Idempotent.
 * Returns 0 if the block answered sensibly (state PROBE_ONLY or ACTIVE),
 * negative if MDSS could not be mapped or the version register looked dead.
 */
int sweet_mdss_init(void);

int sweet_mdss_state(void);

/* 1 if a hardware blit may be attempted for this frame. */
int sweet_mdss_ready(void);

/*
 * Blit one 32-bpp rectangle; every address is physical and must be < 4 GiB.
 * (src_x, src_y) is the top-left inside the source, (dst_x, dst_y) on the
 * panel. Returns 0 when the commit was issued, negative otherwise.
 */
int sweet_mdss_blit(uint32_t src_phys, uint32_t src_stride,
    uint32_t dst_phys, uint32_t dst_stride,
    uint32_t w, uint32_t h,
    uint32_t src_x, uint32_t src_y,
    uint32_t dst_x, uint32_t dst_y);

/*
 * WB destination channel selectors. `field` is the raw 8-bit value packing
 * DSTC0..DSTC3 as (c0 | c1<<2 | c2<<4 | c3<<6). Returns what was stored.
 */
uint32_t sweet_mdss_set_dstc(uint32_t field);
uint32_t sweet_mdss_dstc(void);

/*
 * Select candidate channel-selector index (0..n-1) and return its field value,
 * or -1 when index is past the end. Candidate 0 is the identity mapping, which
 * is what a byte-identical copy should need; the caller walks the list and
 * keeps the first that reproduces its reference bytes.
 */
int sweet_mdss_dstc_candidate(uint32_t index);

/* Wait until the commit's flush bits are consumed. 0 ok, -1 timeout. */
int sweet_mdss_wait_idle(uint32_t timeout_us);

/* Disable permanently, logging `why` once. */
void sweet_mdss_disable(const char *why);

/* Log blit/timeout/failure counters. */
void sweet_mdss_stats(void);

#endif /* _BSD_DEV_MSM8998_MDSS_H_ */
