#pragma once
#include <stdint.h>

/*
 * Per-SoC constants. tools/build.sh regenerates devcfg.h from
 * tools/devices.py for the requested --device (default sweet) before
 * compiling; the committed copy is the Redmi Note 10 Pro one, so a bare
 * `gcc` compile of console.c/host_test.c still works.
 */
#include "devcfg.h"

#define FB_BASE		DCFG_FB_BASE	/* reserved-memory splash region */
#define FB_W		DCFG_FB_W
#define FB_H		DCFG_FB_H
#define FB_STRIDE	DCFG_FB_STRIDE	/* a8r8g8b8, matches mainline simplefb */

#define RED	0xffff0000U
#define BLUE	0xff0000a0U
#define WHITE	0xffffffffU
#define YELLOW	0xffffff00U
#define GREEN	0xff00ff00U

extern volatile uint32_t *con_fb;

/* Clear the band the text uses. Cheap enough to do before anything else. */
void con_clear(uint32_t bg);
/* Clear the entire panel. 2.3 million Device-memory stores: use deliberately. */
void con_clear_full(uint32_t bg);
void con_skip(void);
void con_str(const char *label, const char *s);
void con_hex(const char *label, uint64_t v);
/* label, then "a=xxxxxxxx b=xxxxxxxx" style pairs of 32-bit values */
void con_hex2(const char *label, const char *a, uint32_t va, const char *b, uint32_t vb);
