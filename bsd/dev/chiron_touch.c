/*
 * Xiaomi Mi MIX 2 ("chiron", MSM8998) touchscreen: Synaptics RMI4 over BLSP I2C5.
 *
 * This is a second, independent touch driver next to bsd/dev/sweet_touch.c.
 * They look similar from the outside (a kernel thread polls an IC and feeds
 * sweet_hid_touch()) but share almost no hardware:
 *
 *   sweet  (SM7150)  QUP *GENI* SE7 at 0x00a84000, needs ABL-loaded I2C
 *                    firmware, ICs: Goodix GT9896 / FocalTech  54:52
 *   chiron (MSM8998) QUP *v2* BLSP1 QUP5 at 0x00c179000 -- a hardware I2C
 *                    state machine with no firmware at all, ICs: Synaptics
 *                    DSX (S3508/S3330/TD4322) at 0x20 / FocalTech ft5x46 at 0x38
 *
 * Every register address, bit position and protocol step below is taken from
 * the vendor Linux kernel (LineageOS android_kernel_xiaomi_msm8998), which is
 * the only public documentation for these blocks:
 *
 *   drivers/i2c/busses/i2c-qup.c                     QUP v2 register map, tags
 *   drivers/pinctrl/qcom/pinctrl-msm8998.c           TLMM register layout
 *   drivers/pinctrl/qcom/pinctrl-msm.c               mux/pull/drive bit handling
 *   drivers/clk/msm/clock-gcc-8998.c                 GCC clock enable/divider
 *   include/dt-bindings/clock/msm-clocks-hwio-8998.h GCC register offsets
 *   drivers/input/touchscreen/synaptics_dsx/         RMI4 protocol, PDT scan
 *   arch/arm64/boot/dts/qcom/xiaomi/chiron-msm8998-mtp.dtsi
 *                                                    which bus/pins/supplies
 *
 * The device tree facts (from the chiron FDT that edk2-msm8998 passes):
 *   i2c@c179000 (BLSP1 QUP5), 400 kHz, SDA=gpio87 SCL=gpio88 (function 3)
 *   synaptics_dsi_i2c@20, irq-gpio = gpio125, reset-gpio = gpio89, both GPIO function
 *   supplies: vdd(regulator-l6), avdd(disp_vddts), disp(regulator-l14), lab/ibb
 *
 * STATUS: never run on hardware. It is written to be safe rather than fast:
 * every stage verifies what it read before it proceeds, everything is logged
 * with a "CHIRON TOUCH:" prefix, and any surprise ends the thread instead of
 * touching a register that might belong to something else. Boot-arg
 * chirontouchdelay=N (seconds, default 45) delays the first bus access so a
 * hang cannot stop the GUI; chirontouch=0 disables the driver entirely.
 */
#if defined(ARM64_BOARD_CONFIG_SWEET)
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <kern/sched_prim.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <pexpert/SweetBoard.h>
#include "chiron_touch_proto.h"

extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
extern kern_return_t thread_terminate(thread_t);
extern void sweet_hid_touch(int finger, int down, int x, int y);
void chiron_touch_init(void);

/* ---------------------------------------------------------------- hardware */

/* TLMM (pinctrl@03400000): pin n lives at base + 0x1000*n */
#define TLMM_PA           0x03400000u
#define TLMM_SIZE         0x01000000u
#define TLMM_CTL(n)       ((n) * 0x1000u)
#define TLMM_IO(n)        ((n) * 0x1000u + 0x4u)
#define TLMM_INTR_CFG(n)  ((n) * 0x1000u + 0x8u)
#define TLMM_INTR_STATUS(n) ((n) * 0x1000u + 0xcu)
/* ctl register bit fields, from pinctrl-msm8998.c PINGROUP() */
#define TLMM_PULL_SHIFT   0
#define TLMM_MUX_SHIFT    2
#define TLMM_MUX_MASK     0x7u
#define TLMM_DRV_SHIFT    6
#define TLMM_OE_BIT       9
/* io register */
#define TLMM_IO_IN        (1u << 0)
#define TLMM_IO_OUT       (1u << 1)
/* intr config register */
#define TLMM_INTR_EN      (1u << 0)
#define TLMM_INTR_POL     (1u << 1)
#define TLMM_INTR_DET     (3u << 2)
#define TLMM_DET_LEVEL_HIGH (3u << 2)   /* detection=3, polarity=1 -> level high */
#define TLMM_INTR_RAW     (1u << 4)
#define TLMM_INTR_TARGET  (0xe0u)       /* [7:5]; 3 = kpss (APSS) */

/*
 * Function index written into the mux field. msm_pinmux_set_mux() writes the
 * index *within that pin's own function list*, not the global mux enum:
 *   gpio87/88: PINGROUP(87, EAST, blsp_spi5, blsp_uart2_b, blsp_i2c5, ...)
 *              -> 0 gpio, 1 blsp_spi5, 2 blsp_uart2_b, 3 blsp_i2c5
 *   gpio89, gpio125: first entry is always gpio -> 0
 */
