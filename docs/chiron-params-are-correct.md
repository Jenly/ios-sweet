# chiron: the board parameters are correct. QEMU proves it.

Written 2026-10-21 after an ADT bisection that first went wrong and then, once the
measurement was fixed, gave a clear answer.

## The answer

```
qemu-sweet    undef=0  irq=1478  deepest PC 0xfffffe0007b3dd64
qemu-chiron   undef=0  irq=1490  deepest PC 0xfffffe0007b3dd64
```

Identical depth. Comparable interrupt counts. **The kernel runs on chiron's parameters.**

So the phone's failure is *not* a wrong device tree, a wrong DRAM size, a wrong GIC
address or a wrong timer INTID. All of those were fed to the kernel and it was fine.

## How the wrong conclusion was reached first

The first bisection compared `--device sweet` against `--device chiron` under QEMU and
found that chiron "died at `0xfffffe000748c000` with one Undefined Instruction" while
sweet "reached `0xfffffe0007a861c4`". That looked like a clean result and it was
worthless, for a reason visible in two numbers:

| ADT | size |
|---|---|
| `--qemu` | **7,180** bytes |
| `--device sweet` | **13,928** bytes |
| `--device chiron` | 13,936 bytes |

`--device` emits a **real hardware** device tree: the phone's actual GIC, UART, UFS. QEMU
implements none of those. Every `--device` ADT therefore dies at the same PC for the same
irrelevant reason. The tell that should have stopped me earlier: a variant I had built to
be *identical to a run that worked* also died. When one's control experiment fails, the
harness is wrong, not the hypothesis.

`--qemu` is the only flavour QEMU can run -- it swaps in virt's GIC at `0x08000000`, sets
CNTFRQ to 62.5 MHz, drops the UFS node and uses one generic CPU. It also *composes* with
`--device`, which is what makes the A/B above meaningful: same emulated hardware,
different board parameters.

## What is ruled out now, with evidence

| hypothesis | evidence |
|---|---|
| boot image format | header **v0** only; v1/v2 → `remote: 'BootImage is Incomplete'` |
| payload truncated | `avail == need` = `0xFE0F50` |
| segment copy failed | `bad=00000000`, every segment read back and compared |
| entry point wrong | `_start` = `b +0x1c64` over `LowExceptionVectorBase`'s nops |
| jump address wrong | `0x101490488` = `entry_va + (phys_base - virt_base)` |
| framebuffer address | five `0x9c000000` sites fixed by patch 0090 |
| DRAM size for this handset | chiron's DT: `0x100000000 + 0xbd8c0000`, same as the payload |
| **board parameters in the ADT** | **this document: kernel runs identically with them** |
| kernel image | runs to `0xfffffe0007b3dd64` processing ~1490 interrupts |

That is a lot of eliminated ground, and it was eliminated by measurement rather than
argument.

## What is left

The difference between QEMU and the phone is now the whole of the remaining search space,
and it is the part QEMU deliberately does not model:

- **UART.** `--qemu` drops the UFS node but the kernel still expects a console UART. The
  command line asks for `serial=3`, and chiron has no physical serial port. If the kernel
  blocks or faults in early console setup, that is exactly "silent, immediately".
- **UFS.** The root device (`rd=ufsroot0`) does not exist in QEMU in any usable form, so
  the kernel never tries to mount there. On the phone it does, and chiron's UFS host is
  driven by `sweet_ufs.c` -- written for SM7150 -- whose chiron addresses have never been
  exercised.
- **Real GIC behaviour.** QEMU's virt GIC is well-behaved. MSM8998's needs its
  redistributors found per-core and the port's `sweet_secondary_entry` shows it already
  knows that hazard exists on sweet.
- **Anything the SoC needs before `mrs`/`msr` of an implementation-defined register is
  legal.** QEMU is permissive about registers a real MSM8998 may trap.

## The next experiment on hardware

The kernel gets as far as it does in QEMU in a fraction of a second. On the phone the
panel never changes after the shim's `XNU jumping`. The most informative next step is to
make the shim prove the kernel executed *at all*, rather than continuing to guess:

- Set a breakpoint-shaped trap: have the shim write a known pattern to a scratch
  physical page, and have the kernel's `_start` path write a different pattern over it as
  its very first action (patch 0091). If the second pattern appears, the kernel ran its
  entry code and died later; if not, it never executed the first instruction.
- That is a single-word, unconditionally-visible experiment with no console needed --
  exactly what a board with no serial port requires.

Until that exists, every remaining hypothesis about *where* the kernel dies is a guess,
and this session has produced enough of those.
