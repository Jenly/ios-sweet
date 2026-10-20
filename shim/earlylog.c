/*
 * earlylog.c -- draw something on the panel as early as physically possible.
 *
 * WHY THIS EXISTS. `fastboot boot` reported OKAY and the phone sat with an unchanged
 * screen. That outcome is indistinguishable from a hundred causes, and the shim's
 * console could not help: con_clear() fills 1080x2160 pixels, and with the MMU off
 * every store is a Device-memory access, so the first visible pixel can be tens of
 * millions of cycles away. If something is wrong before or during that fill, the
 * panel never changes and there is no evidence at all.
 *
 * So this writes a small solid block FIRST, from the first few hundred instructions,
 * touching nothing that could plausibly fault: no clearing, no font, no strings, no
 * division. If the block appears, the load address, the framebuffer address and the
 * store path are all correct. If it does not, the problem is upstream of all three
 * and no logging inside the shim would have helped.
 *
 * Each stage repaints the block a different colour, so the panel becomes the
 * progress indicator:
 *
 *   nothing       never executed -- wrong load address, or wrong FB address
 *   dark blue     early log reached, about to call con_clear()
 *   dark green    console cleared, about to read the DTB
 *   amber         DTB read, about to enter the payload
 *   dark red      failed after the console was up (message is on screen)
 *
 * The colours are dim deliberately: a phone stuck on a bright white screen drains
 * itself flat while you are reading the log.
 */

#include "console.h"
#include "earlylog.h"

/*
 * Fill a w x h rectangle. Kept trivial on purpose: aligned 32-bit stores, nothing
 * that can fault for an interesting reason. FB_BASE is 2 MiB-aligned and every
 * offset here is a multiple of 4, so the pointer arithmetic stays aligned even with
 * the MMU off and -mstrict-align on.
 */
static void
fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t colour)
{
	volatile uint32_t *base = (volatile uint32_t *)FB_BASE;

	for (uint32_t row = 0; row < h; row++) {
		volatile uint32_t *p = base + (y + row) * (FB_STRIDE / 4) + x;

		for (uint32_t col = 0; col < w; col++) {
			p[col] = colour;
		}
	}
}

void
early_block(uint32_t colour)
{
	/* 320x96 in the top-left: about 30k stores, and unmistakable at 1080 wide. */
	fill_rect(0, 0, 320, 96, colour);
}

void
early_blocks(uint32_t n, uint32_t colour)
{
	if (n > 8) {
		n = 8;
	}
	for (uint32_t i = 0; i < n; i++) {
		fill_rect(i * 128, 0, 96, 96, colour);
	}
}
