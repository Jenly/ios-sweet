/*
 * sweet_restart.c -- SoC reset and power-off via PSHOLD, for MSM8998 and SM7150.
 *
 * See sweet_restart.h for the hardware sources and the safety argument. The short
 * version: reboot(2) currently does nothing on this port because PE_halt_restart
 * is NULL; this installs it, but only after proving PSHOLD reads as asserted.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <kern/clock.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <pexpert/SweetBoard.h>
#include <IOKit/IOPlatformExpert.h>

#include "sweet_restart.h"

extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);

/*
 * Per-SoC addresses. Both come from the live device tree fragments in
 * LineageOS/android_kernel_xiaomi_msm8998.
 */
struct restart_soc {
	const char *soc;
	uint32_t    pshold_pa;
	uint32_t    imem_reason_pa;    /* absolute address of the reason word */
};

/* msm8998.dtsi:586-590 and :3043-3063 (imem base + the child at 0x65c). */
static const struct restart_soc restart_chiron = {
	.soc             = "MSM8998",
	.pshold_pa       = 0x010AC000u,
	.imem_reason_pa  = 0x146BF65Cu,
};

/*
 * SM7150 / sweet. devices.py carries pshold = 0x010ac000 for chiron; the SM7150
 * value is not in this workspace's device trees, so it is deliberately NOT
 * guessed. A wrong PSHOLD address powers off at best and writes an unknown
 * register at worst, and the preflight below cannot tell the difference between
 * "asserted PSHOLD" and "some other register that happens to read odd".
 */
static const struct restart_soc restart_sweet = {
	.soc             = "SM7150",
	.pshold_pa       = 0,          /* unknown: hook is not installed */
	.imem_reason_pa  = 0,
};

/* msm-poweroff.c:313 -- the "normal reboot" reason. */
#define SWEET_RESTART_REASON_NORMAL  0x77665500u

static vm_offset_t s_pshold;
static vm_offset_t s_reason;
static int         s_available;
static boolean_t   s_attempted;

int
sweet_restart_available(void)
{
	return s_available;
}

static const struct restart_soc *
restart_pick(void)
{
	return sweet_board_is_chiron() ? &restart_chiron : &restart_sweet;
}

/*
 * The hook. Called by IOPlatformExpert::halt_restart() for kPERestartCPU,
 * kPEHaltCPU and the panic variants. On Qualcomm both restart and power-off end
 * with PS_HOLD deasserted; the reason word tells the bootloader which it was.
 *
 * This must not return. Linux's equivalent ends in mdelay(10000) and then logs a
 * failure; here a returned halt would let userland continue after being told the
 * machine was going down, so the last resort is to park the CPU with interrupts
 * off rather than resume.
 */
static int
sweet_halt_restart(unsigned int type)
{
	uint32_t before;

	switch (type) {
	case kPERestartCPU:
	case kPEPanicRestartCPU:
	case kPEPanicRestartCPUNoCallouts:
		printf("SWEET RESTART: restart requested (type %u), writing reason "
		    "%#x to IMEM and deasserting PS_HOLD\n", type,
		    SWEET_RESTART_REASON_NORMAL);
		break;
	case kPEHaltCPU:
		printf("SWEET RESTART: power-off requested (type %u); deasserting "
		    "PS_HOLD\n", type);
		break;
	default:
		/* Not a request we implement; report and let the caller decide. */
		printf("SWEET RESTART: refusing unsupported halt/restart type %u\n",
		    type);
		return -1;
	}

	if (s_pshold == 0) {
		return -1;
	}

	before = *(volatile uint32_t *)s_pshold;
	printf("SWEET RESTART: PS_HOLD reads %#x before the write\n", before);

	/*
	 * Restart reason first. If the reset happened before this word landed the
	 * bootloader would treat the boot as unplanned, which on a Qualcomm
	 * bootloader can mean download mode instead of a normal boot.
	 */
	if (s_reason != 0) {
		*(volatile uint32_t *)s_reason = SWEET_RESTART_REASON_NORMAL;
		__asm__ volatile("dsb sy" ::: "memory");
	}

	__asm__ volatile("dmb sy" ::: "memory");
	*(volatile uint32_t *)s_pshold = 0;
	__asm__ volatile("dsb sy" ::: "memory");

	/*
	 * Should be gone by now. If not, the write did not take -- wrong address,
	 * gated block, or a PMIC that wants the SCM call instead. Say so once and
	 * park rather than resuming as if the machine had reset.
	 */
	delay(1000);
	printf("SWEET RESTART: still here after deasserting PS_HOLD (reads %#x); "
	    "the SoC did not power down\n", *(volatile uint32_t *)s_pshold);
	for (;;) {
		__asm__ volatile("wfi" ::: "memory");
	}
}

int
sweet_restart_init(void)
{
	const struct restart_soc *soc;
	uint32_t probe;

	if (s_attempted) {
		return s_available ? 0 : -1;
	}
	s_attempted = TRUE;

	soc = restart_pick();
	if (soc->pshold_pa == 0) {
		printf("SWEET RESTART: no PSHOLD address for %s in this workspace; "
		    "reboot stays unavailable rather than writing a guess\n",
		    soc->soc);
		return -1;
	}

	s_pshold = ml_io_map(soc->pshold_pa, 4);
	if (s_pshold == 0) {
		printf("SWEET RESTART: cannot map PSHOLD at %#x\n", soc->pshold_pa);
		return -1;
	}
	s_reason = ml_io_map(soc->imem_reason_pa, 4);
	if (s_reason == 0) {
		/* The reset still works; only the way we come back changes. */
		printf("SWEET RESTART: IMEM reason at %#x unavailable; the "
		    "bootloader will not be told this was a normal reboot\n",
		    soc->imem_reason_pa);
	}

	/*
	 * Preflight. A running SoC has PS_HOLD asserted, so bit 0 must be set. A
	 * dead or gated bus reads 0 or all-ones, and any other answer means the
	 * address is not what the device tree claimed.
	 */
	probe = *(volatile uint32_t *)s_pshold;
	if (!sweet_restart_probe_ok(probe)) {
		printf("SWEET RESTART: PSHOLD at %#x reads %#x, which is not an "
		    "asserted power hold; refusing to install the reset hook\n",
		    soc->pshold_pa, probe);
		return -1;
	}

	printf("SWEET RESTART: %s PSHOLD at %#x reads %#x (asserted), reason word "
	    "at %#x; reboot and power-off are live\n",
	    soc->soc, soc->pshold_pa, probe, soc->imem_reason_pa);

	PE_halt_restart = sweet_halt_restart;
	s_available = 1;
	return 0;
}
