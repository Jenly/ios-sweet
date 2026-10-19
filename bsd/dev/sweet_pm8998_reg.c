/*
 * sweet_pm8998_reg.c -- PM8998 LDO control for chiron's touch rails.
 *
 * SAFETY MODEL. This is the first code in the port that WRITES to the PMIC, and
 * that PMIC also feeds the CPU rails, so the constraints are deliberately severe:
 *
 *   1. OFF BY DEFAULT. Nothing here runs unless the boot-arg
 *      `sweetpmic=1` is given. Without it every entry point returns
 *      -1 immediately and the PMIC is never touched.
 *
 *   2. SET-ONLY. The only write is "enable". There is no disable path, no
 *      voltage change, no mode change, no pull-down, no soft-start. A regulator
 *      that is already on stays on with the voltage and mode the bootloader
 *      chose, which is what the display and touch controller were brought up
 *      against.
 *
 *   3. WHITELISTED RAILS. Only PM8998 L6 and L14 (chiron's touch vdd and
 *      analogue rail, chiron-msm8998-mtp.dtsi:214,217) can be addressed. Rails
 *      that feed the CPUs, DDR or the eMMC/UFS are not reachable by name, and
 *      the range check rejects anything outside the two entries.
 *
 *   4. READ-BACK. After writing the enable bit the register is read again; a
 *      rail that does not read back enabled is reported as a failure rather
 *      than assumed to be up.
 *
 *   5. NO BLIND RETRY. If the write does not take, this reports and stops. It
 *      never loops on the PMIC.
 *
 * What is deliberately NOT done: LAB/IBB (pmi8998), which drive the display
 * backlight and need correct current-limit programming that cannot be validated
 * from software yet. They are left exactly as the bootloader set them.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <kern/clock.h>		/* delay(), under XNU_KERNEL_PRIVATE */
#include <pexpert/pexpert.h>

#include "sweet_pm8998_reg.h"
#include "sweet_spmi_v1.h"

/*
 * SPMI write, minimal and local to this file.
 *
 * The v1 arbiter command for a one-byte extended write is opcode 0
 * (PMIC_ARB_OP_EXT_WRITEL, spmi-pmic-arb.c:76) with byte count 0 and the data in
 * WDATA0 (PMIC_ARB_WDATA0 = +0x10, spmi-pmic-arb.c:41). Kept private rather than
 * added to sweet_spmi_v1.c so that the general SPMI path stays read-only: a
 * write can only happen through this whitelisted regulator entry point.
 */
extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);

#define SP_V1_CORE_PA   0x0800f000u
#define SP_V1_CORE_SZ   0x00001000u
#define SP_V1_OBS_PA    0x09400000u
#define SP_V1_OBS_SZ    0x01000000u
#define SP_V1_CFG_PA    0x0800a000u
#define SP_V1_CFG_SZ    0x00003000u

#define SP_CH_CMD       0x00u
#define SP_CH_STATUS    0x08u
#define SP_CH_WDATA0    0x10u
#define SP_CH_RDATA0    0x18u

#define SP_STATUS_DONE  0x1u
#define SP_STATUS_ERRORS 0x0eu

#define SP_OP_EXT_WRITEL 0u
#define SP_OP_EXT_READL  1u

#define SP_CHANNEL_MAP_OFF(n) (0x800u + 0x4u * (n))
#define SP_OBS_OFF(ch)        (0x800u + 0x80u * (ch))
#define SP_FMT_CMD_V1(opc, sid, addr, bc) \
	(((opc) << 27) | (((sid) & 0xfu) << 20) | (((addr) & 0xffffu) << 4) | ((bc) & 0x7u))

#define SP_NO_APID      0xffffu
#define SP_MAX_APIDS    512u
#define SP_PPID_MASK    0xfffu
#define SP_POLL_LIMIT   1000u

static vm_offset_t pm_core, pm_obs, pm_cfg;
static unsigned    pm_apids;
static bool        pm_mapped;
static bool        pm_considered;
static bool        pm_allowed;
static int         pm_sleep_chan;

static uint32_t
pm_rd(vm_offset_t base, unsigned off)
{
	return *(volatile uint32_t *)(base + off);
}

