/*
 * Board identification and display geometry read from the ios-sweet device tree.
 *
 * The Apple Device Tree that tools/mkadt.py generates describes the phone the
 * kernel is running on: the root "compatible" list starts with the iPhone
 * identity whose userland this port boots (D22AP / iPhone10,3) and then names
 * the board, and /arm-io/disp0 carries the panel and the logical (presented)
 * screen size.  These helpers read that description during early boot so that
 * one kernel binary serves every supported phone instead of hardcoding one
 * board's numbers in a dozen places.
 *
 * Devices supported at the time of writing (see tools/devices.py):
 *   sweet   Redmi Note 10 Pro, SM7150,  1080x2400 panel, virt timer INTID 19
 *   chiron  Xiaomi Mi MIX 2,  MSM8998,  1080x2160 panel, virt timer INTID 27
 *
 * Everything here is a small inline function on purpose: it must be usable
 * from C (bsd/dev drivers) and C++ (iokit/Kernel/arm) and needs no config
 * option or extra translation unit.
 */
#ifndef _PEXPERT_SWEETBOARD_H
#define _PEXPERT_SWEETBOARD_H

#include <pexpert/pexpert.h>
#include <pexpert/device_tree.h>
#include <libkern/libkern.h>

#define SWEET_BOARD_SM7150      "qcom,sm7150"   /* sweet: Redmi Note 10 Pro */
#define SWEET_BOARD_MSM8998     "qcom,msm8998"  /* chiron: Mi MIX 2 */
#define SWEET_BOARD_NAME        "ios-sweet"     /* root compatible, all boards */

/* Fallbacks if the tree has no /arm-io/disp0 (or an older tree without the
 * canvas/dots-per-inch properties): the iPhone X screen the userland wants. */
#define SWEET_DISPLAY_LOGICAL_W 1125            /* 375 x 812 points @3x */
#define SWEET_DISPLAY_LOGICAL_H 2436
#define SWEET_DISPLAY_DPI       458

/* ADT node names that the boot shim / mkadt.py generate. */
#define SWEET_DT_DISPLAY_NODE   "disp0"
#define SWEET_DT_FBCON_NODE     "sweet-fbcon"

/*
 * Read one 32-bit property of the ADT node with this device-tree name
 * ("name" property value, e.g. "disp0").  Returns 1 when the property exists
 * and is exactly one cell, 0 otherwise.
 */
static inline int
sweet_dt_u32(const char *node_name, const char *prop, uint32_t *out)
{
	DTEntry entry;
	void const *value = NULL;
	unsigned int size = 0;

	if (SecureDTFindEntry("name", node_name, &entry) != kSuccess) {
		return 0;
	}
	if (SecureDTGetProperty(entry, prop, &value, &size) != kSuccess ||
	    value == NULL || size != sizeof(uint32_t)) {
		return 0;
	}
	*out = *(const uint32_t *)value;
	return 1;
}

/*
 * 1 when the ADT root "compatible" list contains this string.  mkadt.py puts
 * "qcom,sm7150" there for sweet and "qcom,msm8998" for chiron, after the
 * iPhone identity the userland comes from.
 */
static inline int
sweet_board_has_soc(const char *soc)
{
	DTEntry entry;
	void const *value = NULL;
	unsigned int size = 0;
	const char *list;
	unsigned int i;

	if (SecureDTLookupEntry(NULL, "/", &entry) != kSuccess ||
	    SecureDTGetProperty(entry, "compatible", &value, &size) != kSuccess ||
	    value == NULL) {
		return 0;
	}
	list = (const char *)value;
	for (i = 0; i + 1 < size; i += (unsigned int)strlen(list + i) + 1) {
		if (strcmp(list + i, soc) == 0) {
			return 1;
		}
	}
	return 0;
}

/*
 * The SM7150-specific drivers (touch over GENI SE7, PM6150 keys over SPMI,
 * the DWC3 wrapper at 0x0a600000 and the GCC at 0x00100000) must not touch
 * their registers on another SoC: those addresses belong to unrelated blocks
 * on MSM8998.  They ask this instead of assuming real hardware.
 */
static inline int
sweet_board_is_sweet(void)
{
	return sweet_board_has_soc(SWEET_BOARD_SM7150);
}

