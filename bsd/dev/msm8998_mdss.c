/*
 * msm8998_mdss.c -- MDP5 hardware blit (DMA0 SSPP -> LM0 -> WB) for MSM8998.
 *
 * See msm8998_mdss.h for the safety model and for what is deliberately probed
 * rather than assumed. This file is written so that the *default* configuration
 * cannot damage a working display: it does nothing unless sweetmdss>=1, and
 * with sweetmdss=1 it only reads one register.
 *
 * Register sources, all read from this workspace:
 *   arch/arm64/boot/dts/qcom/msm8998-mdss.dtsi
 *       :15   compatible = "qcom,mdss_mdp"
 *       :17   reg = <0x0c900000 0x90000>, <0x0c9b0000 0x1040>
 *       :58   qcom,mdss-mdp-reg-offset = <0x00001000>
 *       :67   qcom,mdss-pipe-dma-off  = <0x25000 ...>
 *       :91   qcom,mdss-ctl-off       = <0x2000 ...>
 *       :93   qcom,mdss-mixer-intf-off = <0x45000 ...>
 *       :96   qcom,mdss-wb-off        = <0x66000>
 *   drivers/gpu/drm/msm/mdp/mdp5/mdp5.xml.h   register offsets and bitfields
 *   drivers/gpu/drm/msm/mdp/mdp_common.xml.h  BPC8 = 3, BPC8A = 3,
 *                                             MDP_PLANE_INTERLEAVED = 0,
 *                                             CHROMA_FULL = 0
 *   drivers/gpu/drm/msm/mdp/mdp_format.c      the FMT() table: BGRA8888 has
 *                                             a=r=g=b=8, unpack {3,2,0,1},
 *                                             alpha_enable, tight, cpp 4, cnt 4
 *   drivers/gpu/drm/msm/mdp/mdp5/mdp5_cfg.c   .mdp.base = { 0x01000 } for the
 *                                             8x96 config, i.e. MDP_HW_VERSION
 *                                             lives at mdp_phys + 0x1000 + 0
 *
 * Derived constants (computed from the bitfields above, not guessed):
 *   SRC_FORMAT for BGRA8888 = 0x000237FF
 *       A_BPC(3)<<6 | R_BPC(3)<<4 | G_BPC(3)<<0 | B_BPC(3)<<2
 *       | ALPHA_ENABLE | CPP(4-1)<<9 | UNPACK_COUNT(4-1)<<12 | PACK_TIGHT
 *   SRC_UNPACK for BGRA8888 = 0x01000203   (e0=3, e1=2, e2=0, e3=1)
 */

#include <sys/param.h>
#include <sys/types.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <kern/sched_prim.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>

#include "msm8998_mdss.h"

/*
 * machine/machine_routines.h declares ml_io_map only inside
 * MACH_KERNEL_PRIVATE, which a BSD driver does not define; bsd/dev/chiron_touch.c
 * declares it locally for the same reason.
 */
extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);

/* ------------------------------------------------------------- addresses */

#define MSM8998_MDSS_PA      0x0C900000u  /* msm8998-mdss.dtsi:17 */
#define MSM8998_MDSS_SIZE    0x00090000u
#define MSM8998_VBIF_PA      0x0C9B0000u
#define MSM8998_VBIF_SIZE    0x00001040u

/* Block bases, relative to MSM8998_MDSS_PA (msm8998-mdss.dtsi) */
#define OFF_MDP_TOP          0x00001000u  /* :58  reg_offset */
#define OFF_CTL0             0x00002000u  /* :91 */
#define OFF_DMA0_SSPP        0x00025000u  /* :67 */
#define OFF_LM0              0x00045000u  /* :93 */
#define OFF_WB               0x00066000u  /* :96 */