#define TLMM_FN_GPIO      0u
#define TLMM_FN_I2C5      3u

#define PIN_I2C_SDA       87
#define PIN_I2C_SCL       88
#define PIN_TS_RESET      89
#define PIN_TS_IRQ        125

/* BLSP1 QUP5 = i2c@c179000, reg <0xc179000 0x600> */
#define QUP5_PA           0x00c179000u
#define QUP5_SIZE         0x600u

/* GCC (qcom,gcc@100000), offsets from msm-clocks-hwio-8998.h */
#define GCC_PA            0x00100000u
#define GCC_SIZE          0x00180000u
#define GCC_BLSP1_BCR        0x17000u
#define GCC_BLSP1_AHB_CBCR   0x17004u
#define GCC_APCS_ENA_VOTE    0x52004u
#define GCC_VOTE_BLSP1_AHB   (1u << 17)
#define GCC_QUP5_I2C_CBCR    0x21008u
#define GCC_QUP5_I2C_CMD_RCGR 0x21020u   /* +0x4 = CFG_RCGR */
#define GCC_CBCR_ENABLE      (1u << 0)
#define GCC_CBCR_HALT        (1u << 31)
#define GCC_BCR_BLSP1        (1u << 0)
#define CMD_RCGR_ROOT_EN     (1u << 1)
#define CMD_RCGR_UPDATE      (1u << 0)
#define CMD_RCGR_ROOT_STATUS (1u << 31)
#define CFG_RCGR_DIV_MASK    (0x1fu << 0)
#define CFG_RCGR_SRC_MASK    (0x7u << 8)
#define CFG_RCGR_SRC_CXO     0u          /* cxo_clk_src_source_val */
#define CFG_RCGR_SRC_GPLL0   1u          /* gpll0_out_main_source_val */

/* QUP v2 register map, from i2c-qup.c */
#define QUP_CONFIG        0x000u
#define QUP_STATE         0x004u
#define QUP_IO_MODE       0x008u
#define QUP_SW_RESET      0x00cu
#define QUP_OPERATIONAL   0x018u
#define QUP_ERROR_FLAGS   0x01cu
#define QUP_ERROR_FLAGS_EN 0x020u
#define QUP_HW_VERSION    0x030u
#define QUP_MX_OUTPUT_CNT 0x100u
#define QUP_OUT_FIFO_BASE 0x110u
#define QUP_MX_WRITE_CNT  0x150u
#define QUP_MX_INPUT_CNT  0x200u
#define QUP_MX_READ_CNT   0x208u
#define QUP_IN_FIFO_BASE  0x218u
#define QUP_I2C_CLK_CTL   0x400u
#define QUP_I2C_STATUS    0x404u

#define QUP_STATE_MASK    0x3u            /* bits [1:0] read back the state */
#define QUP_STATE_RESET   0u
#define QUP_STATE_RUN     1u
#define QUP_STATE_PAUSE   3u
#define QUP_STATE_VALID   (1u << 2)
#define QUP_STATE_I2C_MST (1u << 4)
#define QUP_CONFIG_I2C    ((2u << 8) | 15u)  /* I2C_MINI_CORE | I2C_N_VAL */
#define QUP_IO_REPACK     ((1u << 14) | (1u << 15))
#define QUP_IO_OUT_BLK    (1u << 10)
#define QUP_IO_IN_BLK     (1u << 12)
#define QUP_OP_OUT_NOT_EMPTY (1u << 4)
#define QUP_OP_IN_NOT_EMPTY  (1u << 5)
#define QUP_OP_OUT_FULL      (1u << 6)
#define QUP_OP_OUT_SVC       (1u << 8)
#define QUP_OP_IN_SVC        (1u << 9)
#define QUP_OP_OUT_DONE      (1u << 10)
#define QUP_OP_IN_DONE       (1u << 11)
#define QUP_ERR_NACK         (1u << 3)
#define QUP_STATUS_BUSY      (1u << 5)   /* I2C_STATUS_BUS_ACTIVE */
#define QUP_TAG_START     (1u << 8)
#define QUP_TAG_DATA      (2u << 8)
#define QUP_TAG_STOP      (3u << 8)
#define QUP_TAG_REC       (4u << 8)
#define QUP_MSW_SHIFT     16u            /* I2C_N_VAL + 1 */

/* Synaptics RMI4, from synaptics_dsx_core.h / _i2c.c / _core.c */
#define DSX_ADDR          0x20u
#define FTS_ADDR          0x38u
#define RMI_PAGE_SELECT   0xffu
#define PDT_START         0x00e9u
#define PDT_END           0x00d0u
#define PDT_ENTRY_SIZE    6u
#define SYNAPTICS_RMI4_F01 0x01u
#define SYNAPTICS_RMI4_F12 0x12u

struct rmi_fn_desc {
	uint8_t query_base, cmd_base, ctrl_base, data_base;
	uint8_t intr_src_count, fn_number;
};

