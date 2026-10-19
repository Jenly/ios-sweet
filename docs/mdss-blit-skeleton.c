/*
 * mdss-blit-skeleton.c -- SKELETON ONLY. NOT TESTED, NOT WIRED IN, NOT BUILT.
 *
 * A register-level plan for a memory-to-memory 2D blit on the MSM8998
 * (Snapdragon 835) MDSS/MDP, written in the idiom of the ios-sweet XNU tree
 * (iokit/Kernel/arm/SweetFramebuffer.cpp).  It exists to make the hardware
 * sequence concrete and reviewable, NOT because it is known to work.
 *
 * Every register offset below is traced to a file that was actually read on
 * the build server; the citation is in the comment.  Anything that could not
 * be traced is marked  TODO(unverified)  and MUST be resolved by capturing a
 * register dump from a working Android boot of the same SoC before this code
 * is trusted (see the "verification recipe" in gpu-research.md, section 8).
 *
 * It would be compiled by adding it to iokit/conf/... and calling
 * sweet_mdss_blit() from AppleCLCD::present() instead of the memcpy loop at
 * SweetFramebuffer.cpp:393-399.  It is deliberately NOT wired in: nothing in
 * the tree references these symbols today.
 *
 * HARD PRECONDITIONS (all of them are *unproven* on this port):
 *   1. MMSS GDSC ("gdsc_mdss") can be turned on from XNU by poking the
 *      register, i.e. it is not owned by a secure/always-on power domain.
 *   2. The five MDSS clocks can be enabled by poking MMSS CC branch
 *      registers, i.e. the rate is already what the bootloader set (we do
 *      not program the PLL/RCG here).
 *   3. The MMSS SMMU (0x0CD00000) is left *unprogrammed/bypassed* by the
 *      UEFI boot chain, so MDSS DMA uses physical addresses.  Evidence that
 *      this is the boot-time state: the Linux driver explicitly does not
 *      attach the SMMU until the continuous-splash handoff is over because
 *      "it may still be working with phys addr" (mdss_mdp.c:1521-1526).
 *   4. Source and destination buffers are *physically contiguous*.
 *   5. The MDP is not already busy on the CTL/LM/WB pair chosen here.
 *      (CTL0+LM0+VIG0+INTF0 are assumed to be the bootloader's scanout path
 *      and are NOT touched; this file uses CTL1/LM1/DMA1/WB.)
 *
 * Copyright: same terms as the rest of the ios-sweet tree (GPL-2.0-ish
 * kernel glue; the register facts are from Qualcomm's GPL msm-4.4 tree).
 */

#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOMemoryMap.h>
#include <mach/mach_time.h>

/*
 * ---------------------------------------------------------------------------
 * 1. Addresses
 *
 * MDSS top-level register block: 0x0C900000, length 0x90000
 *   ... "reg = <0x0c900000 0x90000>" -- msm8998-mdss.dtsi:17
 * MDP register window = MDSS base + qcom,mdss-mdp-reg-offset (0x1000)
 *   ... msm8998-mdss.dtsi:58
 * VBIF: 0x0C9B0000, length 0x1040 -- msm8998-mdss.dtsi:17-19
 * Per-block offsets inside the MDP window (msm8998-mdss.dtsi):
 *   CTL0..4        0x2000 0x2200 0x2400 0x2600 0x2800   (:91)
 *   VIG0..3        0x5000 0x7000 0x9000 0x0B000         (:67)
 *   DMA0..3        0x25000 0x27000 0x29000 0x2B000      (:69)
 *   LM0..3         0x45000 0x46000 0x47000 0x4A000      (:93)
 *   DSPP0..1       0x55000 0x57000                      (:95)
 *   WB             0x66000   (named "WB_2" in :180)     (:96)
 *   INTF0..3       0x6B000 0x6B800 0x6C000 0x6C800      (:97)
 *   PingPong0..3   0x71000 0x71800 0x72000 0x72800      (:99)
 * MMSS clock controller: 0x0C8C0000, length 0x40000 -- msm8998.dtsi:948-950
 * MMSS SMMU:            0x0CD00000, length 0x40000 -- msm-arm-smmu-8998.dtsi:98-101
 * MDSS GDSC:            0x0C8C2304 (MMSS CC + 0x2304) -- msm-gdsc-8998.dtsi:116
 */