/* DMA0 SSPP registers (mdp5.xml.h, offsets inside the pipe block) */
#define PIPE_SRC_SIZE        0x0000u
#define PIPE_SRC_IMG_SIZE    0x0004u
#define PIPE_SRC_XY          0x0008u
#define PIPE_OUT_SIZE        0x000Cu
#define PIPE_OUT_XY          0x0010u
#define PIPE_SRC0_ADDR       0x0014u
#define PIPE_SRC1_ADDR       0x0018u
#define PIPE_SRC_STRIDE_A    0x0024u
#define PIPE_SRC_STRIDE_B    0x0028u
#define PIPE_SRC_FORMAT      0x0030u
#define PIPE_SRC_UNPACK      0x0034u
#define PIPE_SRC_OP_MODE     0x0038u

/* LM0 registers (sde_hw_lm.c values, same offsets in mdp5.xml.h) */
#define LM_OUT_SIZE          0x0004u
#define LM_BLEND0_OP         0x0020u
#define LM_BLEND0_FG_ALPHA   0x0024u
#define LM_BLEND0_BG_ALPHA   0x0028u

/* WB registers (mdp5.xml.h REG_MDP5_WB_*) */
#define WB_DST_FORMAT        0x0000u
#define WB_DST_OP_MODE       0x0004u
#define WB_DST_PACK_PATTERN  0x0008u
#define WB_DST0_ADDR         0x000Cu
#define WB_DST_YSTRIDE0      0x001Cu
#define WB_DST_WRITE_CONFIG  0x0048u
#define WB_OUT_SIZE          0x0074u
#define WB_ALPHA_X_VALUE     0x0078u

/* CTL registers (mdp5.xml.h REG_MDP5_CTL_*) */
#define CTL_LAYER0           0x0000u
#define CTL_OP               0x0014u
#define CTL_FLUSH            0x0018u
#define CTL_START            0x001Cu

/* WB_DST_FORMAT fields (mdp5.xml.h:1406-1450) */
#define WB_FMT_DSTC0(v)      (((v) & 3u) << 0)
#define WB_FMT_DSTC1(v)      (((v) & 3u) << 2)
#define WB_FMT_DSTC2(v)      (((v) & 3u) << 4)
#define WB_FMT_DSTC3(v)      (((v) & 3u) << 6)
#define WB_FMT_DST_BPP(v)    (((v) & 3u) << 9)
#define WB_FMT_PACK_COUNT(v) (((v) & 3u) << 12)
#define WB_FMT_PACK_TIGHT    0x00020000u

/* CTL_FLUSH bits (mdp5.xml.h:469-497) */
#define FLUSH_DMA0           0x00000800u
#define FLUSH_LM0            0x00000040u
#define FLUSH_WB             0x00010000u
#define FLUSH_CTL            0x00020000u
#define FLUSH_ALL_USED       (FLUSH_DMA0 | FLUSH_LM0 | FLUSH_WB | FLUSH_CTL)

/* CTL_OP: mode 2 is WB 2-line (mdp5_ctl.c set_ctl_op); no INTF, virtual pipe */
#define CTL_OP_WB_2_LINE     2u

/* A byte-identical BGRA8888 -> BGRA8888 copy should need this. */
#define SRC_FORMAT_BGRA8888  0x000237FFu
#define SRC_UNPACK_BGRA8888  0x01000203u
#define WB_BPP_8             3u
#define WB_PACK_COUNT_4      3u

/* ---------------------------------------------------------------- state */

static vm_offset_t s_mdss;
static vm_offset_t s_vbif;
static int         s_state = SWEET_MDSS_OFF;
static int         s_sleep_chan;
static uint32_t    s_dstc;
static uint64_t    s_blits;
static uint64_t    s_timeouts;
static uint64_t    s_failures;
static boolean_t   s_disable_logged;

/*
 * Channel-selector candidates, tried in order by the caller's verification.
 * Index 0 is the identity mapping (c0..c3 = 0,1,2,3), which is what a copy that
 * preserves byte order should need; index 1 is all-zero, the other obvious
 * encoding. The rest are permutations, so one boot can identify the real
 * encoding without a rebuild.
 */