/* F12 finger data block, 8 bytes (synaptics_rmi4_f12_finger_data) */
#define RMI_F12_FINGER_LEN 8u
#define RMI_MAX_FINGERS    10u

#define TOUCH_MAX          RMI_MAX_FINGERS
#define POLL_MS            10u
#define CT_MAX_WRITE       64u       /* longest single I2C write we issue */

/* ---------------------------------------------------------------- state */

struct chiron_touch_state {
	vm_offset_t tlmm, qup, gcc;
	int started;
	int dead;
	int kind;                     /* 0 = none, 1 = synaptics, 2 = focaltech */
	/* presented panel size, from the ADT (px); the IC reports in panel px */
	uint32_t panel_w, panel_h;
	/* RMI4 layout discovered from the PDT */
	uint16_t f01_query, f01_data;
	uint16_t f12_query, f12_ctrl, f12_data;
	uint32_t f12_fingers;
	uint32_t f12_finger_len;
	uint8_t page;                 /* current RMI4 page */
	/* contact tracking */
	uint32_t active;
	int x[TOUCH_MAX], y[TOUCH_MAX];
};

static struct chiron_touch_state ct;

/* ---------------------------------------------------------------- helpers */

static uint32_t
ct_rd(vm_offset_t base, uint32_t off)
{
	return *(volatile uint32_t *)(base + off);
}

static void
ct_wr(vm_offset_t base, uint32_t off, uint32_t val)
{
	*(volatile uint32_t *)(base + off) = val;
}

/* Sleep channel for this driver's waits (the pattern bsd/dev/sweet_touch.c uses). */
static int ct_sleep_chan;

/*
 * Sleep without busy-waiting: the startup delay is tens of seconds, and the
 * QUP polls can add up, so spin-waiting here would burn a CPU for the whole
 * boot. IOSleep() is an IOKit function and not available to a BSD driver;
 * clock_interval_to_deadline + thread_block is what sweet_touch.c does.
 */
static void
ct_wait_ms(unsigned ms)
{
	uint64_t deadline;

	clock_interval_to_deadline(ms, NSEC_PER_MSEC, &deadline);
	(void)assert_wait_deadline(&ct_sleep_chan, THREAD_UNINT, deadline);
	thread_block(THREAD_CONTINUE_NULL);
}

/* ------------------------------------------------------------- QUP v2 I2C */

/*
 * Wait for the controller to leave a transient state. Returns 0 on success.
 * The timeouts are generous because this runs once, not in a hot loop.
 */
static int
qup_poll(vm_offset_t qup, uint32_t reg, uint32_t mask, uint32_t want, unsigned ms)
{
	for (unsigned i = 0; i < ms * 100u; i++) {
		if ((ct_rd(qup, reg) & mask) == want) {
			return 0;
		}
		ct_wait_ms(1);
	}
	return ETIMEDOUT;
}

static int
qup_state(vm_offset_t qup, uint32_t state)
{
	if (qup_poll(qup, QUP_STATE, QUP_STATE_VALID, QUP_STATE_VALID, 10)) {
		return EIO;
	}
	ct_wr(qup, QUP_STATE, state);
	return qup_poll(qup, QUP_STATE, QUP_STATE_MASK, state, 10);
}

/*
 * Give the QUP an I2C bus frequency. dividend = fs_div per the vendor formula
 *     fs_div = ((src_clk / bus_clk) / 2) - 3,  hs_div = 3
 * With the 19.2 MHz CXO as source and 400 kHz: 19200000/400000/2 - 3 = 21.
 */
static void
qup_set_bus_clock(vm_offset_t qup, uint32_t bus_hz)
{
	uint32_t fs_div;

	if (bus_hz == 0) {
		bus_hz = 400000;
	}
	fs_div = ((19200000u / bus_hz) / 2u) - 3u;
	if (fs_div > 0xffu) {
		fs_div = 0xffu;
	}
	ct_wr(qup, QUP_I2C_CLK_CTL, (3u << 8) | (fs_div & 0xffu));
	printf("CHIRON TOUCH: i2c bus %u Hz -> clk_ctl %#x\n", bus_hz,
	    ct_rd(qup, QUP_I2C_CLK_CTL));
}

/*
 * One write transaction. buf holds the bytes after the address byte, i.e. for
 * a register write buf[0] is the register offset.
 */