static uint16_t
pm_find_apid(unsigned ppid)
{
	for (unsigned i = 0; i < pm_apids; i++) {
		uint32_t map = pm_rd(pm_core, SP_CHANNEL_MAP_OFF(i));

		if (map == 0 || ((map >> 8) & SP_PPID_MASK) != ppid) {
			continue;
		}
		if ((pm_rd(pm_cfg, 0x700u + 4u * i) & 0x7u) == 0) {
			return (uint16_t)i;
		}
	}
	return SP_NO_APID;
}

/* One SPMI transaction. write != 0 stores `data`; otherwise reads into *data. */
static int
pm_xfer(unsigned sid, unsigned addr, uint8_t *data, int write)
{
	unsigned off, base;
	uint16_t apid;
	uint32_t cmd, status;

	if (!pm_mapped || data == NULL) {
		return -1;
	}
	apid = pm_find_apid(((sid & 0xfu) << 8) | ((addr >> 8) & 0xffu));
	if (apid == SP_NO_APID) {
		return -1;
	}
	base = SP_OBS_OFF(0);
	off = base + 0x80u * apid;

	__asm__ volatile("dmb oshst" ::: "memory");
	if (write) {
		*(volatile uint32_t *)(pm_obs + off + SP_CH_WDATA0) = *data;
		__asm__ volatile("dmb oshst" ::: "memory");
	}
	cmd = SP_FMT_CMD_V1(write ? SP_OP_EXT_WRITEL : SP_OP_EXT_READL,
	    sid, addr, 0);
	*(volatile uint32_t *)(pm_obs + off + SP_CH_CMD) = cmd;
	__asm__ volatile("dsb osh" ::: "memory");

	for (unsigned i = 0; i < SP_POLL_LIMIT; i++) {
		status = pm_rd(pm_obs, off + SP_CH_STATUS);
		if (status & SP_STATUS_DONE) {
			if (status & SP_STATUS_ERRORS) {
				return -1;
			}
			if (!write) {
				*data = (uint8_t)pm_rd(pm_obs, off + SP_CH_RDATA0);
			}
			return 0;
		}
		delay(1);
	}
	return -1;      /* timed out; the caller must not retry blindly */
}

/*
 * Gate. Everything below is inert unless sweetpmic=1 and the v1 arbiter is
 * present. `pm_considered` makes the decision once per boot.
 */
static int
pm_allowed_once(void)
{
	int want = 0;

	if (pm_considered) {
		return pm_allowed ? 0 : -1;
	}
	pm_considered = true;

	if (!PE_parse_boot_argn("sweetpmic", &want, sizeof(want)) || want != 1) {
		printf("SWEET PM8998: not enabled (boot-arg sweetpmic=1 required); "
		    "regulators left exactly as the bootloader set them\n");
		return -1;
	}
	if (sweet_spmi_v1_init() != 0) {
		printf("SWEET PM8998: no v1 SPMI arbiter; cannot reach the PMIC\n");
		return -1;
	}

	pm_core = ml_io_map(SP_V1_CORE_PA, SP_V1_CORE_SZ);
	pm_obs  = ml_io_map(SP_V1_OBS_PA, SP_V1_OBS_SZ);
	pm_cfg  = ml_io_map(SP_V1_CFG_PA, SP_V1_CFG_SZ);
	if (pm_core == 0 || pm_obs == 0 || pm_cfg == 0) {
		printf("SWEET PM8998: PMIC window mapping failed\n");
		return -1;
	}
	pm_apids = SP_MAX_APIDS;

	/*
	 * Prove the PMIC answers before allowing any write: read the PM8998
	 * revision register pair (PERPH 0x01, address 0x0100/0x0101 on SID 0).
	 * A dead bus reads 0x00 or 0xff for both bytes.
	 */
	{
		uint8_t rev0 = 0, rev1 = 0;

		if (pm_xfer(PM8998_SID, 0x0100u, &rev0, 0) != 0 ||
		    pm_xfer(PM8998_SID, 0x0101u, &rev1, 0) != 0 ||
		    (rev0 == 0 && rev1 == 0) || (rev0 == 0xff && rev1 == 0xff)) {
			printf("SWEET PM8998: PMIC does not answer at SID 0 "
			    "(rev %#x %#x); refusing to write\n", rev0, rev1);
			return -1;
		}
		printf("SWEET PM8998: PMIC revision %u.%u, writes permitted for "
		    "L6/L14 only\n", rev1, rev0);
	}

	pm_mapped = true;
	pm_allowed = true;
	return 0;
}