static const uint8_t s_dstc_candidates[] = {
	0xE4,  /* c0=0 c1=1 c2=2 c3=3  identity                */
	0x00,  /* c0=0 c1=0 c2=0 c3=0  all from the same plane */
	0x24,  /* c0=0 c1=1 c2=2 c3=0                          */
	0x1B,  /* c0=3 c1=2 c2=1 c3=0                          */
	0x6C,  /* c0=0 c1=3 c2=2 c3=1                          */
	0x93,  /* c0=3 c1=0 c2=1 c3=2                          */
	0x39,  /* c0=1 c1=2 c2=3 c3=0                          */
	0x4E,  /* c0=2 c1=3 c2=0 c3=1                          */
	0xB1,  /* c0=1 c1=0 c2=3 c3=2                          */
	0xD2,  /* c0=2 c1=0 c2=3 c3=3                          */
	0xFF,  /* everything to plane 3                        */
};

/* --------------------------------------------------------------- helpers */

static uint32_t
md_rd(uint32_t off)
{
	return *(volatile uint32_t *)(s_mdss + off);
}

static void
md_wr(uint32_t off, uint32_t val)
{
	*(volatile uint32_t *)(s_mdss + off) = val;
}

static void
md_wait_ms(unsigned ms)
{
	uint64_t deadline;

	clock_interval_to_deadline(ms, NSEC_PER_MSEC, &deadline);
	(void)assert_wait_deadline(&s_sleep_chan, THREAD_UNINT, deadline);
	thread_block(THREAD_CONTINUE_NULL);
}

/* ---------------------------------------------------------------- probe */

int
sweet_mdss_state(void)
{
	return s_state;
}

int
sweet_mdss_ready(void)
{
	return s_state == SWEET_MDSS_ACTIVE;
}

uint32_t
sweet_mdss_set_dstc(uint32_t field)
{
	s_dstc = field & 0xFFu;
	return s_dstc;
}

uint32_t
sweet_mdss_dstc(void)
{
	return s_dstc;
}

int
sweet_mdss_dstc_candidate(uint32_t index)
{
	if (index >= sizeof(s_dstc_candidates)) {
		return -1;
	}
	s_dstc = s_dstc_candidates[index];
	return (int)s_dstc;
}

int
sweet_mdss_init(void)
{
	int     want = 0;
	uint32_t ver, major, minor, step;

	if (s_state != SWEET_MDSS_OFF) {
		return s_state == SWEET_MDSS_FAILED ? -1 : 0;
	}

	if (!PE_parse_boot_argn("sweetmdss", &want, sizeof(want)) || want <= 0) {
		/* Default: leave the display block completely alone. */
		return -1;
	}

	s_mdss = ml_io_map(MSM8998_MDSS_PA, MSM8998_MDSS_SIZE);
	if (s_mdss == 0) {
		printf("SWEET MDSS: cannot map MDP at %#x\n", MSM8998_MDSS_PA);
		s_state = SWEET_MDSS_FAILED;
		return -1;
	}

	/*
	 * The single cheapest experiment in this driver: if MDP_HW_VERSION reads
	 * as a plausible major/minor, the block is clocked and our mapping and
	 * block offset are both right. If it reads 0 or all-ones, nothing below
	 * is worth attempting.
	 */
	ver = md_rd(OFF_MDP_TOP + 0x0000u);
	major = (ver >> 28) & 0xF;
	minor = (ver >> 16) & 0xFFFu;
	step = ver & 0xFFFFu;

	if (ver == 0 || ver == 0xFFFFFFFFu || major == 0) {
		printf("SWEET MDSS: MDP_HW_VERSION=%#x looks dead "
		    "(major=%u minor=%u step=%u); not touching the pipeline\n",
		    ver, major, minor, step);
		s_state = SWEET_MDSS_FAILED;
		return -1;
	}

	printf("SWEET MDSS: MDP_HW_VERSION=%#x major=%u minor=%u step=%u\n",
	    ver, major, minor, step);

	if (want == 1) {
		/* Probe only: deliberately zero writes. */
		s_state = SWEET_MDSS_PROBE_ONLY;
		printf("SWEET MDSS: probe-only (sweetmdss=1); "
		    "set sweetmdss=2 to enable the blit\n");
		return 0;
	}

	s_vbif = ml_io_map(MSM8998_VBIF_PA, MSM8998_VBIF_SIZE);
	if (s_vbif == 0) {
		/* Not fatal: the blit does not program VBIF yet. */
		printf("SWEET MDSS: VBIF map failed (continuing)\n");
	}

	s_state = SWEET_MDSS_ACTIVE;
	printf("SWEET MDSS: blit enabled (sweetmdss=%d), dstc=%#x initially\n",
	    want, s_dstc);
	return 0;
}