static int
qup_write(vm_offset_t qup, uint8_t addr, const uint8_t *buf, unsigned len)
{
	uint32_t words[CT_MAX_WRITE / 2 + 2];
	unsigned nwords;
	int rc;

	if (len == 0 || len > 64) {
		return EINVAL;
	}
	nwords = ct_pack_i2c_write(addr, buf, len, words);
	ct_wr(qup, QUP_IO_MODE, QUP_IO_REPACK);
	ct_wr(qup, QUP_MX_WRITE_CNT, len + 1);

	rc = qup_state(qup, QUP_STATE_RUN);
	if (rc) {
		return rc;
	}
	rc = qup_state(qup, QUP_STATE_PAUSE);
	if (rc) {
		return rc;
	}
	for (unsigned i = 0; i < nwords; i++) {
		ct_wr(qup, QUP_OUT_FIFO_BASE, words[i]);
	}
	rc = qup_state(qup, QUP_STATE_RUN);
	if (rc) {
		return rc;
	}
	rc = qup_poll(qup, QUP_OPERATIONAL, QUP_OP_OUT_DONE, QUP_OP_OUT_DONE, 100);
	if (rc) {
		ct_wr(qup, QUP_SW_RESET, 1);
		return rc;
	}
	if (ct_rd(qup, QUP_ERROR_FLAGS) & QUP_ERR_NACK) {
		ct_wr(qup, QUP_ERROR_FLAGS, QUP_ERR_NACK);
		return EIO;
	}
	return 0;
}

/*
 * One read transaction. Writes the address byte (with STOP) and then reads len
 * bytes. The vendor driver uses a repeated start; a separate stop-then-start
 * read is accepted by RMI4 parts because the register pointer is latched by
 * the preceding write, and it keeps this driver to one simple code path.
 */
static int
qup_read(vm_offset_t qup, uint8_t addr, uint8_t *buf, unsigned len)
{
	int rc;

	if (len == 0 || len > CT_MAX_WRITE) {
		return EINVAL;
	}
	/* FIFO mode; "0 length means 256" never applies at <= CT_MAX_WRITE */
	ct_wr(qup, QUP_IO_MODE, QUP_IO_REPACK);
	ct_wr(qup, QUP_MX_READ_CNT, len);

	rc = qup_state(qup, QUP_STATE_RUN);
	if (rc) {
		return rc;
	}
	rc = qup_state(qup, QUP_STATE_PAUSE);
	if (rc) {
		return rc;
	}
	/* START | addr<<1 | R, then TAG_REC with the byte count */
	ct_wr(qup, QUP_OUT_FIFO_BASE, ct_pack_i2c_read(addr, len));
	rc = qup_state(qup, QUP_STATE_RUN);
	if (rc) {
		return rc;
	}
	rc = qup_poll(qup, QUP_OPERATIONAL, QUP_OP_IN_DONE, QUP_OP_IN_DONE, 100);
	if (rc) {
		ct_wr(qup, QUP_SW_RESET, 1);
		return rc;
	}
	if (ct_rd(qup, QUP_ERROR_FLAGS) & QUP_ERR_NACK) {
		ct_wr(qup, QUP_ERROR_FLAGS, QUP_ERR_NACK);
		return EIO;
	}
	for (unsigned i = 0; i < len; i += 4) {
		uint32_t word = ct_rd(qup, QUP_IN_FIFO_BASE);
		for (unsigned j = 0; j < 4 && i + j < len; j++) {
			buf[i + j] = (uint8_t)(word >> (8 * j));
		}
	}
	return 0;
}

/* ------------------------------------------------------------ RMI4 layer */

/* RMI4 over I2C: to reach an address, select its page first (0xFF = page). */
static int
rmi_set_page(uint8_t page)
{
	uint8_t buf[2];

	if (ct.page == page) {
		return 0;
	}
	buf[0] = RMI_PAGE_SELECT;
	buf[1] = page;
	if (qup_write(ct.qup, DSX_ADDR, buf, 2)) {
		return EIO;
	}
	ct.page = page;
	return 0;
}

static int
rmi_read(uint16_t addr, uint8_t *buf, unsigned len)
{
	uint8_t reg = (uint8_t)(addr & 0xffu);
	int rc;

	if ((rc = rmi_set_page((uint8_t)(addr >> 8))) != 0) {
		return rc;
	}
	if ((rc = qup_write(ct.qup, DSX_ADDR, &reg, 1)) != 0) {
		return rc;
	}
	return qup_read(ct.qup, DSX_ADDR, buf, len);
}

/*
 * There is deliberately no register write to the touch IC in this driver.
 * The vendor driver additionally programs the F12 "report enable" control
 * register; polling can read the data registers without it on the parts this
 * targets, and a wrong control offset on a real phone could switch the sensor
 * into a state we cannot get out of without a reboot. If a boot shows the F12
 * data registers never changing, adding that one write (F12 control register
 * 28, offset computed from the query-5 presence bitmap exactly like
 * ctrl_23_offset above) is the next step.
 */

/*
 * Walk the page description table and remember where F01 and F12 live.
 * PDT entries are 6 bytes: query, cmd, ctrl, data base, intr count, function.
 * Scanning stops at the first entry whose function number is 0.
 */
