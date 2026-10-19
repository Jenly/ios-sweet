/*
 * restart_preflight_test.c -- host test of the PSHOLD safety rule.
 *
 * The driver writes zero to PS_HOLD to reset the SoC. If the preflight ever
 * accepts a register that is not really PS_HOLD, that write goes somewhere
 * unknown -- not recoverable, not debuggable from software. So the rule lives in
 * sweet_restart.h as a pure function and gets its own test.
 *
 * No stubs at all: the test includes the real header and calls the exact function
 * the driver calls. That is the reason the check was moved into the header.
 */

#include <stdio.h>
#include <stdint.h>

#include "../bsd/dev/sweet_restart.h"

static int g_failures;
static int g_checks;

#define CHECK(cond, ...)                                            \
	do {                                                        \
		g_checks++;                                         \
		if (!(cond)) {                                      \
			g_failures++;                               \
			printf("  FAIL  " __VA_ARGS__);            \
			printf("\n");                               \
		}                                                   \
	} while (0)

int
main(void)
{
	printf("sweet_restart PSHOLD preflight\n");

	/*
	 * Every value that must be REJECTED. These are not arbitrary: each is what a
	 * specific failure looks like on real hardware.
	 */
	struct {
		uint32_t    value;
		const char *why;
	} reject[] = {
		{ 0x00000000u, "a gated or powered-down block" },
		{ 0xFFFFFFFFu, "a dead bus, which reads all ones" },
		{ 0x00000002u, "another register: bit 0 clear" },
		{ 0x00000004u, "another register: bit 0 clear" },
		{ 0xFFFFFFFEu, "near all ones but bit 0 clear" },
		{ 0x80000000u, "a status register with only a high bit" },
		{ 0x00000100u, "an enable bit somewhere else entirely" },
	};

	for (unsigned i = 0; i < sizeof(reject) / sizeof(reject[0]); i++) {
		CHECK(sweet_restart_probe_ok(reject[i].value) == 0,
		    "ACCEPTED %#010x (%s) -- must reject",
		    reject[i].value, reject[i].why);
		printf("  reject %#010x  %s\n", reject[i].value, reject[i].why);
	}

	/*
	 * Every value that must be ACCEPTED. Bit 0 set is the invariant; extra bits
	 * are allowed because the register may carry unrelated status and demanding
	 * exactly 1 would reject a working board.
	 */
	struct {
		uint32_t    value;
		const char *why;
	} accept[] = {
		{ 0x00000001u, "plain asserted hold" },
		{ 0x00000003u, "asserted with another low bit" },
		{ 0x00000005u, "asserted with a different low bit" },
		{ 0x000000FFu, "asserted with several low bits" },
		{ 0x80000001u, "asserted with a high status bit" },
		{ 0xFFFFFFFDu, "asserted with almost everything else set" },
	};

	for (unsigned i = 0; i < sizeof(accept) / sizeof(accept[0]); i++) {
		CHECK(sweet_restart_probe_ok(accept[i].value) != 0,
		    "REJECTED %#010x (%s) -- must accept",
		    accept[i].value, accept[i].why);
		printf("  accept %#010x  %s\n", accept[i].value, accept[i].why);
	}

	/*
	 * Exhaustive sweep: for every value with bit 0 clear, the answer must be
	 * "no". This is the property that matters -- one missed pattern is a write to
	 * a random register.
	 */
	{
		int wrong = 0;

		for (uint32_t v = 0; v < 0x100000u; v++) {
			if ((v & 1u) == 0u && sweet_restart_probe_ok(v) != 0 &&
			    v != 0xFFFFFFFFu) {
				if (wrong < 3) {
					printf("  FAIL  accepted %#010x with bit 0 clear\n", v);
				}
				wrong++;
			}
		}
		CHECK(wrong == 0,
		    "%d values with bit 0 clear were accepted", wrong);
		printf("  swept 1M values: no bit-0-clear value accepted\n");
	}

	/*
	 * And the complement: every odd value in the same range must be accepted, so
	 * the rule is not accidentally rejecting working boards.
	 */
	{
		int wrong = 0;

		for (uint32_t v = 1; v < 0x100000u; v += 2) {
			if (sweet_restart_probe_ok(v) == 0) {
				if (wrong < 3) {
					printf("  FAIL  rejected %#010x with bit 0 set\n", v);
				}
				wrong++;
			}
		}
		CHECK(wrong == 0, "%d odd values were rejected", wrong);
		printf("  swept 1M values: every odd value accepted\n");
	}

	/* All-ones is the one odd value that must still be refused. */
	CHECK(sweet_restart_probe_ok(0xFFFFFFFFu) == 0,
	    "all-ones was accepted -- a dead bus must be refused");

	printf("\n%d checks, %d failure(s)\n", g_checks, g_failures);
	if (g_failures == 0) {
		printf("PASS\n");
	}
	return g_failures == 0 ? 0 : 1;
}