/* Same question for chiron, for a driver written for MSM8998 later on. */
static inline int
sweet_board_is_chiron(void)
{
	return sweet_board_has_soc(SWEET_BOARD_MSM8998);
}

/* Panel and presented screen geometry, in panel pixels. */
struct sweet_display_geometry {
	uint32_t panel_w, panel_h;      /* size of the panel memory */
	uint32_t logical_w, logical_h;  /* size reported to the userland */
	uint32_t dots_per_inch;
	int32_t  off_x, off_y;          /* logical origin of the panel */
};

/*
 * Work out what to present on a panel_w x panel_h frame buffer.
 *
 * The iOS userland only has layout and status-bar data for real iPhone
 * screens (UIKit: "No status bar visual provider for screen", red bar for
 * unknown point sizes), so the display is presented as the iPhone X panel,
 * 1125x2436 (375x812 points @3x), whatever the phone has, and the panel shows
 * the part of it that fits:
 *
 *   sweet  1080x2400 panel: centred, 22 px cropped left and right and 18 px
 *          top and bottom (canvas-anchor 0 in the device tree).
 *   chiron 1080x2160 panel: 22 px cropped left and right and top-aligned
 *          (canvas-anchor 1), so the status bar stays on screen and the last
 *          276 rows are off-panel.
 *
 * A panel that is larger than the iPhone X screen is presented at its own
 * size (nothing would be cropped, and the point size would be nonsense), and
 * boot-arg sweetfb_native=1 always presents the real panel size.
 */
static inline void
sweet_display_geometry_for(uint32_t panel_w, uint32_t panel_h,
    struct sweet_display_geometry *geom)
{
	uint32_t canvas_w = 0, canvas_h = 0, dpi = 0, anchor_top = 0;
	int native = 0;

	(void)sweet_dt_u32(SWEET_DT_DISPLAY_NODE, "canvas-width", &canvas_w);
	(void)sweet_dt_u32(SWEET_DT_DISPLAY_NODE, "canvas-height", &canvas_h);
	(void)sweet_dt_u32(SWEET_DT_DISPLAY_NODE, "canvas-anchor", &anchor_top);
	(void)sweet_dt_u32(SWEET_DT_DISPLAY_NODE, "dots-per-inch", &dpi);
	if (canvas_w == 0 || canvas_h == 0) {
		canvas_w = SWEET_DISPLAY_LOGICAL_W;
		canvas_h = SWEET_DISPLAY_LOGICAL_H;
	}
	if (dpi == 0) {
		dpi = SWEET_DISPLAY_DPI;
	}
	(void)PE_parse_boot_argn("sweetfb_native", &native, sizeof(native));

	if (native || panel_w > canvas_w || panel_h > canvas_h) {
		canvas_w = panel_w;
		canvas_h = panel_h;
		anchor_top = 0;
	}
	geom->panel_w = panel_w;
	geom->panel_h = panel_h;
	geom->logical_w = canvas_w;
	geom->logical_h = canvas_h;
	geom->dots_per_inch = dpi;
	geom->off_x = ((int32_t)canvas_w - (int32_t)panel_w) / 2;
	if (panel_h >= canvas_h || !anchor_top) {
		geom->off_y = ((int32_t)canvas_h - (int32_t)panel_h) / 2;
	} else {
		geom->off_y = 0;                /* top-aligned: crop the bottom */
	}
}

/*
 * Panel size as recorded in the ADT (mkadt.py writes panel-width/panel-height
 * from the same table the shim uses); fallback for an older tree.
 */
static inline void
sweet_panel_size(uint32_t *w, uint32_t *h)
{
	uint32_t pw = 0, ph = 0;

	if (!sweet_dt_u32(SWEET_DT_DISPLAY_NODE, "panel-width", &pw) ||
	    !sweet_dt_u32(SWEET_DT_DISPLAY_NODE, "panel-height", &ph)) {
		(void)sweet_dt_u32(SWEET_DT_FBCON_NODE, "width", &pw);
		(void)sweet_dt_u32(SWEET_DT_FBCON_NODE, "height", &ph);
	}
	*w = pw;
	*h = ph;
}

#endif /* _PEXPERT_SWEETBOARD_H */
