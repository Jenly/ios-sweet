/*
 * mdss-blit-skeleton.c -- MDP5 hardware blit for the ios-sweet XNU port.
 *
 * STATUS: skeleton only.  It is NOT part of the XNU build, is not referenced
 * from any header, and no Makefile lists it.  It exists so that the layout of
 * the work is fixed and reviewable: everything below is traced to a register
 * definition or a driver line that was actually read (citations inline).
 *
 * ---------------------------------------------------------------------------
 * WHAT IT REPLACES
 * ---------------------------------------------------------------------------
 * iokit/Kernel/arm/SweetFramebuffer.cpp, AppleCLCD::present() (xnufinal tree),
 * lines 393-399 of the current file:
 *
 *     if (src != 0 && bpe == 4) {
 *         for (int32_t y = 0; y < h; y++) {
 *             memcpy((void *)(fPixels + (uint64_t)(dy + y) * fRowBytes + (uint64_t)dx * 4),
 *                    (const void *)(src + s->offset + (uint64_t)(sy + y) * s->bytesPerRow
 *                                   + (uint64_t)sx * 4),
 *                    (size_t)w * 4);
 *         }
 *     }
 *
 * Today that row loop is the only per-frame pixel work in the kernel: one
 * 32-bpp rectangle per enabled CA layer, copied into the panel memory the
 * bootloader left scanning out (sweet 0x9c000000, chiron 0x9d400000,
 * tools/devices.py "fb").  The loop is called from present(), which
 * AppleCLCD runs from a timer at sweetfb_hz (default 10-30 Hz), not per swap;
 * see the comment at SweetFramebuffer.cpp:292-298.
 *
 * With this file wired in, the same rectangle is copied by the MDP5
 * write-back path: source surface -> VIG/DMA SSPP -> layer mixer -> WB ->
 * panel memory.  The CPU then only writes ~20 registers per layer per frame.
 *
 * ---------------------------------------------------------------------------
 * WHY THE ADDRESSES ARE 32-BIT (and why that is fine here)
 * ---------------------------------------------------------------------------
 * The MDP5 SSPP source-address registers are single 32-bit registers:
 *   mdp5.xml.h REG_MDP5_PIPE_SRC0_ADDR = 0x14 + pipe base            (no _HI)
 *   sde_hw_sspp.c:748-750  writes cfg->layout.plane_addr[i] with
 *   sde_reg_write() -> writel_relaxed()  (sde_hw_util.c, sde_reg_write body)
 * so src and dst must both live below 4 GiB (physical, or an SMMU IOVA).
 * That holds for us by construction: both boards' kernel RAM windows are
 * entirely below 4 GiB (tools/devices.py: sweet ram_low 0xb0000000+0x4d000000,
 * chiron ram_low 0xa0000000+0x60000000, and every IOKit buffer comes from
 * those windows), and the panel memory itself is at 0x9c000000 / 0x9d400000.
 *
 * ---------------------------------------------------------------------------
 * WHAT IS *NOT* SOLVED HERE (see gpu-research.md, "What still cannot be faked")
 * ---------------------------------------------------------------------------
 *  1. Clocks/power.  msm8998-mdss.dtsi:107-114 lists 7 clocks on the MDSS node
 *     (mnoc_clk, iface_clk, bus_clk, core_clk_src, core_clk, vsync_clk,
 *     lut_clk) plus vdd-supply = gdsc_mdss and gdsc-core-supply =
 *     gdsc_bimc_smmu.  This skeleton ASSUMES the bootloader left the display
 *     pipeline running (which it must have, otherwise nothing is on screen
 *     today).  It does NOT touch the clock controller.  UNVERIFIED.
 *  2. SMMU.  msm8998 routes MDP through mmss_smmu (msm8998-mdss.dtsi:233-259,
 *     smmu_mdp_unsec / smmu_mdp_sec context banks).  Two options:
 *       (a) leave the SMMU bypassed for the MDP context bank and program
 *           physical addresses (simplest, what this skeleton assumes), or
 *       (b) build a page table.  UNVERIFIED which state the bootloader leaves.
 *  3. Arbitration/QoS.  msm8998-mdss.dtsi:40-56 has VBIF QoS remapper values
 *     (qcom,mdss-vbif-qos-rt-setting = <1 2 2 2>, vbif-settings 0xac/0xd0).
 *     Not programmed here; the RT reads may be starved by the CPU otherwise.
 *  4. Tearing.  The blit writes panel memory while the display controller is
 *     scanning it out.  Need either a vsync-aligned start or a double buffer.
 *     The existing code has the same problem (it memcpy's live), so this is
 *     not a regression, but it is the obvious next fix.
 */

