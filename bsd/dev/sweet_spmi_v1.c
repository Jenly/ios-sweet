/*
 * sweet_spmi_v1.c -- SPMI PMIC-Arbiter v1 read path for MSM8998 (chiron).
 *
 * See sweet_spmi_v1.h for why this is a separate implementation and for the
 * register-level citations. Every constant below is traced to a line in
 * LineageOS/android_kernel_xiaomi_msm8998.
 *
 * Read-only: the only command ever issued is EXT_READL (opcode 1).
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <machine/machine_routines.h>
#include <IOKit/IOLocks.h>

#include "sweet_spmi_v1.h"

extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);

/* msm8998.dtsi:593-604, reg-names core/chnls/obsrvr/intr/cnfg */
#define SP_V1_CORE_PA   0x0800f000u
#define SP_V1_CORE_SZ   0x00001000u
#define SP_V1_OBS_PA    0x09400000u
#define SP_V1_OBS_SZ    0x01000000u
#define SP_V1_CFG_PA    0x0800a000u
#define SP_V1_CFG_SZ    0x00003000u

/* spmi-pmic-arb.c:30 -- the version word lives at core+0 */
#define SP_ARB_VERSION          0x0000u
/* spmi-pmic-arb.c:32-33 -- below V2_MIN means the v1 layout */
#define SP_ARB_VERSION_V2_MIN   0x20010000u

/* spmi-pmic-arb.c:37-43 -- per-channel block, same in every generation */
#define SP_CH_CMD               0x00u
#define SP_CH_STATUS            0x08u
#define SP_CH_RDATA0            0x18u

/* spmi-pmic-arb.c:68-72 -- status bits */
#define SP_STATUS_DONE          0x1u
#define SP_STATUS_ERRORS        0x0eu   /* FAILURE | DENIED | DROPPED */

/* spmi-pmic-arb.c:75-79 -- opcodes */
#define SP_OP_EXT_READL         1u

/* spmi-pmic-arb.c:1171-1174 -- APID map inside core */
#define SP_CHANNEL_MAP_OFF(n)   (0x800u + 0x4u * (n))
/* spmi-pmic-arb.c:890-895 -- observer window for this execution environment */
#define SP_OBS_OFF(ch)          (0x800u + 0x80u * (ch))
/* spmi-pmic-arb.c:1096-1099 -- command encoding for v1 */
#define SP_FMT_CMD_V1(opc, sid, addr, bc) \
	(((opc) << 27) | (((sid) & 0xfu) << 20) | (((addr) & 0xffffu) << 4) | ((bc) & 0x7u))

#define SP_NO_APID              0xffffu
#define SP_MAX_APIDS            512u
#define SP_PPID_MASK            0xfffu
#define SP_POLL_LIMIT           1000u

static vm_offset_t sp_core, sp_obs, sp_cfg;
static IOLock     *sp_v1_lock;
static unsigned    sp_apid_count;
static unsigned    sp_channel = 0;      /* qcom,channel = <0> (msm8998.dtsi:602) */
static int         sp_version;
static bool        sp_attempted, sp_ready;
/* A command that timed out may still complete: never reuse that lane. */
static bool        sp_poisoned[SP_MAX_APIDS];

static uint32_t
spv1_rd(vm_offset_t base, unsigned off)
{
	return *(volatile uint32_t *)(base + off);
}

static uint32_t
spv1_rd_ordered(vm_offset_t base, unsigned off)
{
	uint32_t v = *(volatile uint32_t *)(base + off);

	__asm__ volatile("dmb oshld" ::: "memory");
	return v;
}

/*
 * PPID forms the upper half of the address field: (sid << 8) | peripheral,
 * where peripheral is addr >> 8. Same scheme the v5 code uses, and the same one
 * spmi-pmic-arb.c builds its ppid_to_apid table from (regval >> 8).
 */
static uint16_t
spv1_find_apid(unsigned ppid)
{
	for (unsigned i = 0; i < sp_apid_count; i++) {
		uint32_t map = spv1_rd_ordered(sp_core, SP_CHANNEL_MAP_OFF(i));
		uint32_t owner;

		if (map == 0) {
			continue;
		}
		if (((map >> 8) & SP_PPID_MASK) != ppid) {
			continue;
		}
		/*
		 * The ownership table says which execution environment may touch
		 * the peripheral. EE 0 is the AP, which is where this runs; a
		 * peripheral owned elsewhere would be DENIED anyway, but checking
		 * first avoids one pointless timed-out transaction.
		 */
		owner = spv1_rd_ordered(sp_cfg, 0x700u + 4u * i) & 0x7u;
		if (owner == 0) {
			return (uint16_t)i;
		}
	}
	return SP_NO_APID;
}