#define SWEET_MDSS_BASE            0x0C900000UL
#define SWEET_MDSS_LEN             0x90000UL
#define SWEET_MDP_OFF              0x1000UL        /* msm8998-mdss.dtsi:58 */
#define SWEET_MMSS_CC_BASE         0x0C8C0000UL
#define SWEET_MMSS_CC_LEN          0x40000UL

/* Absolute addresses used by this file (MDP window + block offset). */
#define SWEET_MDP_BASE             (SWEET_MDSS_BASE + SWEET_MDP_OFF)
#define SWEET_CTL1                 (SWEET_MDP_BASE + 0x2200UL)  /* dtsi:91 */
#define SWEET_DMA1                 (SWEET_MDP_BASE + 0x27000UL) /* dtsi:69 */
#define SWEET_LM1                  (SWEET_MDP_BASE + 0x46000UL) /* dtsi:93 */
#define SWEET_WB                   (SWEET_MDP_BASE + 0x66000UL) /* dtsi:96 */

/*
 * ---------------------------------------------------------------------------
 * 2. Clock / power registers
 *
 * CBCR offsets inside the MMSS CC block, from
 * include/dt-bindings/clock/msm-clocks-hwio-8998.h:
 *   MMSS_MDSS_AHB_CBCR      0x02308   (:354)
 *   MMSS_MDSS_AXI_CBCR      0x02310   (:355)
 *   MMSS_MDSS_MDP_CBCR      0x0231C   (:373)
 *   MMSS_MDSS_MDP_LUT_CBCR  0x02320   (:374)
 *   MMSS_MDSS_VSYNC_CBCR    0x02328   (:378)
 *   MMSS_MDP_CMD_RCGR       0x02040   (:259)   <- source RCG, NOT touched
 * Branch enable is bit 0 of the CBCR: clock-local2.c:68
 *   "#define CBCR_BRANCH_ENABLE_BIT BIT(0)"
 * GDSC power-on is bit 31 of the GDSCR: gdsc.c:29 "#define PWR_ON_MASK BIT(31)"
 * The GDSC status bits are polled by poll_gdsc_status() (gdsc.c:87-118); the
 * exact status bit position is  TODO(unverified)  here on purpose -- read it
 * out of drivers/clk/msm/gdsc.c before enabling, do not guess.
 */
#define SWEET_CBCR_MDSS_AHB        (SWEET_MMSS_CC_BASE + 0x02308UL)
#define SWEET_CBCR_MDSS_AXI        (SWEET_MMSS_CC_BASE + 0x02310UL)
#define SWEET_CBCR_MDP             (SWEET_MMSS_CC_BASE + 0x0231CUL)
#define SWEET_CBCR_MDP_LUT         (SWEET_MMSS_CC_BASE + 0x02320UL)
#define SWEET_CBCR_VSYNC           (SWEET_MMSS_CC_BASE + 0x02328UL)
#define SWEET_GDSCR_MDSS           0x0C8C2304UL
#define SWEET_GDSC_PWR_ON          (1U << 31)       /* gdsc.c:29 */

/* ---------------------------------------------------------------------------
 * 3. MDP register offsets inside the MDP window
 *
 * All from drivers/video/fbdev/msm/mdss_mdp_hwio.h (line numbers given).
 */
/* MDSS top / MDP common */
#define MDP_HW_VERSION             0x0000   /* relative to MDSS base (hwio.h:61) */
#define MDP_INTR_EN                0x0010   /* hwio.h:66 */
#define MDP_INTR_STATUS            0x0014   /* hwio.h:67 */
#define MDP_INTR_CLEAR             0x0018   /* hwio.h:68 */
#define MDP_RT_NRT_VBIF_CLIENT_SEL 0x0190   /* hwio.h:78 */
#define MDP_CLK_CTRL               0x02BC   /* see mdp-settings 0x012bc, dtsi:130 */

/* CTL block (base SWEET_CTL1) */
#define CTL_LAYER(lm)              ((lm) * 0x004)   /* hwio.h:180-181 */
#define CTL_TOP                    0x014            /* hwio.h:189 */
#define CTL_FLUSH                  0x018            /* hwio.h:190 */
#define CTL_START                  0x01C            /* hwio.h:191 */

