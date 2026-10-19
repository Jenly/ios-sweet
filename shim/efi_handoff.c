/*
 * efi_handoff.c -- enter the kernel from an EFI-loaded image.
 *
 * THE PROBLEM THIS SOLVES. loader_boot_payload() writes memory in aligned 8-byte
 * words and ends with an I-cache invalidate, because with the MMU off every byte of
 * memory is Device memory and unaligned access faults. An EFI application runs
 * with the MMU and caches ON -- that is part of the UEFI contract. The kernel must
 * be entered with them off, and the instruction after `msr sctlr_el1` cannot be
 * fetched through a translation that has just been removed.
 *
 * THE SHAPE OF THE ANSWER. mmu_enable() (shim/mmu.c) builds a 36-bit
 * **identity-mapped** space: VA == PA everywhere. That is the property that makes
 * the switch survivable -- after the translation is gone, every address still
 * means the same thing. Two consequences shape the code below:
 *
 *  1. The trampoline MUST live inside this image. mmu_enable() maps
 *     `_head .. _end` Normal write-back and **everything else Device, with PXN and
 *     UXN set** -- i.e. permanently execute-never. Code copied anywhere outside
 *     this image would stop being executable the moment our map took over. So the
 *     trampoline is a function of this image, not a blob copied to scratch.
 *
 *  2. Because the map is an identity map, every function pointer in this image is
 *     already a physical address. Nothing needs translating before the jump.
 *
 * WHY sctlr_el1 IS NOT WRITTEN WITH xzr. The obvious "turn everything off" is
 * `msr sctlr_el1, xzr`, and it is wrong: SCTLR_EL1 has RES1 bits that an
 * implementation may require to stay set, and clearing them is CONSTRAINED
 * UNPREDICTABLE. The sequence below reads the register and clears exactly the three
 * bits that matter -- M (MMU), C (data cache), I (instruction cache) -- leaving
 * every other bit as the firmware left it.
 *
 * WHAT IS NOT VERIFIED. This code has never run. It is compiled and its
 * instructions are checked by disassembly (tools/test-efi-handoff.sh), which proves
 * the encoding is what was intended and proves nothing about the hardware. The
 * first three things to check on a real boot are listed in shim/efi/README.md.
 */

#include <stdint.h>
#include "console.h"
#include "loader.h"
#include "mmu.h"

typedef void (*kernel_entry_fn)(uint64_t boot_args_pa);

/*
 * Turn the MMU and caches off, then branch to `fn` with x0/x1/x2 = a0/a1/a2.
 *
 * Entered with the MMU ON, executing from this image under our own identity map.
 * Does not return: the callee either enters the kernel or fails with no console
 * left to print to -- the framebuffer belongs to the kernel from here on.
 *
 * The branch is indirect through a register on purpose. A direct `b` would be
 * resolved at link time against the virtual address; those are equal here, but an
 * indirect branch states the intent and keeps working if this image is ever linked
 * at a nonzero physical base.
 *
 * `noinline` because this must be a real call and not folded into the caller: the
 * caller's own return path lives in a frame we are about to make unreadable.
 */
__attribute__((noinline, noreturn))
static void
drop_mmu_and_jump(uintptr_t fn, uint64_t a0, uint64_t a1, uint64_t a2)
{
	__asm__ volatile(
		/* Everything the caller wrote must be visible before the caches go. */
		"dsb sy\n\t"
		/* Clear exactly M (bit 0), C (bit 2) and I (bit 12). */
		"mrs x9, sctlr_el1\n\t"
		"bic x9, x9, #(1 << 0)\n\t"
		"bic x9, x9, #(1 << 2)\n\t"
		"bic x9, x9, #(1 << 12)\n\t"
		"msr sctlr_el1, x9\n\t"
		/*
		 * The ISB is what makes the new translation regime take effect. It is
		 * fetched before the change is visible; the fetch after it uses physical
		 * addressing, which lands on the same bytes because the map was an
		 * identity map.
		 */
		"isb\n\t"
		"mov x0, %1\n\t"
		"mov x1, %2\n\t"
		"mov x2, %3\n\t"
		"br %0\n\t"
		:
		: "r"(fn), "r"(a0), "r"(a1), "r"(a2)
		: "x9", "x0", "x1", "x2", "memory");
	__builtin_unreachable();
}

/*
 * Second entry point for an EFI application: the payload is already somewhere in
 * memory and the caller knows where.
 *
 * `shim_pa` is the physical address this image was loaded at, which the EFI
 * application must supply because firmware chooses it. It is used only to check
 * that we are where the caller thinks we are -- a mismatch means the map built
 * below would not cover the running code, and that is worth one comparison rather
 * than a silent hang.
 *
 * Returns only on error, before the point of no return.
 */
const char *
efi_handoff(uintptr_t shim_pa, uintptr_t shim_len, uintptr_t dtb,
    uintptr_t payload_pa, uint64_t payload_len)
{
	struct mmu_info info;

	con_str("EFI", "handoff: entering the kernel from an EFI-loaded image");
	con_hex2("EFI", "shim_pa=", shim_pa, "dtb=", dtb);
	con_hex2("EFI", "payload=", payload_pa, "len=", payload_len);

	/*
	 * A PE image is not one contiguous range, so its extent has to be supplied:
	 * there are no `_head`/`_end` equivalents in a PE link and efi_stubs.c's
	 * placeholders are one byte. Refusing here is better than mapping a one-byte
	 * "image" execute-never and faulting on the next fetch.
	 */
	if (shim_pa == 0 || shim_len == 0) {
		return "the image's own extent was not supplied; cannot map it";
	}
	mmu_image_lo = shim_pa;
	mmu_image_hi = shim_pa + shim_len;

	/*
	 * mmu_enable() maps the image Normal write-back and the rest
	 * Device/execute-never. If the supplied range does not contain the code we
	 * are executing, enabling the map makes the next instruction fetch fail.
	 */
	if (mmu_enable(&info) != 0) {
		return "mmu_enable failed: the image spans more than two 32MB blocks";
	}
	con_hex2("EFI", "img_lo=", info.img_lo, "img_hi=", info.img_hi);

	if (shim_pa < info.img_lo || shim_pa >= info.img_hi) {
		/*
		 * The caller's idea of where this image lives is outside the range just
		 * mapped executable. Say so and stop: continuing would fault at the next
		 * instruction fetch with nothing able to report it.
		 */
		return "shim_pa is outside the mapped image range; refusing to jump";
	}

	/*
	 * From here there is no console: the kernel owns the framebuffer next. The
	 * function pointer is this image's address, which equals its physical address
	 * under the identity map built above.
	 */
	con_str("EFI", "dropping the MMU and entering the kernel");
	drop_mmu_and_jump((uintptr_t)&loader_boot_payload, dtb, payload_pa,
	    payload_len);
}
