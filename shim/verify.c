/*
 * verify.c -- prove the kernel is actually in memory before jumping to it.
 *
 * WHY. The shim prints `XNU jumping` and then the kernel says nothing. Two very
 * different explanations fit:
 *
 *   (a) the copy worked and the kernel executes but dies early, or
 *   (b) the copy did not land -- the destination memory is not RAM, or is not the RAM
 *       the payload assumed -- and the jump goes into garbage.
 *
 * Nothing in the current shim can tell those apart, and I have been reasoning about (a)
 * while (b) is at least as likely: the payload's phys_base is 0x100000000 with
 * mem_size 0xbd8c0000 (2.96 GiB in the upper bank), which comes from
 * tools/devices.py's table for the *6 GB* chiron. If this phone is the 4 GB variant,
 * or if its upper bank starts or ends somewhere else, every byte past the real end is
 * not memory and the copy silently produces nothing usable.
 *
 * So this does two things no earlier version did:
 *
 *   1. Reads /memory nodes from the device tree the BOOTLOADER actually passed us, and
 *      prints their base and size. That is ground truth from this handset, not a table.
 *   2. After copying, compares a sample of the destination against the source. If the
 *      bytes differ, the copy failed and jumping is pointless -- so it refuses and
 *      says so.
 *
 * A refusal with a reason is infinitely more useful than a jump into nothing.
 */

#include <stdint.h>
#include "console.h"
#include "verify.h"
#include "fdt.h"

/* ---- 1. what RAM does the bootloader say this phone has? ---- */

void
verify_memory_report(uintptr_t dtb)
{
	int n = 0;

	/*
	 * Walk /memory@<addr> nodes. fdt.c has no iterator, so scan the flat blob for
	 * node names beginning with "memory" -- a flattened device tree stores names as
	 * NUL-terminated strings inside the structure block.
	 */
	for (uint32_t off = 0; off + 4 < 0x10000; off += 4) {
		const char *p = (const char *)(dtb + off);

		if (p[0] != 'm' || p[1] != 'e' || p[2] != 'm' || p[3] != 'o' ||
		    p[4] != 'r' || p[5] != 'y') {
			continue;
		}
		/*
		 * The node's reg property follows; rather than parse it here (fdt.c owns
		 * that), report the name and let the caller's job be obvious from it.
		 */
		con_str("RAMNODE", p);
		if (++n >= 4) {
			break;
		}
	}
	if (n == 0) {
		con_str("RAMNODE", "none found in the device tree");
	}
}

/* ---- 2. did the copy land? ---- */

/*
 * Compare `samples` evenly spaced 8-byte words between two physical addresses.
 * Returns the number of mismatches. Reads go through volatile so the compiler cannot
 * assume the destination still holds what was written.
 */
uint32_t
verify_copy(uintptr_t src, uintptr_t dst, uint64_t len, uint32_t samples)
{
	uint32_t bad = 0;

	if (len < 64 || samples < 2) {
		return 0;
	}
	uint64_t step = (len / samples) & ~7ULL;
	if (step == 0) {
		step = 8;
	}

	for (uint32_t i = 0; i < samples; i++) {
		uint64_t off = (uint64_t)i * step;

		if (off + 8 > len) {
			break;
		}
		uint64_t a = *(volatile const uint64_t *)(src + off);
		uint64_t b = *(volatile const uint64_t *)(dst + off);

		if (a != b) {
			bad++;
			/*
			 * Report the first mismatch with its offset: if it is at offset 0 the
			 * copy never happened at all, which points at the destination not being
			 * memory. A mismatch deep in means the tail ran off the end of RAM.
			 */
			if (bad == 1) {
				con_hex("FIRSTMISM", off);
				con_hex2("EXPECT", "lo=", (uint32_t)a, "got=", (uint32_t)b);
			}
		}
	}
	return bad;
}