static int
rmi_scan_pdt(void)
{
	uint16_t addr;

	ct.f01_query = ct.f01_data = 0;
	ct.f12_query = ct.f12_ctrl = ct.f12_data = 0;

	for (addr = PDT_START; addr > PDT_END; addr -= PDT_ENTRY_SIZE) {
		struct rmi_fn_desc fd;
		uint8_t raw[PDT_ENTRY_SIZE];

		if (rmi_read(addr, raw, sizeof(raw))) {
			return EIO;
		}
		fd.query_base = raw[0];
		fd.cmd_base = raw[1];
		fd.ctrl_base = raw[2];
		fd.data_base = raw[3];
		fd.intr_src_count = raw[4];
		fd.fn_number = raw[5];
		if (fd.fn_number == 0) {
			break;
		}
		printf("CHIRON TOUCH: PDT %#x -> F%02x q%#x c%#x d%#x intr=%u\n",
		    addr, fd.fn_number, fd.query_base, fd.ctrl_base, fd.data_base,
		    fd.intr_src_count);
		switch (fd.fn_number) {
		case SYNAPTICS_RMI4_F01:
			ct.f01_query = fd.query_base;
			ct.f01_data = fd.data_base;
			break;
		case SYNAPTICS_RMI4_F12:
			ct.f12_query = fd.query_base;
			ct.f12_ctrl = fd.ctrl_base;
			ct.f12_data = fd.data_base;
			break;
		default:
			break;
		}
	}
	if (ct.f01_query == 0 || ct.f12_data == 0) {
		printf("CHIRON TOUCH: PDT incomplete (F01 %#x, F12 data %#x)\n",
		    ct.f01_query, ct.f12_data);
		return ENODEV;
	}
	return 0;
}

/*
 * F12 reports eight bytes per contact. Two registers decide how many contacts
 * exist and how long each block is; both live behind "control presence"
 * bitmaps in the query registers, so walk them the way the vendor driver does
 * (synaptics_rmi4_f12_init()).
 */
static int
rmi_f12_config(void)
{
	uint8_t q5[4];
	unsigned ctrl_8_offset = 0, ctrl_23_offset = 0, i;
	uint8_t c23[2];
	uint8_t q7, q8[4];

	/* query[5]: one bit per control register 0..7 -> the control base offset
	 * of register 8. Only the bits up to the ones we need matter here. */
	if (rmi_read(ct.f12_query + 5, q5, sizeof(q5))) {
		return EIO;
	}
	for (i = 0; i < 4; i++) {
		uint8_t b = q5[i];
		for (unsigned k = 0; k < 8; k++) {
			unsigned reg = i * 8 + k;
			unsigned present = (b >> k) & 1u;
			if (reg < 8) {
				ctrl_8_offset += present;
			} else if (reg < 23) {
				ctrl_23_offset += present;
			}
		}
	}
	ctrl_23_offset += ctrl_8_offset;

	if (rmi_read((uint16_t)(ct.f12_ctrl + ctrl_23_offset), c23, 2)) {
		return EIO;
	}
	ct.f12_fingers = c23[1] & 0x0fu;     /* max reported objects, low nibble */
	if (ct.f12_fingers == 0 || ct.f12_fingers > RMI_MAX_FINGERS) {
		printf("CHIRON TOUCH: implausible finger count %u (raw %02x%02x)\n",
		    ct.f12_fingers, c23[0], c23[1]);
		return EINVAL;
	}

	if (rmi_read(ct.f12_query + 7, &q7, 1)) {
		return EIO;
	}
	if (q7 < 1 || q7 > sizeof(q8)) {
		return EINVAL;
	}
	if (rmi_read(ct.f12_query + 8, q8, q7)) {
		return EIO;
	}
	/* Every one of data0..data15 present adds a byte in front of the 2D
	 * data; for a plain 2D sensor data0 is the only one and the finger
	 * blocks start one byte after the base. */
	ct.f12_finger_len = RMI_F12_FINGER_LEN;
	printf("CHIRON TOUCH: F12 %u fingers, query8 len %u%s\n", ct.f12_fingers,
	    q7, (q8[0] & 1u) ? ", data0 present" : "");
	return 0;
}

/* ---------------------------------------------------------------- report */

static void
ct_report(uint32_t mask, const int *x, const int *y)
{
	for (unsigned id = 0; id < TOUCH_MAX; id++) {
		uint32_t bit = 1u << id;

		if (mask & bit) {
			if (!(ct.active & bit) || x[id] != ct.x[id] || y[id] != ct.y[id]) {
				sweet_hid_touch((int)id, 1, x[id], y[id]);
			}
			ct.x[id] = x[id];
			ct.y[id] = y[id];
		} else if (ct.active & bit) {
			sweet_hid_touch((int)id, 0, ct.x[id], ct.y[id]);
		}
	}
	ct.active = mask;
}

/*
 * Read one F12 report: the finger status registers first (2 bits per contact),
 * then the 8-byte block of every contact that reports a finger. The block is
 *
 *   [0] object type (low nibble) and status (high nibble)
 *   [1] x_lsb  [2] x_msb  [3] y_lsb  [4] y_msb  [5] z  [6] wx  [7] wy
 */