#ifndef __IOS_SWEET_MDSS_BLIT_SKELETON__
#define __IOS_SWEET_MDSS_BLIT_SKELETON__

#include <stdint.h>
#include <stddef.h>

/* ------------------------------------------------------------------ */
/* 1. Register map                                                     */
/* ------------------------------------------------------------------ */

/*
 * Base addresses, all from the live MSM8998 device tree fragment that this
 * repository already carries: arch/arm64/boot/dts/qcom/msm8998-mdss.dtsi
 * (LineageOS/android_kernel_xiaomi_msm8998).
 *
 *   :14   mdss_mdp: qcom,mdss_mdp@c900000
 *   :17   reg = <0x0c900000 0x90000>, <0x0c9b0000 0x1040>
 *         reg-names = "mdp_phys", "vbif_phys"
 *   :20   interrupts = <0 83 0>        (GIC SPI 83 -> INTID 32+83 = 115)
 *   :58   qcom,mdss-mdp-reg-offset = <0x00001000>
 *
 * Every block offset below is "relative to mdp_phys" per the comment at
 * msm8998-mdss.dtsi:77-79, i.e. the absolute address of an MDP5 register is
 *
 *     MDP_BASE + <block offset from DTS> + <register offset from mdp5.xml.h>
 *
 * and mdp5.xml.h's __offset_PIPE()/__offset_LM()/__offset_CTL() helpers return
 * exactly the DTS block offsets (mdp5.xml.h:541-556, 1066-1078, 355-366).
 */
#define MSM8998_MDP_BASE        0x0C900000u   /* msm8998-mdss.dtsi:17 */
#define MSM8998_VBIF_BASE       0x0C9B0000u   /* msm8998-mdss.dtsi:17 */
#define MSM8998_MDP_TOP         0x00001000u   /* msm8998-mdss.dtsi:58 */

/* SSPP (source pipe) block bases -- msm8998-mdss.dtsi:67-71 */
#define MSM8998_VIG0_SSPP       0x00005000u   /* :67 qcom,mdss-pipe-vig-off */
#define MSM8998_VIG1_SSPP       0x00007000u
#define MSM8998_VIG2_SSPP       0x00009000u
#define MSM8998_VIG3_SSPP       0x0000B000u
#define MSM8998_DMA0_SSPP       0x00025000u   /* :69 qcom,mdss-pipe-dma-off */
#define MSM8998_DMA1_SSPP       0x00027000u
#define MSM8998_DMA2_SSPP       0x00029000u
#define MSM8998_DMA3_SSPP       0x0002B000u

/* CTL (control / flush / kick) -- msm8998-mdss.dtsi:91 */
#define MSM8998_CTL0            0x00002000u
#define MSM8998_CTL1            0x00002200u

/* Layer mixer 0 ... 3 -- msm8998-mdss.dtsi:93 (note: on 8998 the mixer and
 * the interface are one block, hence the DTS name "mixer-intf") */
#define MSM8998_LM0             0x00045000u
#define MSM8998_LM1             0x00046000u

/* Writeback -- msm8998-mdss.dtsi:96 qcom,mdss-wb-off */
#define MSM8998_WB              0x00066000u

