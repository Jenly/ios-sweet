#pragma once
#include <stdint.h>

/*
 * Copy verification and RAM reporting for the shim.
 *
 * Both exist because the kernel produces no output and I need to know whether the copy
 * landed before blaming the kernel. See verify.c for the reasoning.
 */

/* Print the /memory nodes the bootloader passed. Ground truth for this handset. */
void verify_memory_report(uintptr_t dtb);

/*
 * Compare `samples` evenly spaced 8-byte words between src and dst over `len` bytes.
 * Returns the number of mismatches; 0 means the copy looks correct.
 */
uint32_t verify_copy(uintptr_t src, uintptr_t dst, uint64_t len, uint32_t samples);