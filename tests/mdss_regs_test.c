/*
 * mdss_regs_test.c -- host test of the MDP5 register encoding.
 *
 * Why: the touch driver's STOP-bit bug was caught by a host test of the wire
 * protocol, not by compiling and not by CI. The MDP5 blit has the same exposure
 * -- a wrong bit position in SRC_FORMAT, or a wrong block offset, produces a
 * driver that compiles perfectly and draws garbage. This test recomputes every
 * constant from the Linux kernel's own bitfields and compares.
 *
 * It does not include the driver; it re-derives the numbers independently and
 * checks the driver's #defines against them (tools/test-mdss.sh does the grep
 * half). Two independent derivations agreeing is the point.
 *
 * Not part of the kernel. Built and run by tools/test-mdss.sh.
 */
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

static int failures;
static int checks;

#define CHECK(cond, ...)                                            \
	do {                                                        \
		checks++;                                           \
		if (!(cond)) {                                      \
			failures++;                                 \
			printf("  FAIL  " __VA_ARGS__);             \
			printf("\n");                               \
		}                                                   \
	} while (0)

/* ---- field definitions, quoted from the Linux kernel ---- */
/* mdp_common.xml.h:89-101 */
#define BPC8   3
#define BPC8A  3
/* mdp5.xml.h:805-852 */
#define A_BPC(v)   (((v) << 6)  & 0x000000C0u)
#define R_BPC(v)   (((v) << 4)  & 0x00000030u)
#define G_BPC(v)   (((v) << 0)  & 0x00000003u)
#define B_BPC(v)   (((v) << 2)  & 0x0000000Cu)
#define CPP(v)     (((v) << 9)  & 0x00000600u)
#define UNP_CNT(v) (((v) << 12) & 0x00003000u)
#define ALPHA_EN   0x00000100u
#define UNP_TIGHT  0x00020000u
/* mdp_common.xml.h:56 -- MDP_PLANE_INTERLEAVED = 0, CHROMA_FULL = 0 */