/* SSPP register offsets, from mdp5.xml.h (all "+ pipe base") */
#define MDP5_PIPE_SRC_SIZE          0x0000u  /* mdp5.xml.h:696  w[15:0] h[31:16] */
#define MDP5_PIPE_SRC_IMG_SIZE      0x0004u  /* mdp5.xml.h:710 */
#define MDP5_PIPE_SRC_XY            0x0008u  /* mdp5.xml.h:724  x[15:0] y[31:16] */
#define MDP5_PIPE_OUT_SIZE          0x000Cu  /* mdp5.xml.h:738 */
#define MDP5_PIPE_OUT_XY            0x0010u  /* mdp5.xml.h:752 */
#define MDP5_PIPE_SRC0_ADDR         0x0014u  /* mdp5.xml.h:766  (32-bit!) */
#define MDP5_PIPE_SRC1_ADDR         0x0018u  /* mdp5.xml.h:768 */
#define MDP5_PIPE_SRC_STRIDE_A      0x0024u  /* mdp5.xml.h:774  P0|P1<<16 */
#define MDP5_PIPE_SRC_STRIDE_B      0x0028u  /* mdp5.xml.h:788  P2|P3<<16 */
#define MDP5_PIPE_SRC_FORMAT        0x0030u  /* mdp5.xml.h:804 */
#define MDP5_PIPE_SRC_UNPACK        0x0034u  /* mdp5.xml.h:858 */
#define MDP5_PIPE_SRC_OP_MODE       0x0038u  /* mdp5.xml.h:884 */
#define MDP5_PIPE_SRC_CONSTANT_COLOR 0x003Cu /* mdp5.xml.h:901 */
#define MDP5_PIPE_FETCH_CONFIG      0x0048u  /* mdp5.xml.h:903 */

/* LM register offsets, from mdp5.xml.h (all "+ LM base") */
#define MDP5_LM_OP_MODE             0x0000u  /* sde_hw_lm.c:19 LM_OP_MODE */
#define MDP5_LM_OUT_SIZE            0x0004u  /* mdp5.xml.h:1086; sde_hw_lm.c:20 */
#define MDP5_LM_BORDER_COLOR_0      0x0008u  /* mdp5.xml.h:1100 */
#define MDP5_LM_BLEND0_OP           0x0020u  /* __offset_BLEND(0)=0x20, mdp5.xml.h:1104-1110 */
#define MDP5_LM_BLEND0_FG_ALPHA     0x0024u  /* sde_hw_lm.c:27 */
#define MDP5_LM_BLEND0_BG_ALPHA     0x0028u  /* sde_hw_lm.c:28 */

/* CTL register offsets, from mdp5.xml.h (all "+ CTL base") */
#define MDP5_CTL_LAYER0             0x0000u  /* mdp5.xml.h:380 */
#define MDP5_CTL_OP                 0x0014u  /* mdp5.xml.h:446; sde_hw_ctl.c:27 */
#define MDP5_CTL_FLUSH              0x0018u  /* mdp5.xml.h:468; sde_hw_ctl.c:28 */
#define MDP5_CTL_START              0x001Cu  /* mdp5.xml.h:499; sde_hw_ctl.c:29 */

/* WB register offsets, from sde_hw_wb.c:20-48.  SDE 3.0.0 is MSM8998 -- see
 * sde_hw_catalog.h:45-46, which annotates SDE_HW_VER_300 (3,0,0) as
 * "8998 v1.0" and SDE_HW_VER_301 (3,0,1) as "8998 v1.1". */