static int
dsx_poll(void)
{
	uint8_t status[3];
	uint8_t blocks[RMI_MAX_FINGERS * RMI_F12_FINGER_LEN];
	unsigned nstatus = (ct.f12_fingers + 3) / 4;
	int x[TOUCH_MAX] = { 0 }, y[TOUCH_MAX] = { 0 };
	uint32_t mask = 0;

	if (nstatus > sizeof(status)) {
		return EINVAL;
	}
	if (rmi_read(ct.f12_data, status, nstatus)) {
		return EIO;
	}
	if (rmi_read((uint16_t)(ct.f12_data + 1), blocks,
	    ct.f12_fingers * ct.f12_finger_len)) {
		return EIO;
	}
	if (ct_decode_f12(status, blocks, ct.f12_fingers, ct.f12_finger_len,
	    ct.panel_w, ct.panel_h, &mask, x, y)) {
		return EIO;
	}
	ct_report(mask, x, y);
	return 0;
}

/* FocalTech ft5x46 on the same bus (the EBBG panel variant). */
static int
fts_poll(void)
{
	uint8_t buf[64];
	int x[TOUCH_MAX] = { 0 }, y[TOUCH_MAX] = { 0 };
	uint32_t mask = 0, seen = 0;
	unsigned active = 0, n;

	if (rmi_read(0x00, buf, sizeof(buf))) {
		return EIO;
	}
	n = buf[2] & 0x0fu;
	if ((buf[0] & 0x70u) || n > TOUCH_MAX) {
		return EIO;
	}
	for (unsigned i = 0; i < TOUCH_MAX; i++) {
		const uint8_t *p = buf + 3u + 6u * i;
		unsigned id = p[2] >> 4, event = p[0] >> 6;
		unsigned px, py;

		if (id >= TOUCH_MAX) {
			break;
		}
		if (seen & (1u << id)) {
			return EIO;
		}
		seen |= 1u << id;
		if (event == 1u) {
			continue;                /* UP record */
		}
		if (event == 3u) {
			return EIO;              /* reserved */
		}
		px = ((unsigned)(p[0] & 0x0fu) << 8) | p[1];
		py = ((unsigned)(p[2] & 0x0fu) << 8) | p[3];
		if (px >= ct.panel_w || py >= ct.panel_h) {
			return EIO;
		}
		mask |= 1u << id;
		x[id] = (int)px;
		y[id] = (int)py;
		active++;
	}
	if (active != n) {
		return EIO;
	}
	ct_report(mask, x, y);
	return 0;
}

/* ------------------------------------------------------------- power up */

static void
tlmm_pin(uint32_t pin, uint32_t mux, uint32_t pull, uint32_t drv, int oe, int output)
{
	uint32_t ctl = ct_rd(ct.tlmm, TLMM_CTL(pin));

	ctl &= ~(TLMM_MUX_MASK << TLMM_MUX_SHIFT);
	ctl |= (mux & TLMM_MUX_MASK) << TLMM_MUX_SHIFT;
	ctl &= ~(0x3u << TLMM_PULL_SHIFT);
	ctl |= (pull & 0x3u) << TLMM_PULL_SHIFT;
	ctl &= ~(0x7u << TLMM_DRV_SHIFT);
	ctl |= (drv & 0x7u) << TLMM_DRV_SHIFT;
	if (oe) {
		ctl |= TLMM_OE_BIT;
	} else {
		ctl &= ~TLMM_OE_BIT;
	}
	ct_wr(ct.tlmm, TLMM_CTL(pin), ctl);
	if (oe) {
		/* io register: drive the output bit */
		uint32_t io = ct_rd(ct.tlmm, TLMM_IO(pin));
		if (output) {
			io |= TLMM_IO_OUT;
		} else {
			io &= ~TLMM_IO_OUT;
		}
		ct_wr(ct.tlmm, TLMM_IO(pin), io);
	}
}

/*
 * Enable BLSP1 AHB and QUP5 I2C clocks through the GCC. Only called when the
 * QUP does not answer, i.e. when the bootloader did not leave it clocked --
 * writing the GCC of a running board could disturb other peripherals.
 */