static int
pm_rail_ok(unsigned ldo)
{
	/* Only the two rails chiron's touch path uses. */
	return ldo == PM8998_LDO_L6 || ldo == PM8998_LDO_L14;
}

int
sweet_pm8998_read(unsigned ldo, unsigned reg, uint8_t *out)
{
	if (!pm_rail_ok(ldo) || reg > 0x7fu || out == NULL) {
		return -1;
	}
	if (pm_allowed_once() != 0) {
		return -1;
	}
	return pm_xfer(PM8998_SID, PM8998_LDO_BASE(ldo) + reg, out, 0);
}

int
sweet_pm8998_report(const char *label, unsigned ldo)
{
	uint8_t en = 0, mode = 0, range = 0, set = 0;

	if (!pm_rail_ok(ldo)) {
		printf("SWEET PM8998: rail %u is not whitelisted\n", ldo);
		return -1;
	}
	if (pm_allowed_once() != 0) {
		return -1;
	}
	if (sweet_pm8998_read(ldo, PM8998_REG_ENABLE, &en) != 0 ||
	    sweet_pm8998_read(ldo, PM8998_REG_MODE, &mode) != 0 ||
	    sweet_pm8998_read(ldo, PM8998_REG_VOLTAGE_RANGE, &range) != 0 ||
	    sweet_pm8998_read(ldo, PM8998_REG_VOLTAGE_SET, &set) != 0) {
		printf("SWEET PM8998: %s (L%u) unreadable\n",
		    label != NULL ? label : "?", ldo);
		return -1;
	}

	printf("SWEET PM8998: %s (L%u) enable=%s mode=%s range=%u set=%u\n",
	    label != NULL ? label : "?", ldo,
	    (en & PM8998_ENABLE_BIT) ? "on" : "off",
	    (mode & PM8998_MODE_HPM) ? "HPM" :
	    ((mode & PM8998_MODE_AUTO) ? "AUTO" : "LPM"),
	    range, set);

	return (en & PM8998_ENABLE_BIT) ? 1 : 0;
}

int
sweet_pm8998_enable(const char *label, unsigned ldo)
{
	uint8_t en = 0;

	if (!pm_rail_ok(ldo)) {
		printf("SWEET PM8998: refusing to touch rail %u (not whitelisted)\n",
		    ldo);
		return -1;
	}
	if (pm_allowed_once() != 0) {
		return -1;
	}
	if (sweet_pm8998_read(ldo, PM8998_REG_ENABLE, &en) != 0) {
		printf("SWEET PM8998: %s (L%u) enable register unreadable\n",
		    label != NULL ? label : "?", ldo);
		return -1;
	}
	if (en & PM8998_ENABLE_BIT) {
		/*
		 * Already on -- leave it alone. This is the expected case when a
		 * bootloader powered the panel, and there is no reason to rewrite
		 * a register that already says what we want.
		 */
		printf("SWEET PM8998: %s (L%u) already enabled (%#x); no write\n",
		    label != NULL ? label : "?", ldo, en);
		return 0;
	}

	/*
	 * SET-ONLY: OR in the enable bit, keep every other bit as it is. Never
	 * write 0x80 outright, because the low nibble carries the hardware
	 * enable-follow bits (SPMI_COMMON_ENABLE_FOLLOW_*_MASK) which the
	 * bootloader may have configured.
	 */
	{
		uint8_t want = (uint8_t)(en | PM8998_ENABLE_BIT);

		if (pm_xfer(PM8998_SID, PM8998_LDO_BASE(ldo) + PM8998_REG_ENABLE,
		    &want, 1) != 0) {
			printf("SWEET PM8998: %s (L%u) enable write failed; "
			    "leaving the rail as it was\n",
			    label != NULL ? label : "?", ldo);
			return -1;
		}
	}

	/* Read back rather than assume. */
	if (sweet_pm8998_read(ldo, PM8998_REG_ENABLE, &en) != 0 ||
	    !(en & PM8998_ENABLE_BIT)) {
		printf("SWEET PM8998: %s (L%u) did not read back enabled (%#x)\n",
		    label != NULL ? label : "?", ldo, en);
		return -1;
	}
	printf("SWEET PM8998: %s (L%u) enabled (now %#x)\n",
	    label != NULL ? label : "?", ldo, en);
	return 0;
}