#define WB_DST_FORMAT               0x0000u  /* sde_hw_wb.c:20 */
#define WB_DST_OP_MODE              0x0004u  /* sde_hw_wb.c:21 */
#define WB_DST_PACK_PATTERN         0x0008u  /* sde_hw_wb.c:22 */
#define WB_DST0_ADDR                0x000Cu  /* sde_hw_wb.c:23 */
#define WB_DST_YSTRIDE0             0x001Cu  /* sde_hw_wb.c:27 */
#define WB_DST_WRITE_CONFIG         0x0048u  /* sde_hw_wb.c:35 */
#define WB_OUT_SIZE                 0x0074u  /* sde_hw_wb.c:42 */
#define WB_ALPHA_X_VALUE            0x0078u  /* sde_hw_wb.c:43 */
#define WB_DST_ADDR_SW_STATUS       0x02B0u  /* sde_hw_wb.c:45 */
#define WB_OUT_IMAGE_SIZE           0x02C0u  /* sde_hw_wb.c:47 */
#define WB_OUT_XY                   0x02C4u  /* sde_hw_wb.c:48 */

/* CTL_FLUSH bit assignments.  Two independent sources agree:
 *   - mdp5.xml.h:469-497   MDP5_CTL_FLUSH_VIG0=0x1 ... DMA0=0x800 ...
 *                          LM0=0x40 ... WB=0x10000 ... CTL=0x20000
 *   - sde_hw_ctl.c get_bitmask_sspp()/get_bitmask_mixer()/get_bitmask_wb(),
 *     which return BIT(0)/BIT(1)/BIT(2)/BIT(18) for VIG0..3, BIT(11)/BIT(12)
 *     for DMA0/1, BIT(6)..BIT(10) for LM0..4, and append BIT(17) for CTL.
 */
#define MDP5_FLUSH_VIG0         0x00000001u  /* mdp5.xml.h:469 */
#define MDP5_FLUSH_DMA0         0x00000800u  /* mdp5.xml.h:480 */
#define MDP5_FLUSH_LM0          0x00000040u  /* mdp5.xml.h:475 */
#define MDP5_FLUSH_WB           0x00010000u  /* mdp5.xml.h:485 */
#define MDP5_FLUSH_CTL          0x00020000u  /* mdp5.xml.h:486 */

/* MDP5_CTL_OP fields (mdp5.xml.h:447-459) */
#define MDP5_CTL_OP_MODE(v)         (((v) << 0)  & 0x0000000Fu)
#define MDP5_CTL_OP_INTF_NUM(v)     (((v) << 4)  & 0x00000070u)
#define MDP5_CTL_OP_CMD_MODE        0x00020000u

/* ------------------------------------------------------------------ */
/* 2. Formats                                                          */
/* ------------------------------------------------------------------ */
/*
 * SRC_FORMAT layout (mdp5.xml.h:804-857, and the encoder at
 * sde_hw_sspp.c:281-317):
 *   [1:0]   R_BPC-1        (COLOR_8BIT == 3 per bit, see sde_formats.c)
 *   [3:2]   G_BPC-1
 *   [5:4]   B_BPC-1
 *   [7:6]   A_BPC-1
 *   [8]     ALPHA_ENABLE   (sde_hw_sspp.c:289 writes BIT(8) as SRCC3_EN)
 *   [10:9]  CPP-1          (components per pixel - 1)
 *   [13:12] UNPACK_COUNT-1
 *   [17]    UNPACK_TIGHT
 *   [18]    UNPACK_ALIGN_MSB
 *   [20:19] FETCH_TYPE
 *   [22:23] CHROMA_SAMP
 * SRC_UNPACK holds the four element indices, one byte each (mdp5.xml.h:858+).
 * The element order is what makes ABGR vs RGBA: sde_formats.c:204-260 lists
 * ARGB8888 as elements { C1_B_Cb, C0_G_Y, C2_R_Cr, C3_ALPHA } and ABGR8888 as
 * { C2_R_Cr, C0_G_Y, C1_B_Cb, C3_ALPHA }.
 */
#define SWEET_FMT_BPC8          3u           /* 8 bits per component          */
#define SWEET_FMT_CPP4          3u           /* 4 components per pixel        */
#define SWEET_FMT_UNPACK_4      3u           /* 4 components, unpack_count-1  */