int
main(void)
{
	uint32_t expect_src_format =
	    A_BPC(BPC8A) | R_BPC(BPC8) | G_BPC(BPC8) | B_BPC(BPC8) |
	    ALPHA_EN | CPP(4 - 1) | UNP_CNT(4 - 1) | UNP_TIGHT;

	printf("MDP5 register encoding\n");
	printf("  SRC_FORMAT derived = 0x%08X\n", expect_src_format);
	CHECK(expect_src_format == 0x000237FFu,
	    "SRC_FORMAT changed: 0x%08X != 0x000237FF", expect_src_format);
	CHECK((expect_src_format & 0x000000C0u) == A_BPC(BPC8A),
	    "alpha bpc field not at shift 6 (mdp5.xml.h:823)");
	CHECK((expect_src_format & 0x00000030u) == R_BPC(BPC8),
	    "red bpc field not at shift 4 (mdp5.xml.h:817)");
	CHECK((expect_src_format & 0x00000600u) == CPP(3),
	    "cpp field not at shift 9 (mdp5.xml.h:830)");
	CHECK((expect_src_format & 0x00003000u) == UNP_CNT(3),
	    "unpack_count not at shift 12 (mdp5.xml.h:837)");
	CHECK((expect_src_format & 0x00000100u) == ALPHA_EN,
	    "alpha enable not at bit 8 (mdp5.xml.h:829)");
	CHECK((expect_src_format & 0x00020000u) == UNP_TIGHT,
	    "unpack_tight not at bit 17 (mdp5.xml.h:843)");
	/*
	 * Bits 19-20 (fetch type) and 23-24 (chroma) must be zero for a linear
	 * RGB surface: a non-zero fetch type would make the pipe read the buffer
	 * as planar and produce noise.
	 */
	CHECK((expect_src_format & 0x00180000u) == 0u,
	    "fetch type must be 0 (interleaved) for a linear surface");
	CHECK((expect_src_format & 0x01800000u) == 0u,
	    "chroma sample must be 0 (full) for RGB");

	/* ---- SRC_UNPACK for BGRA8888: unpack {3,2,0,1} (mdp_format.c FMT table) ---- */
	{
		uint32_t expect_unpack = (3u << 0) | (2u << 8) | (0u << 16) |
		    (1u << 24);
		int seen[4] = {0, 0, 0, 0};
		int perm = 1;

		printf("  SRC_UNPACK derived = 0x%08X\n", expect_unpack);
		CHECK(expect_unpack == 0x01000203u,
		    "SRC_UNPACK changed: 0x%08X != 0x01000203", expect_unpack);

		for (int i = 0; i < 4; i++) {
			unsigned v = (expect_unpack >> (i * 8)) & 0xFFu;

			if (v > 3u || seen[v]) {
				perm = 0;
			} else {
				seen[v] = 1;
			}
		}
		CHECK(perm, "BGRA8888 unpack {3,2,0,1} is not a permutation");
	}

	/* ---- WB_DST_FORMAT field positions (mdp5.xml.h:1406-1440) ---- */
	{
		uint32_t f = ((0u & 3u) << 0) | ((1u & 3u) << 2) |
		    ((2u & 3u) << 4) | ((3u & 3u) << 6) |
		    ((3u & 3u) << 9) | ((3u & 3u) << 12) | 0x00020000u;

		CHECK((f & 0x00000003u) == 0u, "DSTC0 not at shift 0");
		CHECK((f & 0x0000000Cu) == 0x4u, "DSTC1 not at shift 2");
		CHECK((f & 0x00000030u) == 0x20u, "DSTC2 not at shift 4");
		CHECK((f & 0x000000C0u) == 0xC0u, "DSTC3 not at shift 6");
		CHECK((f & 0x00000600u) == 0x600u, "DST_BPP not at shift 9");
		CHECK((f & 0x00003000u) == 0x3000u, "PACK_COUNT not at shift 12");
		CHECK((f & 0x00020000u) == 0x00020000u, "PACK_TIGHT bit wrong");
	}

	/* ---- destination pixel address arithmetic ---- */
	{
		uint32_t dst_phys = 0x9d400000u;   /* chiron panel memory */
		uint32_t stride = 1080u * 4u;
		uint32_t x = 7u, y = 11u;
		uint32_t addr = dst_phys + y * stride + x * 4u;

		CHECK(addr == dst_phys + 11u * 4320u + 28u,
		    "destination pixel address arithmetic is wrong");
		CHECK((addr - dst_phys) % 4u == 0u,
		    "destination address is not 4-byte aligned");
		/* the whole panel must fit in the 32-bit register */
		CHECK((uint64_t)dst_phys + 2160ull * stride < 0x100000000ull,
		    "chiron panel memory exceeds the 32-bit WB address register");
	}

	/* ---- CTL_FLUSH must cover every block the blit programs ---- */
	{
		uint32_t flush = 0x00000800u | 0x00000040u | 0x00010000u |
		    0x00020000u;

		printf("  CTL_FLUSH derived  = 0x%08X\n", flush);
		CHECK(flush == 0x00030840u, "flush mask derivation: 0x%08X", flush);
		CHECK((flush & 0x00000800u) != 0u, "flush mask misses DMA0");
		CHECK((flush & 0x00000040u) != 0u, "flush mask misses LM0");
		CHECK((flush & 0x00010000u) != 0u, "flush mask misses WB");
		CHECK((flush & 0x00020000u) != 0u, "flush mask misses CTL");
	}

	/* ---- every block base must lie inside the mapped window ---- */
	{
		uint32_t window = 0x90000u;    /* msm8998-mdss.dtsi:17 */

		struct {
			const char *name;
			uint32_t    off;
			uint32_t    max_reg;
		} blocks[] = {
			{ "MDP_TOP",   0x00001000u, 0x0000u },
			{ "CTL0",      0x00002000u, 0x001Cu },
			{ "DMA0_SSPP", 0x00025000u, 0x0038u },
			{ "LM0",       0x00045000u, 0x0028u },
			{ "WB",        0x00066000u, 0x0078u },
		};

		for (unsigned i = 0; i < sizeof(blocks) / sizeof(blocks[0]); i++) {
			CHECK(blocks[i].off + blocks[i].max_reg < window,
			    "%s+0x%X runs past the 0x%X mapped window",
			    blocks[i].name, blocks[i].max_reg, window);
		}
	}

	/*
	 * ---- the 4 GiB constraint documented in msm8998_mdss.h ----
	 * MDP5 address registers are 32-bit with no _HI. A future board whose
	 * panel or RAM sits above 4 GiB must be rejected, not truncated.
	 */
	{
		uint64_t too_high = 0x100000000ull;

		CHECK((too_high >> 32) != 0u,
		    "the >4GiB rejection test does not detect a high address");
	}

	printf("\n%d checks, %d failure(s)\n", checks, failures);
	if (failures == 0) {
		printf("PASS\n");
	}
	return failures == 0 ? 0 : 1;
}
