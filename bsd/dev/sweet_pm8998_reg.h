/*
 * sweet_pm8998_reg.h -- PM8998 regulator control over the v1 SPMI arbiter.
 *
 * Why this exists: chiron's touch controller is powered from pm8998_l6 and its
 * analogue rail from pm8998_l14 (xiaomi/chiron-msm8998-mtp.dtsi:214,217):
 *
 *     vdd-supply  = <&pm8998_l6>;      synaptics,pwr-reg-name = "vdd";
 *     disp-supply = <&pm8998_l14>;     synaptics,bus-reg-name = "avdd";
 *
 * On the Android side the PMIC leaves those rails up. Whether a bootloader
 * handoff keeps them up is unknown, so the touch driver needs to be able to turn
 * them on itself. Today chiron_touch.c only notes the supplies in a comment.
 *
 * REGISTER MAP -- from qcom_spmi-regulator.c in this workspace, which is the
 * authority rather than a datasheet guess:
 *
 *   module bases (spmi_regulator_data tables, qcom_spmi-regulator.c:1441-1510)
 *       l1  0x4000    l2  0x4100   ...   l6  0x4500   ...   l14 0x4d00
 *       i.e. 0x4000 + 0x100 * (n - 1)
 *
 *   control registers, common to every QPNP regulator
 *   (qcom_spmi-regulator.c:159-166, "these are offsets from 0x40"):
 *       +0x40 VOLTAGE_RANGE
 *       +0x41 VOLTAGE_SET
 *       +0x45 MODE
 *       +0x46 ENABLE          <- SPMI_COMMON_REG_ENABLE
 *
 *   enable bits (qcom_spmi-regulator.c:193-195)
 *       ENABLE_MASK 0x80   ENABLE 0x80   DISABLE 0x00
 *
 *   mode bits (qcom_spmi-regulator.c:203-206)
 *       HPM 0x80 (high power mode)   AUTO 0x40   BYPASS 0x20
 *
 *   the driver's own enable path is
 *       spmi_regulator_common_enable -> update_bits(REG_ENABLE, 0x80, 0x80)
 *   (qcom_spmi-regulator.c:505-511) so writing 0x80 into +0x46 is exactly what
 *   Linux does.
 *
 * SID: pm8998 sits at SPMI slave 0 (msm-pm8998.dtsi:18, "qcom,pm8998@0").
 *
 * PUMP/IBB (lab-supply, ibb-supply) are PMI8998 LAB/IBB regulators with their
 * own module types and current-limit programming. They are NOT touched here:
 * they drive the display backlight, not the touch panel, and getting their
 * current limits wrong is a hardware risk with no way to validate it from
 * software yet.
 */

#ifndef SWEET_PM8998_REG_H
#define SWEET_PM8998_REG_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* PM8998 LDO module bases, qcom_spmi-regulator.c:1446-1466 */
#define PM8998_LDO_BASE(n)     (0x4000u + 0x100u * ((n) - 1u))

#define PM8998_LDO_L6          6u
#define PM8998_LDO_L14         14u

/* Common control register offsets from the module base */
#define PM8998_REG_VOLTAGE_RANGE  0x40u
#define PM8998_REG_VOLTAGE_SET    0x41u
#define PM8998_REG_MODE           0x45u
#define PM8998_REG_ENABLE         0x46u

#define PM8998_ENABLE_BIT         0x80u
#define PM8998_MODE_HPM           0x80u
#define PM8998_MODE_AUTO          0x40u

#define PM8998_SID                0u

/*
 * Read one regulator control register. 0 on success.
 * Needs sweet_spmi_v1_init() to have succeeded.
 */
int sweet_pm8998_read(unsigned ldo, unsigned reg, uint8_t *out);

/*
 * Report what an LDO currently looks like: enable state, mode, voltage selector.
 * Logs one line; never writes. Returns 1 if enabled, 0 if not, -1 on error.
 */
int sweet_pm8998_report(const char *label, unsigned ldo);

/*
 * Ensure an LDO is enabled. If it already is, this is a no-op and says so --
 * the common case when a bootloader already brought the panel up.
 * Returns 0 if the rail ends up enabled, negative otherwise.
 */
int sweet_pm8998_enable(const char *label, unsigned ldo);

#ifdef __cplusplus
}
#endif

#endif /* SWEET_PM8998_REG_H */