/* Element indices used by SRC_UNPACK (sde_formats.c:44-70 macro args) */
#define SWEET_ELEM_C0_G_Y       0u
#define SWEET_ELEM_C1_B_Cb      1u
#define SWEET_ELEM_C2_R_Cr      2u
#define SWEET_ELEM_C3_ALPHA     3u

/* One 32-bpp little-endian pixel: IOSurface 'BGRA' (0x42475241) and
 * 'ARGB'/'RGBA' are all handled by picking the element order; this is the
 * order for BGRA8888 (memory bytes B,G,R,A), i.e. sde_formats.c BGRA8888. */
#define SWEET_SRC_FORMAT_BGRA8888 \
	((SWEET_FMT_BPC8 << 0) | (SWEET_FMT_BPC8 << 2) | (SWEET_FMT_BPC8 << 4) | \
	 (SWEET_FMT_BPC8 << 6) | (1u << 8) | (SWEET_FMT_CPP4 << 9) |          \
	 (SWEET_FMT_UNPACK_4 << 12) | (1u << 17))
#define SWEET_SRC_UNPACK_BGRA8888 \
	((SWEET_ELEM_C3_ALPHA << 0) | (SWEET_ELEM_C2_R_Cr << 8) | \
	 (SWEET_ELEM_C0_G_Y << 16) | (SWEET_ELEM_C1_B_Cb << 24))

/* WB_DST_FORMAT for a linear 32-bpp RGB destination; field layout is
 * sde_hw_wb.c:96-133:
 *   [1:0][3:2][5:4][7:6] component bit depths-1 for C0..C3
 *   [8]   DSTC3_EN (alpha present)
 *   [10:9] DST_BPP-1
 *   [13:12] PACK_COUNT-1
 *   [17]  PACK_TIGHT, [18] PACK_ALIGN_MSB
 */
#define SWEET_WB_FORMAT_BGRA8888 \
	((SWEET_FMT_BPC8 << 0) | (SWEET_FMT_BPC8 << 2) | (SWEET_FMT_BPC8 << 4) | \
	 (SWEET_FMT_BPC8 << 6) | (1u << 8) | (SWEET_FMT_CPP4 << 9) |          \
	 (SWEET_FMT_UNPACK_4 << 12) | (1u << 17))
#define SWEET_WB_PACK_BGRA8888 \
	((SWEET_ELEM_C3_ALPHA << 24) | (SWEET_ELEM_C2_R_Cr << 16) | \
	 (SWEET_ELEM_C0_G_Y << 8) | (SWEET_ELEM_C1_B_Cb << 0))

/* ------------------------------------------------------------------ */
/* 3. The blit                                                         */
/* ------------------------------------------------------------------ */

struct sweet_blit {
	uint32_t src_phys;      /* < 4 GiB, see header comment            */
	uint32_t src_stride;    /* bytes per row                          */
	uint32_t src_w, src_h;  /* full source image size                 */
	uint32_t src_x, src_y;  /* source rect origin                     */
	uint32_t dst_phys;      /* < 4 GiB (panel memory or a scratch)    */
	uint32_t dst_stride;    /* bytes per row of the destination       */
	uint32_t dst_x, dst_y;  /* destination position                   */
	uint32_t w, h;          /* rectangle size, both must be > 0       */
};

/*
 * Register accessors.  In the real XNU driver these become
 *   OSWriteLittleInt32(fMDPBase, off, val) / OSReadLittleInt32(fMDPBase, off)
 * on a memory-mapped IOKit range obtained from the /arm-io/mdss node.  They
 * are function pointers here so the skeleton links in a host test harness.
 */
typedef void    (*sweet_mdp_write_fn)(void *ctx, uint32_t off, uint32_t val);
typedef uint32_t (*sweet_mdp_read_fn)(void *ctx, uint32_t off);