/* DMA/VIG SSPP source pipe (base SWEET_DMA1) */
#define SSPP_SRC_SIZE              0x000    /* hwio.h:241 */
#define SSPP_SRC_XY                0x008    /* hwio.h:243 */
#define SSPP_OUT_SIZE              0x00C    /* hwio.h:244 */
#define SSPP_OUT_XY                0x010    /* hwio.h:245 */
#define SSPP_SRC0_ADDR             0x014    /* hwio.h:246 */
#define SSPP_SRC_YSTRIDE0          0x024    /* hwio.h:250 */
#define SSPP_SRC_YSTRIDE1          0x028    /* hwio.h:251 */
#define SSPP_SRC_FORMAT            0x030    /* hwio.h:253 */
#define SSPP_SRC_UNPACK_PATTERN    0x034    /* hwio.h:254 */
#define SSPP_SRC_OP_MODE           0x038    /* hwio.h:255 */
#define SSPP_FETCH_CONFIG          0x048    /* hwio.h:293 */

/* Layer mixer (base SWEET_LM1) */
#define LM_OP_MODE                 0x000    /* hwio.h:457 */
#define LM_OUT_SIZE                0x004    /* hwio.h:458 */
#define LM_BORDER_COLOR_0          0x008    /* hwio.h:459 */
#define LM_BORDER_COLOR_1          0x010    /* hwio.h:460 */
#define LM_BLEND_OFFSET(stage)     (0x20 + ((stage) * 0x30))  /* hwio.h:462 */
#define LM_BLEND_OP                0x00     /* hwio.h:463 */
#define LM_BLEND_FG_ALPHA          0x04     /* hwio.h:464 */
#define LM_BLEND_BG_ALPHA          0x08     /* hwio.h:465 */

/* Writeback (base SWEET_WB) */
#define WB_DST_FORMAT              0x000    /* hwio.h:520 */
#define WB_DST_OP_MODE             0x004    /* hwio.h:521 */
#define WB_DST_PACK_PATTERN        0x008    /* hwio.h:522 */
#define WB_DST0_ADDR               0x00C    /* hwio.h:523 */
#define WB_DST_YSTRIDE0            0x01C    /* hwio.h:527 */
#define WB_OUT_SIZE                0x074    /* hwio.h:542 */
#define WB_ALPHA_X_VALUE           0x078    /* hwio.h:543 */
#define WB_DST_ADDR_SW_STATUS      0x2B0    /* hwio.h:549 */

/* CTL_TOP opmode values (hwio.h:195-202) */
#define CTL_OP_WB0_MODE            0x3      /* hwio.h:200 */
#define CTL_OP_WB1_MODE            0x4      /* hwio.h:201 */

/* Interrupt bits (hwio.h:93-95) */
#define INTR_WB_2_DONE             (1U << 4)    /* hwio.h:95 */
#define INTR_CLEAR_ALL_WB          (0x7U)       /* WB0|WB1|WB2 done bits */

/* Blend op bits (hwio.h:494-509) */
#define BLEND_FG_ALPHA_FG_CONST    (0 << 0)     /* hwio.h:494 */
#define BLEND_BG_ALPHA_BG_CONST    (1 << 8)     /* hwio.h:503 */

