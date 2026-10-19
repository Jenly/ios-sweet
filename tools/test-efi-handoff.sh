#!/bin/bash
# test-efi-handoff.sh -- verify the MMU-drop trampoline's instruction encoding.
#
# shim/efi_handoff.c cannot be executed without hardware. What CAN be checked is
# that the assembly it emits is the assembly intended, instruction by instruction,
# because the failure mode of getting it wrong is a silent hang with no console.
#
# Checks, all from the disassembly:
#   * `dsb sy` precedes everything -- writes must be visible before caches go
#   * SCTLR_EL1 is read first (mrs), so RES1 bits are preserved -- writing xzr is
#     CONSTRAINED UNPREDICTABLE and is the classic way to get this wrong
#   * exactly M (bit 0), C (bit 2), I (bit 12) are cleared, and nothing else
#   * an isb follows the msr, with nothing else between them
#   * the jump is indirect (br), so it cannot be a link-time virtual offset
#   * x0/x1/x2 carry the three arguments loader_boot_payload() expects
set -uo pipefail
cd "$(dirname "$0")/.."

OUT=${OUT:-/tmp/sweet-efi-handoff}
mkdir -p "$OUT"

CC=${CROSS_COMPILE:-aarch64-linux-gnu-}gcc
OBJDUMP=${CROSS_COMPILE:-aarch64-linux-gnu-}objdump

command -v "$CC" >/dev/null 2>&1 || { echo "no cross compiler ($CC)" >&2; exit 1; }
command -v "$OBJDUMP" >/dev/null 2>&1 || {
	echo "no cross objdump ($OBJDUMP)" >&2; exit 1; }

echo "EFI handoff trampoline"

# Compile just this file, with the shim's own flags. Note: NOT -nostdinc --
# shim/build.sh uses -ffreestanding -nostdlib and gets stdint.h from the cross
# compiler's headers, so -nostdinc breaks the include it actually relies on.
# -mstrict-align matters here too: it is what the shim is built with, and it
# changes how the compiler treats the memory around the MMU switch.
"$CC" -O2 -ffreestanding -nostdlib -fno-pic -mgeneral-regs-only -mstrict-align \
	-fno-stack-protector -Wall -Wextra -Ishim \
	-c shim/efi_handoff.c -o "$OUT/efi_handoff.o" 2>"$OUT/cc.log" || {
	echo "compile failed:" >&2
	cat "$OUT/cc.log" >&2
	exit 1
}

fail=0
check() {
	local desc=$1 want=$2 got=$3
	if [ "$want" = "$got" ]; then
		printf '  ok   %-46s %s\n' "$desc" "$got"
	else
		printf '  FAIL %-46s want %s got %s\n' "$desc" "$want" "$got"
		fail=$((fail + 1))
	fi
}

# --- isolate the trampoline: it is the function containing `msr sctlr_el1` ---
"$OBJDUMP" -d --no-show-raw-insn "$OUT/efi_handoff.o" > "$OUT/dis.txt"
"$OBJDUMP" -d "$OUT/efi_handoff.o" > "$OUT/dis-raw.txt"

# The whole-object disassembly is enough to look for the mrs/msr pair in order.
grep -q 'mrs[[:space:]]*x9, sctlr_el1' "$OUT/dis.txt" \
	|| { echo "  FAIL no 'mrs x9, sctlr_el1' -- SCTLR is not being read first"; fail=$((fail+1)); }
grep -q 'msr[[:space:]]*sctlr_el1, x9' "$OUT/dis.txt" \
	|| { echo "  FAIL no 'msr sctlr_el1, x9' -- RMW not used"; fail=$((fail+1)); }

# Writing xzr directly must NOT appear: that clears RES1 bits.
if grep -qE 'msr[[:space:]]*sctlr_el1, xzr' "$OUT/dis.txt"; then
	echo "  FAIL 'msr sctlr_el1, xzr' appears -- RES1 bits would be cleared"
	fail=$((fail+1))
else
	echo "  ok   SCTLR_EL1 is not overwritten with xzr"
fi