int
sweet_spmi_arb_version(void)
{
	return sp_version;
}

const char *
sweet_spmi_arb_name(void)
{
	switch (sp_version) {
	case 1:  return "v1 (MSM8998)";
	case 5:  return "v5 (SM7150)";
	case 0:  return "not present";
	default: return "unknown";
	}
}

int
sweet_spmi_v1_init(void)
{
	uint32_t version;

	if (sp_attempted) {
		return sp_ready ? 0 : -1;
	}
	sp_attempted = true;

	sp_core = ml_io_map(SP_V1_CORE_PA, SP_V1_CORE_SZ);
	sp_cfg  = ml_io_map(SP_V1_CFG_PA, SP_V1_CFG_SZ);
	sp_obs  = ml_io_map(SP_V1_OBS_PA, SP_V1_OBS_SZ);
	if (sp_core == 0 || sp_cfg == 0 || sp_obs == 0) {
		printf("SWEET SPMI v1: mapping failed (core=%#lx cfg=%#lx obs=%#lx)\n",
		    (unsigned long)sp_core, (unsigned long)sp_cfg,
		    (unsigned long)sp_obs);
		return -1;
	}

	version = spv1_rd_ordered(sp_core, SP_ARB_VERSION);
	if (version == 0 || version == 0xFFFFFFFFu) {
		printf("SWEET SPMI v1: arbiter version reads %#x; block not clocked?\n",
		    version);
		return -1;
	}
	if (version >= SP_ARB_VERSION_V2_MIN) {
		/*
		 * Not the generation this file implements. Refuse rather than
		 * interpret registers that mean something else here.
		 */
		printf("SWEET SPMI v1: arbiter is %#x (v2 or newer); this driver "
		    "only implements the v1 layout at 0x%x\n",
		    version, SP_V1_CORE_PA);
		return -1;
	}

	/*
	 * APID count. The v1 arbiter does not publish a count the way v5 does
	 * (core+4); the usable range is bounded by the mapping table's size in
	 * the core window, (0x1000 - 0x800) / 4 = 512 entries.
	 */
	sp_apid_count = SP_MAX_APIDS;

	sp_v1_lock = IOLockAlloc();
	if (sp_v1_lock == NULL) {
		printf("SWEET SPMI v1: cannot allocate the access lock\n");
		return -1;
	}

	sp_version = 1;
	sp_ready = true;
	printf("SWEET SPMI: arbiter %#x -> %s, channel %u, read-only\n",
	    version, sweet_spmi_arb_name(), sp_channel);
	return 0;
}

int
sweet_spmi_v1_read(unsigned sid, unsigned addr, uint8_t *out)
{
	unsigned off, base;
	uint16_t apid;
	uint32_t cmd, status;

	if (!sp_ready || out == NULL || addr > 0xffffu || sid > 0xfu) {
		return -1;
	}

	IOLockLock(sp_v1_lock);

	apid = spv1_find_apid(((sid & 0xfu) << 8) | ((addr >> 8) & 0xffu));
	if (apid == SP_NO_APID || sp_poisoned[apid]) {
		IOLockUnlock(sp_v1_lock);
		return -1;
	}

	base = SP_OBS_OFF(sp_channel);
	off = base + 0x80u * apid;

	/*
	 * EXT_READL, byte count 0 (one byte). PMIC_ARB_CMD is a single 32-bit
	 * store; the barrier afterwards is what spmi-pmic-arb.c gets from
	 * readl/writel semantics, made explicit here.
	 */
	cmd = SP_FMT_CMD_V1(SP_OP_EXT_READL, sid, addr, 0);
	__asm__ volatile("dmb oshst" ::: "memory");
	*(volatile uint32_t *)(sp_obs + off + SP_CH_CMD) = cmd;
	__asm__ volatile("dsb osh" ::: "memory");

	for (unsigned i = 0; i < SP_POLL_LIMIT; i++) {
		status = spv1_rd_ordered(sp_obs, off + SP_CH_STATUS);
		if (status & SP_STATUS_DONE) {
			if (status & SP_STATUS_ERRORS) {
				IOLockUnlock(sp_v1_lock);
				return -1;
			}
			*out = (uint8_t)spv1_rd_ordered(sp_obs,
			    off + SP_CH_RDATA0);
			IOLockUnlock(sp_v1_lock);
			return 0;
		}
		delay(1);
	}

	/*
	 * Timed out. The transaction may still complete and land in RDATA0
	 * later, which would corrupt an unrelated read on this lane, so retire
	 * it for the rest of the boot.
	 */
	sp_poisoned[apid] = true;
	printf("SWEET SPMI v1: apid %u (sid %u addr %#x) timed out; lane retired\n",
	    apid, sid, addr);
	IOLockUnlock(sp_v1_lock);
	return -1;
}