static int
chiron_touch_clocks(void)
{
	uint32_t cbcr, dead;

	/* AHB branch: it is vote-controlled, so set both the branch and the vote */
	cbcr = ct_rd(ct.gcc, GCC_BLSP1_AHB_CBCR);
	if (cbcr & GCC_CBCR_HALT) {
		ct_wr(ct.gcc, GCC_BLSP1_AHB_CBCR, cbcr | GCC_CBCR_ENABLE);
		ct_wr(ct.gcc, GCC_APCS_ENA_VOTE,
		    ct_rd(ct.gcc, GCC_APCS_ENA_VOTE) | GCC_VOTE_BLSP1_AHB);
		if (qup_poll(ct.gcc, GCC_BLSP1_AHB_CBCR, GCC_CBCR_HALT, 0, 20)) {
			printf("CHIRON TOUCH: BLSP1 AHB clock did not come up\n");
			return ETIMEDOUT;
		}
	}
	/* QUP5 I2C apps branch: source stays on CXO/19.2 MHz (div 1) */
	cbcr = ct_rd(ct.gcc, GCC_QUP5_I2C_CBCR);
	if (cbcr & GCC_CBCR_HALT) {
		/* RCGR: source select CXO, divider 1 */
		uint32_t cfg = ct_rd(ct.gcc, GCC_QUP5_I2C_CMD_RCGR + 4);
		cfg &= ~(CFG_RCGR_DIV_MASK | CFG_RCGR_SRC_MASK);
		cfg |= (CFG_RCGR_SRC_CXO << 8) | 0u;   /* div field 0 = divide by 1 */
		ct_wr(ct.gcc, GCC_QUP5_I2C_CMD_RCGR + 4, cfg);
		ct_wr(ct.gcc, GCC_QUP5_I2C_CMD_RCGR,
		    ct_rd(ct.gcc, GCC_QUP5_I2C_CMD_RCGR) | CMD_RCGR_UPDATE);
		if (qup_poll(ct.gcc, GCC_QUP5_I2C_CMD_RCGR, CMD_RCGR_ROOT_STATUS, 0, 20)) {
			printf("CHIRON TOUCH: QUP5 RCGR root did not come up\n");
			return ETIMEDOUT;
		}
		ct_wr(ct.gcc, GCC_QUP5_I2C_CBCR,
		    ct_rd(ct.gcc, GCC_QUP5_I2C_CBCR) | GCC_CBCR_ENABLE);
		if (qup_poll(ct.gcc, GCC_QUP5_I2C_CBCR, GCC_CBCR_HALT, 0, 20)) {
			printf("CHIRON TOUCH: QUP5 I2C clock did not come up\n");
			return ETIMEDOUT;
		}
	}
	dead = ct_rd(ct.gcc, GCC_BLSP1_BCR);
	printf("CHIRON TOUCH: clocks ahb=%#x qup=%#x bcr=%#x\n",
	    ct_rd(ct.gcc, GCC_BLSP1_AHB_CBCR), ct_rd(ct.gcc, GCC_QUP5_I2C_CBCR), dead);
	return 0;
}

/* Bring the QUP v2 block into a known state and check it is really there. */
static int
qup_init(void)
{
	uint32_t ver = ct_rd(ct.qup, QUP_HW_VERSION);
	uint32_t cfg;

	printf("CHIRON TOUCH: qup hw_version=%#x state=%#x config=%#x\n",
	    ver, ct_rd(ct.qup, QUP_STATE), ct_rd(ct.qup, QUP_CONFIG));
	/* An unclocked or absent block reads all-ones or all-zeros */
	if (ver == 0 || ver == 0xffffffffu) {
		return ENXIO;
	}
	ct_wr(ct.qup, QUP_SW_RESET, 1);
	ct_wait_ms(5);
	cfg = QUP_CONFIG_I2C;
	ct_wr(ct.qup, QUP_CONFIG, cfg);
	if (ct_rd(ct.qup, QUP_CONFIG) != cfg) {
		printf("CHIRON TOUCH: QUP config write did not stick (%#x)\n",
		    ct_rd(ct.qup, QUP_CONFIG));
		return EIO;
	}
	ct_wr(ct.qup, QUP_ERROR_FLAGS, 0xffffffffu);
	ct_wr(ct.qup, QUP_ERROR_FLAGS_EN, 0);
	ct_wr(ct.qup, QUP_OPERATIONAL, 0x000ff0u);
	ct_wr(ct.qup, QUP_I2C_STATUS, 0xfffffcu);
	qup_set_bus_clock(ct.qup, 400000);
	if (qup_poll(ct.qup, QUP_STATE, QUP_STATE_I2C_MST, QUP_STATE_I2C_MST, 20)) {
		printf("CHIRON TOUCH: QUP did not report I2C master mode\n");
		return ETIMEDOUT;
	}
	return 0;
}

/* Which touch controller answered? */
static int
chiron_touch_probe(void)
{
	uint8_t raw[8];
	int dsx_ok = 0, fts_ok = 0;

	/* RMI4: the F01 query base is discoverable through the PDT, so a
	 * successful PDT read is itself the identity check. */
	ct.page = 0xffu;                        /* force a page select */
	if (rmi_scan_pdt() == 0) {
		dsx_ok = 1;
	}
	/* FocalTech: chip id registers 0xA3/0x9F read 0x54/0x52 */
	ct.page = 0xffu;
	if (qup_write(ct.qup, FTS_ADDR, (const uint8_t[]){ 0xa3 }, 1) == 0 &&
	    qup_read(ct.qup, FTS_ADDR, raw, 1) == 0 && raw[0] == 0x54) {
		if (qup_write(ct.qup, FTS_ADDR, (const uint8_t[]){ 0x9f }, 1) == 0 &&
		    qup_read(ct.qup, FTS_ADDR, raw, 1) == 0 && raw[0] == 0x52) {
			fts_ok = 1;
		}
	}
	printf("CHIRON TOUCH: probe synaptics=%d focaltech=%d\n", dsx_ok, fts_ok);
	if (dsx_ok && fts_ok) {
		printf("CHIRON TOUCH: both ICs answered, refusing to guess\n");
		return EIO;
	}
	if (dsx_ok) {
		ct.kind = 1;
		return rmi_f12_config();
	}
	if (fts_ok) {
		ct.kind = 2;
		return 0;
	}
	return ENODEV;
}