struct sweet_mdp {
	void                 *ctx;
	sweet_mdp_write_fn    write;
	sweet_mdp_read_fn     read;
};

static inline void
sweet_mdp_wr(struct sweet_mdp *m, uint32_t abs_off, uint32_t val)
{
	m->write(m->ctx, abs_off, val);
}

static inline uint32_t
sweet_mdp_rd(struct sweet_mdp *m, uint32_t abs_off)
{
	return m->read(m->ctx, abs_off);
}

/*
 * Program one SSPP for a source rectangle.  Mirrors
 * sde_hw_sspp_setup_sourceaddress()/..._setup_sourceformat()/
 * mdp5_plane_atomic_update() (mdp5_plane.c:755-806) but with no scaling:
 * SRC_SIZE == OUT_SIZE makes the scaler a pass-through, and leaving
 * SCALE_CONFIG untouched keeps it disabled (mdp5_plane.c:822-825 only writes
 * it when the state actually scales).
 */
static void
sweet_blit_setup_pipe(struct sweet_mdp *m, uint32_t pipe_base,
    const struct sweet_blit *b)
{
	uint32_t src_img = ((b->src_h & 0xFFFFu) << 16) | (b->src_w & 0xFFFFu);
	uint32_t src_xy  = ((b->src_y & 0xFFFFu) << 16) | (b->src_x & 0xFFFFu);
	uint32_t src_sz  = ((b->h     & 0xFFFFu) << 16) | (b->w     & 0xFFFFu);
	uint32_t out_xy  = ((b->dst_y & 0xFFFFu) << 16) | (b->dst_x & 0xFFFFu);

	sweet_mdp_wr(m, MSM8998_MDP_BASE + pipe_base + MDP5_PIPE_SRC_IMG_SIZE, src_img);
	sweet_mdp_wr(m, MSM8998_MDP_BASE + pipe_base + MDP5_PIPE_SRC_XY,      src_xy);
	sweet_mdp_wr(m, MSM8998_MDP_BASE + pipe_base + MDP5_PIPE_SRC_SIZE,    src_sz);
	sweet_mdp_wr(m, MSM8998_MDP_BASE + pipe_base + MDP5_PIPE_OUT_XY,      out_xy);
	sweet_mdp_wr(m, MSM8998_MDP_BASE + pipe_base + MDP5_PIPE_OUT_SIZE,    src_sz);

	/* Strides: P0 in [15:0], P1 in [31:16] (mdp5.xml.h:774-800).  A single
	 * plane leaves P1..P3 at zero, which is what mdp5_plane.c:397-402 does
	 * for the unused planes of a linear format. */
	sweet_mdp_wr(m, MSM8998_MDP_BASE + pipe_base + MDP5_PIPE_SRC_STRIDE_A, b->src_stride);
	sweet_mdp_wr(m, MSM8998_MDP_BASE + pipe_base + MDP5_PIPE_SRC_STRIDE_B, 0);

	sweet_mdp_wr(m, MSM8998_MDP_BASE + pipe_base + MDP5_PIPE_SRC0_ADDR, b->src_phys);
	sweet_mdp_wr(m, MSM8998_MDP_BASE + pipe_base + MDP5_PIPE_SRC1_ADDR, 0);

	sweet_mdp_wr(m, MSM8998_MDP_BASE + pipe_base + MDP5_PIPE_SRC_FORMAT,
	    SWEET_SRC_FORMAT_BGRA8888);
	sweet_mdp_wr(m, MSM8998_MDP_BASE + pipe_base + MDP5_PIPE_SRC_UNPACK,
	    SWEET_SRC_UNPACK_BGRA8888);
	/* No flip, no deinterlace, no pixel extension (mdp5_plane.c:799-805). */
	sweet_mdp_wr(m, MSM8998_MDP_BASE + pipe_base + MDP5_PIPE_SRC_OP_MODE, 0);
}