# --- the ordering, by address ---
# Extract the WHOLE trampoline, from its function label to the indirect branch.
# Starting the window at the `mrs` would miss a `dsb` placed before it, and a dsb
# before the sctlr read is precisely what this code needs.
SEQ=$(awk '/<drop_mmu_and_jump>:/{f=1} f{print} f&&/br[[:space:]]+x[0-9]+/{exit}' "$OUT/dis.txt")
printf '%s\n' "$SEQ" | sed 's/^/      /'

addr_of() { printf '%s\n' "$SEQ" | grep -E "$1" | head -1 | awk -F: '{print $1}'; }

A_DSB=$(addr_of 'dsb')
A_MRS=$(addr_of 'mrs[[:space:]]*x9, sctlr_el1')
A_MSR=$(addr_of 'msr[[:space:]]*sctlr_el1, x9')
A_BR=$(addr_of 'br[[:space:]]+x[0-9]+')

# Every instruction before the mrs is in the same function; a `dsb sy` there is the
# barrier that makes the caller's stores visible before the caches go.
BEFORE_MRS=$(printf '%s\n' "$SEQ" | awk -v m="$A_MRS" -F: '{a=$1+0; if (a<m) print}' )
check "a dsb precedes the sctlr read" yes \
	"$(printf '%s\n' "$BEFORE_MRS" | grep -q 'dsb' && echo yes || echo no)"

# Exactly one isb between the msr and the branch: the ISB is what makes the new
# translation regime take effect, and it must be the only thing there.
check "exactly one isb sits between msr and br" 1 \
	"$(printf '%s\n' "$SEQ" | awk -v s="$A_MSR" -v b="$A_BR" -F: '{a=$1+0; if (a>s && a<b && $0 ~ /isb/) c++} END{print c+0}')"

# Nothing that touches memory may sit between the msr and the branch other than
# the isb and the three register moves: a load or store there would fault.
check "no memory access between msr and br" 0 \
	"$(printf '%s\n' "$SEQ" | awk -v s="$A_MSR" -v b="$A_BR" -F: '{a=$1+0; if (a>s && a<b && $0 ~ /(ldr|str|ldp|stp)/) c++} END{print c+0}')"

# --- bit masks: the cleared bits must be exactly 0, 2 and 12 ---
# BIC (immediate) on a 64-bit register is rendered by the disassembler as
# `and x9, x9, #~imm`. The three masks are therefore:
#   bit 0  (M, MMU)            -> 0xFFFFFFFFFFFFFFFE
#   bit 2  (C, data cache)     -> 0xFFFFFFFFFFFFFFFB
#   bit 12 (I, instruction cache) -> 0xFFFFFFFFFFFFEFFF
# This is a STRICT check, not a report: clearing the wrong bit is the whole class
# of bug this test exists for, and an earlier version of this script only printed a
# note here -- which let an injected off-by-one (13 instead of 12) pass silently.
BITS=$(printf '%s\n' "$SEQ" | grep -oE '(and|bic)[[:space:]]+x9, x9, #0x[0-9a-f]+' \
	| grep -oE '0x[0-9a-f]+$' | sort -u | tr '\n' ' ')
echo "      masks: ${BITS:-none}"

for want in 0xfffffffffffffffe 0xfffffffffffffffb 0xffffffffffffefff; do
	check "clears the bit with mask $want" yes \
		"$(printf '%s' " $BITS " | grep -qi " $want " && echo yes || echo no)"
done

# And nothing else may be cleared: exactly three masks, no more.
check "exactly three bits are cleared" 3 \
	"$(printf '%s' "$BITS" | wc -w)"

# --- the three arguments must land in x0, x1, x2 ---
check "x0 is loaded from a parameter (dtb)" yes \
	"$(grep -qE 'mov[[:space:]]+x0, x[0-9]+' "$OUT/dis.txt" && echo yes || echo no)"
check "x1 is loaded (payload_pa)" yes \
	"$(grep -qE 'mov[[:space:]]+x1, x[0-9]+' "$OUT/dis.txt" && echo yes || echo no)"
check "x2 is loaded (payload_len)" yes \
	"$(grep -qE 'mov[[:space:]]+x2, x[0-9]+' "$OUT/dis.txt" && echo yes || echo no)"

echo
if [ $fail -eq 0 ]; then
	echo "EFI handoff trampoline OK"
	exit 0
fi
echo "EFI handoff trampoline FAILED ($fail)" >&2
exit 1