/* ------------------------------------------------------------- the blit */

int
sweet_mdss_blit(uint32_t src_phys, uint32_t src_stride,
    uint32_t dst_phys, uint32_t dst_stride,
    uint32_t w, uint32_t h,
    uint32_t src_x, uint32_t src_y,
    uint32_t dst_x, uint32_t dst_y)
{
	uint32_t size, src_img, src_xy, out_xy;
	uint32_t wb_fmt;

	if (s_state != SWEET_MDSS_ACTIVE) {
		return -1;
	}
	if (w == 0 || h == 0 || w > 0xFFFFu || h > 0xFFFFu) {
		return -1;
	}
	/* Single 32-bit address registers: both ends must live below 4 GiB. */
	if (src_phys == 0 || dst_phys == 0 ||
	    (src_phys >> 32) != 0 || (dst_phys >> 32) != 0) {
		return -1;
	}
	if (src_stride < w * 4u || dst_stride < w * 4u) {
		return -1;
	}

	size    = (h << 16) | w;
	src_img = (h << 16) | w;      /* no scaling: image == output */
	src_xy  = ((src_y & 0xFFFFu) << 16) | (src_x & 0xFFFFu);
	out_xy  = ((dst_y & 0xFFFFu) << 16) | (dst_x & 0xFFFFu);

	/* ---- source pipe: linear BGRA8888, no scaling, no flip ---- */
	md_wr(OFF_DMA0_SSPP + PIPE_SRC_IMG_SIZE, src_img);
	md_wr(OFF_DMA0_SSPP + PIPE_SRC_XY, src_xy);
	md_wr(OFF_DMA0_SSPP + PIPE_SRC_SIZE, size);
	md_wr(OFF_DMA0_SSPP + PIPE_OUT_XY, out_xy);
	md_wr(OFF_DMA0_SSPP + PIPE_OUT_SIZE, size);
	md_wr(OFF_DMA0_SSPP + PIPE_SRC_STRIDE_A, src_stride & 0xFFFFu);
	md_wr(OFF_DMA0_SSPP + PIPE_SRC_STRIDE_B, 0);
	md_wr(OFF_DMA0_SSPP + PIPE_SRC0_ADDR, src_phys);
	md_wr(OFF_DMA0_SSPP + PIPE_SRC1_ADDR, 0);
	md_wr(OFF_DMA0_SSPP + PIPE_SRC_FORMAT, SRC_FORMAT_BGRA8888);
	md_wr(OFF_DMA0_SSPP + PIPE_SRC_UNPACK, SRC_UNPACK_BGRA8888);
	md_wr(OFF_DMA0_SSPP + PIPE_SRC_OP_MODE, 0);

	/* ---- layer mixer: opaque stage-0 copy over the rectangle ---- */
	md_wr(OFF_LM0 + LM_OUT_SIZE,
	    (((dst_y + h) & 0xFFFFu) << 16) | ((dst_x + w) & 0xFFFFu));
	md_wr(OFF_LM0 + LM_BLEND0_FG_ALPHA, 0xFFu);
	md_wr(OFF_LM0 + LM_BLEND0_BG_ALPHA, 0u);
	md_wr(OFF_LM0 + LM_BLEND0_OP, 0u);

	/* ---- writeback destination: same 32-bpp layout as the source ---- */
	wb_fmt = WB_FMT_DSTC0(s_dstc & 3u) |
	    WB_FMT_DSTC1((s_dstc >> 2) & 3u) |
	    WB_FMT_DSTC2((s_dstc >> 4) & 3u) |
	    WB_FMT_DSTC3((s_dstc >> 6) & 3u) |
	    WB_FMT_DST_BPP(WB_BPP_8) |
	    WB_FMT_PACK_COUNT(WB_PACK_COUNT_4) |
	    WB_FMT_PACK_TIGHT;

	md_wr(OFF_WB + WB_DST_FORMAT, wb_fmt);
	md_wr(OFF_WB + WB_DST_OP_MODE, 0);
	md_wr(OFF_WB + WB_DST_PACK_PATTERN, 0);
	md_wr(OFF_WB + WB_DST0_ADDR, dst_phys + dst_y * dst_stride + dst_x * 4u);
	md_wr(OFF_WB + WB_DST_YSTRIDE0, dst_stride & 0xFFFFu);
	md_wr(OFF_WB + WB_OUT_SIZE, size);
	md_wr(OFF_WB + WB_ALPHA_X_VALUE, 0xFFu);

	/*
	 * ---- commit ----
	 * Same order mdp5_ctl.c uses (:519-530): flush the blocks, then kick
	 * CTL_START. CTL_OP selects WB 2-line mode with no interface number,
	 * because this is a virtual (writeback) pipeline.
	 */
	md_wr(OFF_CTL0 + CTL_LAYER0, 0);
	md_wr(OFF_CTL0 + CTL_OP, CTL_OP_WB_2_LINE);
	md_wr(OFF_CTL0 + CTL_FLUSH, FLUSH_ALL_USED);
	md_wr(OFF_CTL0 + CTL_START, 1);

	s_blits++;
	return 0;
}