/*
 * ---------------------------------------------------------------------------
 * 4. Derived constants: BGRA8888 ('BGRA' = 0x42475241, what the iOS side
 *    hands us -- SweetFramebuffer.cpp:83)
 *
 * Format entry: MDP_BGRA_8888, mdss_mdp_formats.h:320-322
 *   -> FMT_RGB_8888(..., alpha_en=1, e0=C1_B_Cb, e1=C0_G_Y, e2=C2_R_Cr,
 *      e3=C3_ALPHA)
 * FMT_RGB_8888 (mdss_mdp_formats.h:75-94) gives: fetch_planes=0 (interleaved),
 *   unpack_tight=1, unpack_align_msb=0, unpack_count=4, bpp=4,
 *   bits[C0..C3]=COLOR_8BIT=3.
 *
 * SRC_FORMAT is built by mdss_mdp_format_setup(), mdss_mdp_pipe.c:2123-2155:
 *   (chroma<<23)|(planes<<19)|(b3<<6)|(b2<<4)|(b1<<2)|(b0<<0)
 *   | ((unpack_count-1)<<12) | (unpack_tight<<17) | (unpack_align_msb<<18)
 *   | ((bpp-1)<<9)
 * =>  0xFF | 0x3000 | 0x20000 | 0x600  = 0x000236FF
 *
 * DST_FORMAT is built by mdss_mdp_writeback_format_setup(),
 * mdss_mdp_intf_writeback.c:444-479, which is the same encoding PLUS
 *   BIT(8) DSTC3_EN when bits[C3_ALPHA] or alpha_enable is set
 * => 0x000236FF | 0x100 = 0x000237FF
 *
 * PACK_PATTERN (writeback, intf_writeback.c:468-471; and the pipe's "unpack"
 * at mdss_mdp_pipe.c:2140-2141) = (e3<<24)|(e2<<16)|(e1<<8)|e0
 * => (3<<24)|(2<<16)|(0<<8)|1 = 0x03020001
 *
 * These three values are DERIVED by applying the driver's own encoding code
 * to the driver's own format table -- they were not read back from hardware.
 */
#define SWEET_MDP_FMT_BGRA8888_SRC  0x000236FFU
#define SWEET_MDP_FMT_BGRA8888_DST  0x000237FFU
#define SWEET_MDP_FMT_BGRA8888_PAT  0x03020001U

/* SSPP fetch config reset value, hwio.h:50.  For *linear* formats the driver
 * does not write this register at all (mdss_mdp_pipe.c:2158-2168 only writes
 * it when fetch_mode != LINEAR), so neither do we. */
#define SWEET_MDP_FETCH_CONFIG_RESET 0x00000087U

/* CTL_LAYER pipe->stage encoding.
 * mdss_mdp_ctl.c:46-61 mdp_pipe_hwio[] gives, per pipe, a {start,len,shift}
 * bitfield inside CTL_LAYER<n>; the value written is the *stage*:
 *   DMA1 -> { start 21, len 3, shift 0 }            (mdss_mdp_ctl.c:56)
 * Stage index values are enum mdss_mdp_stage_index (hwio.h:442-453):
 *   UNUSED 0, BASE 1, STAGE_0 2, STAGE_1 3, ...
 * Which layer-mixer number the WB mixer is programmed through depends on
 * MDSS_CAPS_MIXER_1_FOR_WB: hw mixer = 1 (if the cap is set) else 3
 * (mdss_mdp_ctl.c:4875-4884), and CTL_LAYER(n) for the WB mixer is
 * CTL_LAYER(3)/CTL_LAYER(4) per the comment at mdss_mdp_ctl.c:4863-4870.
 *  TODO(unverified): which of these two applies to msm8998 -- read
 *  mdata->mdss_caps_map (mdss_mdp_hw_rev_caps_init, mdss_mdp.c:2027+) or,
 *  much better, dump CTL_LAYER registers from a working Android boot.
 * The macro below is the safe/no-WB-cap variant (hw mixer 3).
 */
#define SWEET_MDP_STAGE_0          2
#define SWEET_CTL_LAYER_WB_REG     CTL_LAYER(3)
#define SWEET_CTL_LAYER_WB_VAL     ((uint32_t)SWEET_MDP_STAGE_0 << 21)

/* CTL_FLUSH bits: BIT(pipe->flush_bit) per pipe (mdss_mdp_ctl.c:5446,
 * table mdss_mdp_ctl.c:46-61 -> DMA1 = bit 12), BIT(mixer hw num + 6) for the
 * layer mixer (mdss_mdp_ctl.c:5189) and BIT(16) for WB
 * (mdss_mdp_intf_writeback.c:1053). */
#define SWEET_FLUSH_DMA1           (1U << 12)
#define SWEET_FLUSH_LM(hw)         (1U << ((hw) < 5 ? 6 + (hw) : 20))
#define SWEET_FLUSH_WB             (1U << 16)

/*
 * ---------------------------------------------------------------------------
 * 5. The blit
 */

