/*
 * earlylog.h -- draw on the panel before anything else can fail.
 *
 * See earlylog.c for why this exists. Short version: the shim's first act used to be
 * a 2.3-million-store full-screen fill, so a failure anywhere in it looked exactly
 * like a shim that never started. These functions paint a small block instead, from
 * the first few hundred instructions.
 *
 * Colours are dim on purpose: a phone stuck on a bright screen drains itself flat
 * while you are reading the log.
 */

#ifndef SWEET_EARLYLOG_H
#define SWEET_EARLYLOG_H

#include <stdint.h>

#define EARLY_STARTED	0xff102040U	/* dark blue   -- shim is executing     */
#define EARLY_CONSOLE	0xff104020U	/* dark green  -- console is up         */
#define EARLY_DTB	0xffc0a000U	/* amber       -- DTB read, about to go */
#define EARLY_FAILED	0xffa02020U	/* dark red    -- something failed      */

/* Paint one 320x96 block at the top-left. About 30k stores. */
void early_block(uint32_t colour);

/* Paint n 96x96 blocks across the top, as a stage number without a font. */
void early_blocks(uint32_t n, uint32_t colour);

#endif /* SWEET_EARLYLOG_H */