int
sweet_mdss_wait_idle(uint32_t timeout_us)
{
	uint64_t deadline, now;
	uint32_t left;

	if (s_state != SWEET_MDSS_ACTIVE) {
		return -1;
	}
	if (timeout_us == 0) {
		timeout_us = 20000;   /* 20 ms, a bit more than one 60 Hz frame */
	}

	/*
	 * FLUSH is write-1-to-clear per bit in this block, so a successful
	 * commit leaves it at zero. Poll it rather than trusting the write.
	 */
	clock_interval_to_deadline(timeout_us, NSEC_PER_USEC, &deadline);
	for (;;) {
		left = md_rd(OFF_CTL0 + CTL_FLUSH) & FLUSH_ALL_USED;
		if (left == 0) {
			return 0;
		}
		now = mach_absolute_time();
		if (now > deadline) {
			s_timeouts++;
			return -1;
		}
		md_wait_ms(1);
	}
}

void
sweet_mdss_disable(const char *why)
{
	if (s_state == SWEET_MDSS_ACTIVE) {
		s_failures++;
	}
	s_state = SWEET_MDSS_FAILED;
	if (!s_disable_logged) {
		s_disable_logged = TRUE;
		printf("SWEET MDSS: disabled permanently: %s\n",
		    why != NULL ? why : "(no reason given)");
	}
}

void
sweet_mdss_stats(void)
{
	printf("SWEET MDSS: state=%d blits=%llu timeouts=%llu failures=%llu dstc=%#x\n",
	    s_state, s_blits, s_timeouts, s_failures, s_dstc);
}