/* ---------------------------------------------------------------- thread */

static void
chiron_touch_thread(void *arg, wait_result_t result)
{
	unsigned delay_s = 45;
	unsigned errors = 0;
	int rc;

	(void)arg;
	(void)result;
	(void)PE_parse_boot_argn("chirontouchdelay", &delay_s, sizeof(delay_s));
	printf("CHIRON TOUCH: starting in %u s\n", delay_s);
	ct_wait_ms(delay_s * 1000u);

	if (!ct.panel_w || !ct.panel_h) {
		uint32_t w = 0, h = 0;
		sweet_panel_size(&w, &h);
		ct.panel_w = w ? w : 1080;
		ct.panel_h = h ? h : 2160;
	}
	printf("CHIRON TOUCH: panel %ux%u\n", ct.panel_w, ct.panel_h);

	ct.tlmm = ml_io_map(TLMM_PA, TLMM_SIZE);
	ct.qup = ml_io_map(QUP5_PA, QUP5_SIZE);
	ct.gcc = ml_io_map(GCC_PA, GCC_SIZE);
	if (!ct.tlmm || !ct.qup || !ct.gcc) {
		printf("CHIRON TOUCH: MMIO mapping failed\n");
		goto out;
	}

	/* Reset line as a GPIO output held low, IRQ line as a plain input. */
	tlmm_pin(PIN_TS_RESET, TLMM_FN_GPIO, 0, 0, true, false);
	tlmm_pin(PIN_TS_IRQ, TLMM_FN_GPIO, 2, 2, false, false);

	/* If the QUP is already clocked we leave the GCC alone. */
	if (qup_init()) {
		printf("CHIRON TOUCH: QUP silent, trying the GCC clocks\n");
		if (chiron_touch_clocks()) {
			goto out;
		}
		if (qup_init()) {
			printf("CHIRON TOUCH: QUP still silent; giving up\n");
			goto out;
		}
	}

	/* I2C5 pins to their blsp_i2c5 function. */
	tlmm_pin(PIN_I2C_SDA, TLMM_FN_I2C5, 3, 3, false, false);
	tlmm_pin(PIN_I2C_SCL, TLMM_FN_I2C5, 3, 3, false, false);

	/* Release reset (vendor: 5 ms active low, then >= 200 ms settle). */
	tlmm_pin(PIN_TS_RESET, TLMM_FN_GPIO, 0, 0, true, true);
	ct_wait_ms(250);

	rc = chiron_touch_probe();
	if (rc) {
		printf("CHIRON TOUCH: no supported controller (%d); stopping\n", rc);
		goto out;
	}
	printf("CHIRON TOUCH: %s ready (%u contacts), poll=%u ms\n",
	    ct.kind == 1 ? "Synaptics RMI4" : "FocalTech ft5x46",
	    ct.kind == 1 ? ct.f12_fingers : 5u, POLL_MS);

	while (!ct.dead) {
		int err = ct.kind == 1 ? dsx_poll() : fts_poll();
		if (err) {
			if ((errors++ % 100u) == 0) {
				printf("CHIRON TOUCH: report err=%d\n", err);
			}
			if (errors == 20) {
				ct_report(0, ct.x, ct.y);   /* release stuck contacts */
			}
		} else {
			errors = 0;
		}
		ct_wait_ms(POLL_MS);
	}
	ct_report(0, ct.x, ct.y);
out:
	(void)thread_terminate(current_thread());
	thread_block(THREAD_CONTINUE_NULL);
}

void
chiron_touch_init(void)
{
	thread_t thread;
	int enabled = 1;

	if (ct.started) {
		return;
	}
	ct.started = true;
	/*
	 * Only chiron has QUP v2 I2C5 at 0x00c179000 with a Synaptics panel.
	 * On any other board these addresses are something else entirely, so
	 * the default comes from the SoC; chirontouch=1 forces it anyway.
	 */
	if (!PE_parse_boot_argn("chirontouch", &enabled, sizeof(enabled)) &&
	    !sweet_board_is_chiron()) {
		enabled = 0;
		printf("CHIRON TOUCH: not an MSM8998 board, driver off "
		    "(chirontouch=1 forces it)\n");
	}
	if (!enabled) {
		printf("CHIRON TOUCH: disabled (chirontouch=0)\n");
		return;
	}
	if (kernel_thread_start(chiron_touch_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	} else {
		printf("CHIRON TOUCH: kernel_thread_start failed\n");
	}
}
#endif /* ARM64_BOARD_CONFIG_SWEET */