/*
 * LM0 output size = the destination rectangle position+size, so the mixer
 * window matches the panel area we are about to overwrite.  sde_hw_lm.c:76-95
 * (sde_hw_lm_setup_out) writes out_height<<16|out_width to LM_OUT_SIZE and
 * leaves BIT(31) clear for a non-split mixer.
 */
static void
sweet_blit_setup_lm(struct sweet_mdp *m, uint32_t lm_base,
    const struct sweet_blit *b)
{
	sweet_mdp_wr(m, MSM8998_MDP_BASE + lm_base + MDP5_LM_OUT_SIZE,
	    (((b->dst_y + b->h) & 0xFFFFu) << 16) | ((b->dst_x + b->w) & 0xFFFFu));
	/* Stage 0 opaque copy: source alpha ignored, no blending. */
	sweet_mdp_wr(m, MSM8998_MDP_BASE + lm_base + MDP5_LM_BLEND0_FG_ALPHA, 0xFFu);
	sweet_mdp_wr(m, MSM8998_MDP_BASE + lm_base + MDP5_LM_BLEND0_BG_ALPHA, 0u);
	sweet_mdp_wr(m, MSM8998_MDP_BASE + lm_base + MDP5_LM_BLEND0_OP, 0u);
}

/*
 * Writeback destination.  Follows sde_hw_wb_setup_format() (sde_hw_wb.c:81-160)
 * and sde_hw_wb_setup_outaddress() (sde_hw_wb.c:70-79).
 */
static void
sweet_blit_setup_wb(struct sweet_mdp *m, const struct sweet_blit *b)
{
	uint32_t outsize = ((b->h & 0xFFFFu) << 16) | (b->w & 0xFFFFu);

	sweet_mdp_wr(m, MSM8998_MDP_BASE + MSM8998_WB + WB_DST0_ADDR, b->dst_phys);
	sweet_mdp_wr(m, MSM8998_MDP_BASE + MSM8998_WB + WB_DST_YSTRIDE0, b->dst_stride);
	sweet_mdp_wr(m, MSM8998_MDP_BASE + MSM8998_WB + WB_DST_FORMAT,
	    SWEET_WB_FORMAT_BGRA8888);
	sweet_mdp_wr(m, MSM8998_MDP_BASE + MSM8998_WB + WB_DST_PACK_PATTERN,
	    SWEET_WB_PACK_BGRA8888);
	sweet_mdp_wr(m, MSM8998_MDP_BASE + MSM8998_WB + WB_DST_OP_MODE, 0);  /* linear */
	sweet_mdp_wr(m, MSM8998_MDP_BASE + MSM8998_WB + WB_OUT_SIZE, outsize);
	sweet_mdp_wr(m, MSM8998_MDP_BASE + MSM8998_WB + WB_ALPHA_X_VALUE, 0xFFu);
	sweet_mdp_wr(m, MSM8998_MDP_BASE + MSM8998_WB + WB_DST_ADDR_SW_STATUS, 0);
}

/*
 * The commit handshake, exactly as mdp5_ctl_commit() does it
 * (mdp5_ctl.c:495-530): write CTL_FLUSH with the mask of every block that was
 * reprogrammed, then pulse CTL_START.
 *
 * The layer-register value puts DMA0 into stage 0 of LM0:
 *   mdp5.xml.h:419-420  MDP5_CTL_LAYER_REG_DMA0__MASK = 0x001c0000, __SHIFT 18
 * so "pipe index 0 in stage 0" is 0 << 18 == 0.  SDE 3.0.0 repacks the layer
 * registers (sde_hw_ctl.c:345-455 builds mixercfg stage by stage); UNVERIFIED
 * whether the MDP5 packing above is still correct on 8998.  Read back
 * CTL_LAYER0 and compare with what the bootloader left there before trusting
 * this line -- that is the single cheapest experiment in the whole file.
 */