typedef struct sweet_mdss {
	volatile uint32_t *mdss;        /* MDSS top (0x0C900000) */
	volatile uint32_t *mdp;         /* MDP window (0x0C901000) */
	volatile uint32_t *ctl1;
	volatile uint32_t *dma1;
	volatile uint32_t *lm1;
	volatile uint32_t *wb;
	volatile uint32_t *mmss_cc;
	bool               powered;
} sweet_mdss_t;

static inline void
sweet_io_write(volatile uint32_t *base, uint32_t off, uint32_t val)
{
	base[off / 4] = val;
	__asm__ __volatile__("dsb sy" ::: "memory");
}

static inline uint32_t
sweet_io_read(volatile uint32_t *base, uint32_t off)
{
	uint32_t v = base[off / 4];
	__asm__ __volatile__("dsb sy" ::: "memory");
	return v;
}

/* Map a physical range read/write.  Same mechanism the existing tree already
 * uses for the panel memory: IOMemoryDescriptor::withPhysicalAddress()
 * (SweetSurface.cpp:242) followed by map(). */
static volatile uint32_t *
sweet_map_phys(uint64_t phys, uint64_t len)
{
	IOMemoryDescriptor *md;
	IOMemoryMap        *map;
	volatile uint32_t  *va;

	md = IOMemoryDescriptor::withPhysicalAddress(phys, len, kIODirectionOutIn);
	if (md == NULL) {
		return NULL;
	}
	map = md->map();
	md->release();
	if (map == NULL) {
		return NULL;
	}
	va = (volatile uint32_t *)map->getVirtualAddress();
	/* The map is intentionally leaked for the lifetime of the driver;
	 * a real implementation keeps the IOMemoryMap and releases it in stop(). */
	return va;
}

/*
 * Turn on the MMSS power domain and the five MDSS clocks.
 *
 * This is the part of the whole exercise with the highest chance of hanging
 * the machine, which is why it is isolated here: if the GDSC never reports
 * "enabled", every later register access faults or hangs the AXI bus.
 */
static bool
sweet_mdss_power_on(sweet_mdss_t *d)
{
	uint32_t v;
	int      i;

	/* 1. MMSS GDSC.  gdsc.c:29 PWR_ON_MASK = BIT(31); gdsc.c:87-118 polls
	 *    a status field with udelay(1) between reads.
	 *     TODO(unverified): the status bit position.  Until it is known,
	 *     this code must not run on a device we care about. */
	v = sweet_io_read(d->mmss_cc, SWEET_GDSCR_MDSS - SWEET_MMSS_CC_BASE);
	sweet_io_write(d->mmss_cc, SWEET_GDSCR_MDSS - SWEET_MMSS_CC_BASE,
	    v | SWEET_GDSC_PWR_ON);
	for (i = 0; i < 1000; i++) {
		IODelay(1);
		v = sweet_io_read(d->mmss_cc, SWEET_GDSCR_MDSS - SWEET_MMSS_CC_BASE);
		if ((v & SWEET_GDSC_PWR_ON) && (v & (1U << 16)) != 0) {
			break;      /*  TODO(unverified): bit 16 is a guess */
		}
	}
	if (i == 1000) {
		IOLog("SWEET MDP: gdsc_mdss did not come up (0x%08x)\n", v);
		return false;
	}

	/* 2. Clocks.  We do not touch MMSS_MDP_CMD_RCGR (0x2040): the rate the
	 *    bootloader left is assumed good enough for a 1080p blit.  We only
	 *    ungate the branches: bit 0 of each CBCR (clock-local2.c:68). */
	static const uint32_t cbcr[] = {
		SWEET_CBCR_MDSS_AHB, SWEET_CBCR_MDSS_AXI, SWEET_CBCR_MDP,
		SWEET_CBCR_MDP_LUT,  SWEET_CBCR_VSYNC,
	};
	for (i = 0; i < (int)(sizeof(cbcr) / sizeof(cbcr[0])); i++) {
		uint32_t off = cbcr[i] - SWEET_MMSS_CC_BASE;
		v = sweet_io_read(d->mmss_cc, off);
		sweet_io_write(d->mmss_cc, off, v | 1U);
	}

	/* 3. Make sure the MDP's AXI clients are not clock-gated by the
	 *    per-pipe clock-control fields.  The DT's qcom,mdp-settings replay
	 *    value for 0x012bc is 0x00cccccc (msm8998-mdss.dtsi:130), i.e. the
	 *    five 4-bit fields at 0x2BC are all "always on" (0xc).  The Linux
	 *    driver applies that table at init; XNU has to as well. */
	sweet_io_write(d->mdp, MDP_CLK_CTRL, 0x00CCCCCC);

	d->powered = true;
	return true;
}

