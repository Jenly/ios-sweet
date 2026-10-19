/*
 * sweet_restart.h -- SoC reset and power-off for the ios-sweet port.
 *
 * WHY THIS EXISTS. `PE_halt_restart` is assigned nowhere on ARM, so it stays NULL
 * and IOPlatformExpert::halt_restart() returns -1 for every request
 * (IOPlatformExpert.cpp:363-368). That means `reboot(2)` and the shutdown path in
 * userland do nothing at all on this port -- the call reports failure and the
 * phone keeps running. On a device whose only other escape is holding the power
 * button, that is worth fixing.
 *
 * HOW QUALCOMM DOES IT. Deasserting PS_HOLD tells the PMIC to remove the SoC's
 * power; the bootloader then decides what to do next based on a "restart reason"
 * word left in IMEM. Both halves matter: reset without a reason can be read as a
 * crash, and the reason without the deassert does nothing.
 *
 *   PSHOLD         msm8998.dtsi:586-590  restart@10ac000, reg = <0x10ac000 0x4>
 *                  deassert_ps_hold() does __raw_writel(0, msm_ps_hold)
 *                  (msm-poweroff.c:381-396)
 *   restart reason msm8998.dtsi:3043-3063  qcom,msm-imem@146bf000 size 0x1000,
 *                  child restart_reason@65c size 4
 *                  -> absolute 0x146bf000 + 0x65c = 0x146bf65c
 *   reason values  msm-poweroff.c:313-361  0x77665500 normal, then
 *                  0x77665501/02/03 and 0x77665508/09/0a for the dload modes
 *
 * SAFETY. Writing 0 to PS_HOLD powers the SoC off immediately, so taking over
 * reboot is only justified when the register can be shown to be what we think it
 * is. sweet_restart_probe_ok() is that check, and it is a pure function on
 * purpose: it lives here, both the driver and its test call this exact code, and
 * testing it needs no stubs at all.
 *
 * Nothing else is written: no PMIC access, no SCM call, no watchdog bite, no
 * download-mode flags. Linux also disables the SDI and halts the SPMI arbiter
 * before resetting; neither is needed for a reset to take effect on this hardware
 * and both are extra writes that cannot be validated here, so they are left out
 * deliberately.
 */

#ifndef SWEET_RESTART_H
#define SWEET_RESTART_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Is this PSHOLD read plausible for a SoC that is currently running?
 *
 * Rule: bit 0 must be set, and the value must not be all ones. A running SoC has
 * PS_HOLD asserted, so bit 0 reads 1. The three ways this register can look wrong
 * are all rejected by that single bit:
 *
 *   0x00000000  a gated or powered-down block (should be impossible while the CPU
 *               is executing, which is exactly why it means the address is wrong)
 *   0xFFFFFFFF  a dead bus, which reads all ones
 *   anything with bit 0 clear   some other register entirely
 *
 * Extra bits above bit 0 are allowed: the register may carry unrelated status in
 * its upper bits, and demanding an exact value of 1 would reject a working board.
 *
 * Returning 0 means "do not install the reset hook", which leaves reboot(2)
 * reporting failure. That is strictly better than writing zero to an unknown
 * register on a phone.
 */
static inline int
sweet_restart_probe_ok(uint32_t pshold_value)
{
	if (pshold_value == 0u || pshold_value == 0xFFFFFFFFu) {
		return 0;
	}
	return (pshold_value & 1u) != 0u;
}

/*
 * Probe PSHOLD and, if it is trustworthy, install the PE_halt_restart hook.
 * Idempotent. Returns 0 if the hook is installed, negative otherwise; the reason
 * goes to the boot log either way.
 */
int sweet_restart_init(void);

/* 1 if the hook is installed, i.e. reboot(2) will actually reset the SoC. */
int sweet_restart_available(void);

#ifdef __cplusplus
}
#endif

#endif /* SWEET_RESTART_H */
