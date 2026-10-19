#pragma once
#include <stdint.h>
#include "devcfg.h"

#define FB_PHYS	DCFG_FB_BASE

struct mmu_info {
	uint64_t img_lo, img_hi, ttbr;
	uint64_t sctlr_before, sctlr_after;
	int n_l3;
};

/*
 * Set both before mmu_enable() to override the image extent taken from `_head` and
 * `_end`. For a caller whose linker does not provide those symbols, such as an EFI
 * application linked as PE32+. Leaving them zero keeps the default behaviour.
 */
extern uintptr_t mmu_image_lo, mmu_image_hi;

int mmu_enable(struct mmu_info *info);
uint64_t mmu_probe(uintptr_t va);