static void
sweet_blit_commit(struct sweet_mdp *m, uint32_t ctl_base, uint32_t flush_mask)
{
	sweet_mdp_wr(m, MSM8998_MDP_BASE + ctl_base + MDP5_CTL_LAYER0, 0u);
	sweet_mdp_wr(m, MSM8998_MDP_BASE + ctl_base + MDP5_CTL_FLUSH, flush_mask);
	/* CTL_OP: no INTF (virtual/WB pipeline), 2-line WB mode.
	 * mdp5_ctl.c:148-176 set_ctl_op(); MDP5_CTL_OP_MODE(MODE_WB_2_LINE). */
	sweet_mdp_wr(m, MSM8998_MDP_BASE + ctl_base + MDP5_CTL_OP, MDP5_CTL_OP_MODE(2u));
	sweet_mdp_wr(m, MSM8998_MDP_BASE + ctl_base + MDP5_CTL_START, 1u);
}

/*
 * Public entry point.  Drop-in replacement for the memcpy loop:
 *
 *   SweetFramebuffer::present(), inner loop
 *     -> sweet_mdss_blit(&fb->fMdp, &blit)
 *
 * where blit.src_phys = (uint32_t)(src + s->offset + sy*bpr + sx*4)
 *                      truncated after the existing bounds checks, and
 *       blit.dst_phys = (uint32_t)(fPhysBase + dy*fRowBytes + dx*4).
 * The existing rectangle clipping code at SweetFramebuffer.cpp:352-392 stays
 * exactly as it is; only the per-row copy disappears.
 *
 * Returns 0 on success, -1 if the rectangle is unusable.
 */
static int
sweet_mdss_blit(struct sweet_mdp *m, const struct sweet_blit *b)
{
	if (b == NULL || b->w == 0 || b->h == 0) {
		return -1;
	}
	if (b->src_phys == 0 || b->dst_phys == 0) {
		return -1;
	}
	if (b->src_stride < b->w * 4u || b->dst_stride < b->w * 4u) {
		return -1;
	}

	sweet_blit_setup_pipe(m, MSM8998_DMA0_SSPP, b);
	sweet_blit_setup_lm(m, MSM8998_LM0, b);
	sweet_blit_setup_wb(m, b);
	sweet_blit_commit(m, MSM8998_CTL0,
	    MDP5_FLUSH_DMA0 | MDP5_FLUSH_LM0 | MDP5_FLUSH_WB | MDP5_FLUSH_CTL);

	return 0;
}

/*
 * ---------------------------------------------------------------------------
 * 4. What a real implementation still needs
 * ---------------------------------------------------------------------------
 *  a) Map MDP_BASE (0x90000 bytes) + VBIF (0x1040) as two IOKit memory ranges
 *     from the /arm-io/mdss_mdp node, and wire sweet_mdp.write/read to
 *     OSWriteLittleInt32/OSReadLittleInt32.
 *  b) Wait for completion.  The MDP raises GIC SPI 83 (msm8998-mdss.dtsi:20),
 *     and the WB block has its own done bit; the cheap interim is to poll
 *     WB_DST_ADDR_SW_STATUS (sde_hw_wb.c:45) or simply to trust the flush and
 *     delay one vsync, because present() already runs on a timer.
 *  c) Decide CONFIG/ownership: the bootloader's MDP state is unknown.  Read
 *     MDP_TOP + 0x0 (MDP_HW_VERSION, mdp5.xml.h:211-231) first: a non-zero
 *     major/minor proves the block is clocked and the mapping is right.  That
 *     single read is the go/no-go for the whole option.
 *  d) If SRC0_ADDR is not honoured (SMMU enforcing), fall back to allocating
 *     the source through an SMMU domain, or copy the surface once into a
 *     bounce buffer that the bootloader already mapped.
 *  e) Never enable this while the on-screen console is the kernel's text
 *     console: present() currently pauses it on the first swap
 *     (SweetFramebuffer.cpp:317-325).
 */

#endif /* __IOS_SWEET_MDSS_BLIT_SKELETON__ */
