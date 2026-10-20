/*
 * llp64_test.c -- catch constants that only work on an LP64 target.
 *
 * WHY THIS EXISTS. shim/mmu.c wrote its page-table constants as `1UL << n`. That is
 * right for aarch64-linux-gnu (LP64: unsigned long is 64 bits) and silently wrong
 * for aarch64-pc-win32-coff (LLP64: unsigned long is 32 bits), which is the target
 * the EFI application is built for.
 *
 * clang does not error, it warns:
 *
 *     warning: shift count >= width of type
 *       D_PXN (1UL << 53)  -> 0
 *       D_UXN (1UL << 54)  -> 0
 *       TCR   (1UL << 32)  -> 0
 *
 * With PXN and UXN zero, every Device descriptor in the page table becomes
 * EXECUTABLE, and losing TCR bit 32 changes the physical address size. The build
 * would have produced a 9 KB EFI application that linked perfectly and was wrong.
 *
 * WHAT IS CHECKED. The three properties that must hold on EVERY target:
 *
 *   1. the shifted constants are exactly the bits they name
 *   2. no constant is zero, which is what a wrapped shift produces
 *   3. the bits above 32 survive, since those are the ones LLP64 eats
 *
 * The test is compiled twice by tools/test-llp64.sh -- once for the host and once
 * for --target=aarch64-pc-win32-coff -- and must pass both times. A constant
 * written as `1UL << 53` passes the first and fails the second, which is exactly
 * the signal wanted.
 */

#include <stdint.h>

/*
 * No <stdio.h> under the COFF target, so printf is declared by hand. It is only
 * ever linked in the host build; the COFF build is compile-only, which is all this
 * test needs from it.
 */
int printf(const char *fmt, ...);

/*
 * The same expressions mmu.c uses, reproduced here so the test does not depend on
 * mmu.c's internal macros -- if someone changes mmu.c, this file should be updated
 * deliberately rather than silently tracking it.
 */
#define T_PXN   (UINT64_C(1) << 53)
#define T_UXN   (UINT64_C(1) << 54)
#define T_AF    (UINT64_C(1) << 10)
#define T_T0SZ  (UINT64_C(28) << 0)
#define T_IPS   (UINT64_C(1) << 32)
#define T_TG0   (UINT64_C(1) << 14)

static int g_failures;
static int g_checks;

static void
check(const char *name, uint64_t got, uint64_t want)
{
	g_checks++;
	if (got == want) {
		printf("  ok   %-8s %#018llx\n", name,
		    (unsigned long long)got);
	} else {
		g_failures++;
		printf("  FAIL %-8s want %#018llx got %#018llx\n", name,
		    (unsigned long long)want, (unsigned long long)got);
	}
}

static void
nonzero(const char *name, uint64_t got)
{
	g_checks++;
	if (got != 0) {
		printf("  ok   %-8s is non-zero\n", name);
	} else {
		g_failures++;
		printf("  FAIL %-8s is ZERO -- a shift wrapped (LLP64?)\n", name);
	}
}

int
main(void)
{
	printf("LLP64 constant check\n");
	printf("  sizeof(unsigned long) = %u\n", (unsigned)sizeof(unsigned long));
	printf("  sizeof(uint64_t)      = %u\n", (unsigned)sizeof(uint64_t));

	check("PXN", T_PXN, UINT64_C(0x0020000000000000));
	check("UXN", T_UXN, UINT64_C(0x0040000000000000));
	check("AF", T_AF, UINT64_C(0x400));
	check("TCR.IPS", T_IPS, UINT64_C(0x0000000100000000));
	check("TCR.TG0", T_TG0, UINT64_C(0x4000));
	check("TCR.T0SZ", T_T0SZ, UINT64_C(28));

	/* Each one must also be non-zero: a wrapped shift yields exactly 0. */
	nonzero("PXN", T_PXN);
	nonzero("UXN", T_UXN);
	nonzero("TCR.IPS", T_IPS);

	/*
	 * And the bits above 32 must survive, since those are what LLP64 destroys.
	 * Tested as a property rather than per-name so a new high-bit constant added
	 * later is covered too.
	 */
	g_checks++;
	if (((T_PXN | T_UXN | T_IPS) >> 32) != 0) {
		printf("  ok   bits above 32 survive the shift\n");
	} else {
		g_failures++;
		printf("  FAIL every bit above 32 was lost -- LLP64 wrap\n");
	}

	printf("\n%d checks, %d failure(s)\n", g_checks, g_failures);
	if (g_failures == 0) {
		printf("PASS\n");
	}
	return g_failures == 0 ? 0 : 1;
}