/*
 * Program one DMA1 -> LM -> WB blit.  No scaling, no rotation, no alpha
 * blending: a straight 32bpp copy, which is exactly what the current
 * CPU memcpy at SweetFramebuffer.cpp:393-399 does.
 *
 * src_phys / dst_phys must be physical addresses of contiguous buffers.
 */
static bool
sweet_mdss_blit(sweet_mdss_t *d,
    uint64_t src_phys, uint32_t src_stride, uint32_t src_x, uint32_t src_y,
    uint64_t dst_phys, uint32_t dst_stride, uint32_t dst_x, uint32_t dst_y,
    uint32_t w, uint32_t h)
{
	uint32_t v;
	int      i;

	if (!d->powered && !sweet_mdss_power_on(d)) {
		return false;
	}

	/* 0. The WB block must not be busy with a previous kickoff
	 *    (mdss_mdp_intf_writeback.c:1014-1018 refuses to start twice). */
	if (sweet_io_read(d->wb, WB_DST_ADDR_SW_STATUS) != 0) {
		/*  TODO(unverified): this register is written with the secure
		 *  flag by the driver (intf_writeback.c:1054); reading it as a
		 *  busy indicator is *not* what the driver does.  A real
		 *  implementation tracks completion through the interrupt at
		 *  MDP+0x14 (INTR_WB_2_DONE) or through CTL_START going idle. */
	}

	/* 1. Source pipe: linear BGRA8888, no scaling.
	 *    Register write order follows mdss_mdp_pipe_setup() /
	 *    mdss_mdp_format_setup() (mdss_mdp_pipe.c:2097-2196) and
	 *    mdss_mdp_src_addr_setup() (mdss_mdp_pipe.c:2283-2286). */
	sweet_io_write(d->dma1, SSPP_SRC_SIZE, (h << 16) | w);      /* packed wh */
	sweet_io_write(d->dma1, SSPP_SRC_XY, (src_y << 16) | src_x);
	sweet_io_write(d->dma1, SSPP_OUT_SIZE, (h << 16) | w);
	sweet_io_write(d->dma1, SSPP_OUT_XY, 0);
	sweet_io_write(d->dma1, SSPP_SRC0_ADDR,
	    (uint32_t)(src_phys + (uint64_t)src_y * src_stride +
	    (uint64_t)src_x * 4));
	sweet_io_write(d->dma1, SSPP_SRC_YSTRIDE0, src_stride);     /* [15:0] */
	sweet_io_write(d->dma1, SSPP_SRC_YSTRIDE1, 0);
	sweet_io_write(d->dma1, SSPP_SRC_FORMAT, SWEET_MDP_FMT_BGRA8888_SRC);
	sweet_io_write(d->dma1, SSPP_SRC_UNPACK_PATTERN, SWEET_MDP_FMT_BGRA8888_PAT);
	sweet_io_write(d->dma1, SSPP_SRC_OP_MODE, 0);   /* no flip, no scaler */

	/* 2. Writeback destination (mdss_mdp_writeback_format_setup(),
	 *    mdss_mdp_intf_writeback.c:502-509 -- same order).
	 *
	 *    NOTE the two different "opmode" values, which are easy to
	 *    confuse:  WB_DST_OP_MODE gets ctx->opmode, which the line/wfd
	 *    prepare path sets to 0 (intf_writeback.c:636); only YUV, UBWC or
	 *    rotator add bits to it (:423-442, :487-494, :690-712).
	 *    CTL_TOP (step 5) gets ctl->opmode = MDSS_MDP_CTL_OP_WB0_MODE = 3
	 *    (hwio.h:200), chosen from the mixer number at
	 *    mdss_mdp_wfd.c:259-266. */
	sweet_io_write(d->wb, WB_ALPHA_X_VALUE, 0xFF);  /* intf_writeback.c:502 */
	sweet_io_write(d->wb, WB_DST_FORMAT, SWEET_MDP_FMT_BGRA8888_DST);
	sweet_io_write(d->wb, WB_DST_OP_MODE, 0);       /* intf_writeback.c:636 */
	sweet_io_write(d->wb, WB_DST_PACK_PATTERN, SWEET_MDP_FMT_BGRA8888_PAT);
	sweet_io_write(d->wb, WB_DST_YSTRIDE0, dst_stride);
	sweet_io_write(d->wb, WB_OUT_SIZE, (h << 16) | w);
	sweet_io_write(d->wb, WB_DST0_ADDR,
	    (uint32_t)(dst_phys + (uint64_t)dst_y * dst_stride +
	    (uint64_t)dst_x * 4));

	/* 3. Layer mixer: output size and an opaque stage 0.
	 *    LM_OUT_SIZE is written by mdss_mdp_mixer_setup()
	 *    (mdss_mdp_ctl.c:5055); the blend registers per stage are written
	 *    at mdss_mdp_ctl.c:5176-5181 with the op computed at 5105-5164.
	 *    For an opaque pipe the driver uses
	 *    FG_ALPHA_FG_CONST | BG_ALPHA_BG_CONST with fg=0xFF, bg=0x00.
	 *     TODO(unverified): the stage index.  mdss_mdp_ctl.c:5103-5104
	 *    shows stage > 3 shifts by LM_BLEND_STAGE4 (0x150), so the index is
	 *    a small ordinal; stage 0 (enum value 2) is assumed here. */
	sweet_io_write(d->lm1, LM_OUT_SIZE, (h << 16) | w);
	sweet_io_write(d->lm1, LM_OP_MODE, 0);
	sweet_io_write(d->lm1, LM_BLEND_OFFSET(SWEET_MDP_STAGE_0) + LM_BLEND_OP,
	    BLEND_FG_ALPHA_FG_CONST | BLEND_BG_ALPHA_BG_CONST);
	sweet_io_write(d->lm1, LM_BLEND_OFFSET(SWEET_MDP_STAGE_0) + LM_BLEND_FG_ALPHA,
	    0xFF);
	sweet_io_write(d->lm1, LM_BLEND_OFFSET(SWEET_MDP_STAGE_0) + LM_BLEND_BG_ALPHA,
	    0x00);
	sweet_io_write(d->lm1, LM_BORDER_COLOR_0, 0);   /* ctl.c:5200-5203 */
	sweet_io_write(d->lm1, LM_BORDER_COLOR_1, 0);

	/* 4. Bind DMA1 to the WB mixer's stage 0 (CTL_LAYER<n>).
	 *    mdss_mdp_ctl.c:4823-4847 (masks) -> :4928-4929 (write). */
	sweet_io_write(d->ctl1, SWEET_CTL_LAYER_WB_REG, SWEET_CTL_LAYER_WB_VAL);

	/* 5. CTL_TOP: tell the CTL where to send the mixer output.
	 *    mdss_mdp_ctl.c:5887 writes ctl->opmode here; the opmode value
	 *    is chosen at mdss_mdp_wfd.c:259-266 and is
	 *    MDSS_MDP_CTL_OP_WB0_MODE = 0x3 (hwio.h:200) when the assigned
	 *    mixer is MDSS_MDP_WB_LAYERMIXER0.  The "& 0xF" of that value is
	 *    the writeback selector: mem_sel = (opmode & 0xF) - 1
	 *    (mdss_mdp_intf_writeback.c:1084-1087), i.e. 0x3 -> wb_ctx_list[2]
	 *    = WRITEBACK_TYPE_LINE (intf_writeback.c:96-103).
	 *     TODO(unverified): that the mixer really is WB_LAYERMIXER0 here --
	 *    if it is WB_LAYERMIXER1 the value becomes 0x4 and the CTL_LAYER
	 *    register below must change too. */
	sweet_io_write(d->ctl1, CTL_TOP, CTL_OP_WB0_MODE);

	/* 6. Flush the blocks we touched, then start.
	 *    Flush bits: DMA1 BIT(12), LM BIT(6+hw), WB BIT(16)
	 *    (intf_writeback.c:1052-1055, mdss_mdp_ctl.c:5189, :5446). */
	v = SWEET_FLUSH_DMA1 | SWEET_FLUSH_WB | SWEET_FLUSH_LM(3);
	sweet_io_write(d->ctl1, CTL_FLUSH, v);
	sweet_io_write(d->ctl1, CTL_START, 1);      /* intf_writeback.c:1069 */

	/* 7. Wait for WB done.  The driver waits on the WB interrupt
	 *    (intf_writeback.c:1059 enables MDSS_MDP_IRQ_TYPE_WB_ROT_COMP and
	 *    mdss_mdp_wb_wait4comp() sleeps on a completion, :922-999); XNU has
	 *    no MDSS interrupt handler yet, so poll MDP+0x14 instead.
	 *    IRO 83 is the MDP's SPI (msm8998-mdss.dtsi:20). */
	for (i = 0; i < 200; i++) {                 /* 200 x 100us = 20 ms */
		v = sweet_io_read(d->mdp, MDP_INTR_STATUS);
		if (v & INTR_WB_2_DONE) {
			break;
		}
		IODelay(100);
	}
	sweet_io_write(d->mdp, MDP_INTR_CLEAR, INTR_CLEAR_ALL_WB);
	if (i == 200) {
		IOLog("SWEET MDP: blit timed out, INTR_STATUS 0x%08x CTL_START %u\n",
		    v, sweet_io_read(d->ctl1, CTL_START));
		return false;
	}
	return true;
}

