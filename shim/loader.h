#pragma once
#include <stdint.h>

/* Boot the SWEETXNU payload found in the ABL ramdisk; returns only on error. */
const char *loader_boot(uintptr_t dtb);

/*
 * Same, but with the payload at an address the caller chose. For a caller that
 * gets the payload somewhere other than the bootloader's ramdisk.
 *
 * REQUIRES the MMU and caches to be off. An EFI application cannot call it until
 * it has made that true; see shim/efi/README.md.
 */
const char *loader_boot_payload(uintptr_t dtb, uintptr_t payload_pa,
    uint64_t payload_len);
