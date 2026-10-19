/*
 * efi_stubs.c -- symbols the PE linker cannot provide.
 *
 * mmu.c takes the running image's extent from `_head` and `_end`, which shim.ld
 * supplies for the raw Image build. A PE32+ application has no link script, so
 * those names do not exist. They are referenced even when the override
 * (mmu_image_lo/hi) is in use, because a C conditional still names both operands
 * -- the linker sees the reference regardless.
 *
 * Defining them here satisfies the link. They are deliberately EMPTY: the EFI path
 * must always set the override, because a PE's sections are not one contiguous
 * range and `&_head .. &_end` here would describe a single byte, not the image.
 * efi_handoff() therefore refuses to proceed if it was given no length, rather
 * than silently mapping a one-byte "image" execute-never.
 */

#include <stdint.h>

/*
 * Symbols, not variables: mmu.c does `(uintptr_t)_head`. An array provides an
 * address with no storage requirement beyond its own bytes.
 */
char _head[1];
char _end[1];