/*
 * ---------------------------------------------------------------------------
 * 6. How this replaces the memcpy in SweetFramebuffer::present()
 *
 * Today (SweetFramebuffer.cpp:393-399):
 *
 *     if (src != 0 && bpe == 4) {
 *             for (int32_t y = 0; y < h; y++) {
 *                     memcpy((void *)(fPixels + (uint64_t)(dy + y) * fRowBytes +
 *                         (uint64_t)dx * 4),
 *                         (const void *)(src + s->offset +
 *                         (uint64_t)(sy + y) * s->bytesPerRow + (uint64_t)sx * 4),
 *                         (size_t)w * 4);
 *             }
 *     }
 *
 * The hardware equivalent is one call:
 *
 *     sweet_mdss_blit(&g_mdss,
 *         src_phys_of(s) + s->offset, s->bytesPerRow, sx, sy,
 *         fPhys,                      fRowBytes,       dx, dy, w, h);
 *
 * ...where src_phys_of() is the *physical* address of the surface's backing
 * store.  That is the hard part, and it is not a driver problem:
 *
 *   - SweetSurface allocates its pixels with
 *       IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task,
 *           kIODirectionInOut | kIOMemoryKernelUserShared, allocSize, page_size)
 *     (SweetSurface.cpp:691-692), i.e. page-based and NOT physically
 *     contiguous.  There is no SMMU driver in the XNU tree today
 *     (`grep -ril smmu iokit/Kernel/arm/` is empty), so the MDP can only be
 *     pointed at physical addresses.
 *   - XNU can allocate contiguous memory (kIOMemoryPhysicallyContiguous +
 *     kIOMemoryMapperNone -> IOKernelAllocateWithPhysicalRestrict(),
 *     iokit/Kernel/IOBufferMemoryDescriptor.cpp:302-326), but a 1080x2400
 *     BGRA surface is 10,368,000 bytes = ~633 pages of 16 KB and the
 *     allocation can fail on a fragmented boot.  That has to be measured,
 *     not assumed.
 *   - The panel memory (fPhys, from PE_Video) is a fixed physical window the
 *     display controller is already scanning out, so the *destination* is
 *     fine as it stands.
 *
 * If contiguous surfaces turn out to be unobtainable, the alternatives are
 * (a) one blit per row from a staging buffer (pointless), (b) keep the CPU
 * copy, or (c) write an MMSS SMMU driver and map surfaces into a context
 * bank -- the "correct" answer and a much bigger project (see the report).
 */
