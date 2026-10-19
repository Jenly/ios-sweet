# chiron bring-up: what is actually verified, and where it will break first

This document exists because a green CI run is easy to misread. Everything below
was written from the state of this repository, and every claim is tagged with how
it was established. If a claim has no tag, treat it as unverified.

**Nothing in this repository has ever executed on a Xiaomi Mi MIX 2.**

That single sentence is the most important thing in the file. Everything else is
about narrowing down *which* unknown will bite first.

## How to read the tags

| tag | meaning |
|---|---|
| **[compiled]** | a full macOS CI build produced a linkable kernel containing it |
| **[host]** | a host-side test asserts a property of it (and that test was shown able to fail) |
| **[read]** | a constant or sequence was transcribed from a cited line of the Linux kernel or device tree |
| **[shipped]** | it is in the built kernel, proven by symbol address |
| **[unverified]** | none of the above; it is a reasonable inference from the hardware documentation |

A **[host]** tag means the *arithmetic* is right. It does not mean the hardware
behaves as the arithmetic assumes.

## The kernel

Built and linked for real: 89 patches, `0001` through `0089`, applied with 0
failures to xnu-8796.141.3; CI run `39317064478` succeeded; the artifact is a
17,257,592-byte `Mach-O 64-bit arm64 executable, flags:<NOUNDEFS|PIE>` with
70,923 defined symbols and **zero undefined**. **[compiled]**

Everything below is **[shipped]** 鈥?the symbol is at an address in that binary:

| driver | symbol | what it does |
|---|---|---|
| `sweet_hwprobe.c` | `_sweet_hwprobe_run` | read-only hardware report |
| `sweet_spmi_v1.c` | `_sweet_spmi_v1_read` | PMIC arbiter v1 reads |
| `sweet_pm8998_reg.c` | `_sweet_pm8998_enable` | PM8998 LDO enable (gated) |
| `chiron_keys.c` | `_chiron_keys_init` | volume key via PM8998 GPIO6 |
| `msm8998_mdss.c` | `_sweet_mdss_blit` | MDP5 writeback blit |
| `sweet_restart.c` | `_sweet_restart_init` | PSHOLD reset hook |

---

## Ranked by how likely it is to be the first thing that breaks

### 1. The kernel never gets control at all **[unverified]**

The boot chain is ABL 鈫?`fastboot boot` 鈫?shim 鈫?XNU. For chiron, the piece that
does not exist yet is the UEFI wrapper: the shim is built as a boot.img payload,
not an EFI application. The `sweet-uefi` menu can chainload EFI images but the
shim is not one.

**Why this is first:** it is upstream of every other item. Nothing below can be
tested until the kernel starts executing.

**What to do:** build the shim as (or wrap it in) an EFI application, put it on
the ESP, and the menu picks it up with no changes. That is the single highest-value
next step.

### 2. `sweet_spmi_v1.c` 鈥?the arbiter may not be v1 at all **[read]**

Every address and encoding is traced: core `0x0800f000`, observer window
`0x9400000`, channel offset `0x800 + 0x80*channel`, APID map at `core + 0x800 +
4n`, command `opc<<27 | (sid&0xf)<<20 | addr<<4 | bc`. **[read]** from
`msm8998.dtsi:593` and `spmi-pmic-arb.c:890,1096,1171-1174`.

The driver reads the arbiter version register first and **refuses to run if it is
not below `0x20010000`** 鈥?so a wrong assumption produces a log line, not a hang.
**[host]** for the classification logic; the version check itself is **[unverified]**.

**Where it breaks:** if `ml_io_map` of the arbiter core succeeds but the block is
not clocked, the version read returns 0 and the driver refuses. That is the
designed behaviour but it means the PMIC path silently disappears.

### 3. PM8998 regulator writes 鈥?the only code that writes to the PMIC **[unverified]**

Everything about it is deliberately conservative: off unless `sweetpmic=1`,
set-only (never clears a bit), L6/L14 whitelisted, bus proven alive by reading the
revision pair before any write, read-back after. **[read]** for the register map
(`qcom_spmi-regulator.c:1446-1466`, `+0x40/0x41/0x45/0x46`, enable bit `0x80`).

**Where it breaks:** if the touch rails are already on (likely, since the
bootloader brought the panel up), this code does nothing at all and says so. The
risk is only in the `sweetpmic=1` path, which should not be used on a first boot.

### 4. `chiron_keys.c` 鈥?GPIO6 may not be enabled **[read]**

`0xC508`, STATUS1 at `+0x08`, bit 0 = input, bit 7 = enabled. **[read]** from
`msm-pm8998.dtsi:82-90` and `qpnp-pin.c:35-39`.

Read-only by design: it diagnoses, never reconfigures the pin. If bit 7 is clear
the driver logs once and produces no button 鈥?it does not pretend.

### 5. `msm8998_mdss.c` 鈥?the blit may simply not work **[unverified]**

The register sequence is traced (`mdp5.xml.h` bitfields, `mdp5_ctl.c:519-530`), and
the derived constants are checked against independent re-derivation. **[host]**
32 checks. But two hardware questions cannot be answered from source:

- whether the bootloader leaves the **MDSS SMMU** bypassed,
- whether `qcom,mdss-wfd-mode = "intf"` means WB must be fed from the interface
  rather than a layer mixer.

**Where it breaks:** the verification design assumes it *will* fail. With
`sweetmdss=2` the first frame is drawn by the CPU, one scanline is kept, the engine
is asked to reproduce it under 11 channel-selector candidates, and on total failure
the driver disables itself permanently and the CPU path continues. A wrong guess
costs a log line, not a black screen. There is also a **write-combine ordering
fix** in that path (DMB before the blit, DMB + 2 ms settle after) which is
**[unverified]** 鈥?it is correct in principle and untestable without hardware.

### 6. `sweet_restart.c` 鈥?PS_HOLD may not read as asserted **[unverified]**

`0x010AC000` **[read]** from `msm8998.dtsi:586-590`. The safety rule (bit 0 set,
not all-ones) is a pure function tested both directions over 1,000,000 values.
**[host]** 16 checks.

**Where it breaks:** if the preflight rejects, the hook is simply not installed and
`reboot(2)` keeps returning failure. That is the safe outcome. If it *accepts* on a
register that is not really PS_HOLD, the first reboot writes zero somewhere
unknown 鈥?which is why the preflight exists and why SM7150 is excluded rather than
guessed.

### 7. `sweet_usb.c` 鈥?per-SoC table, easy to have wrong **[read] + [host]**

Six facts, each traced. **[read]** The table is parsed out of the source and
compared against independently transcribed expectations. **[host]** 53 checks 鈥?and it caught two of my own errors before anything shipped: the GCC window
(`0xB0000`, not sweet's `0x90000`) and the GDSC offset (absolute `0x10F004` minus
the GCC base = `0xF004`, same as sweet).

**Where it breaks:** the probe reads GCC status before touching DWC3/QUSB2, so a
powered-down USB block is reported rather than hung on. **[unverified]** whether
that ordering is sufficient on real MSM8998.

### 8. UFS 鈥?the root filesystem **[compiled] only**

`sweet_ufs.c` trusts the bootloader's UFS state and finds `/sweet-ufs` in the ADT,
which `mkadt.py` emits correctly for chiron (`0x1da4000`, matching
`msm8998.dtsi:1796`). **No host test covers it** 鈥?it is IO-heavy and the only
meaningful test is on hardware.

**Where it breaks:** if the UEFI/handoff path does not leave the UFS host with its
doorbell enabled, root cannot be found and the boot stops before userland. The
hardware report (`sweethwprobe=1`) prints exactly that: `no doorbell -- the host is
not running, sweet_ufs.c will not find a root device`.

---

## What the first boot should pass

```
sweethwprobe=1          read-only report: MDSS / UFS / USB / TLMM / PMIC
```

and nothing else. Specifically **not** `sweetmdss=2`, **not** `sweetpmic=1`. Those
are step two, once the report says the blocks answered.

The report runs at the top of UFS init, before anything that can fail, and writes
nothing. Its whole purpose is that a boot which dies immediately still tells you
what the machine looked like.

## What is genuinely verified

| claim | evidence |
|---|---|
| the kernel compiles and links for SWEET | CI `39317064478`, `NOUNDEFS`, 0 undefined symbols |
| all 89 patches apply cleanly | local replay from a pristine tree, 0 failures |
| every header the patches add is exported | `tools/xnu-hdrcheck.py`, the check written after exactly that broke a build |
| MDP5 register constants match the Linux bitfields | 32 host checks, independent re-derivation |
| the USB per-SoC table matches the device trees | 53 host checks |
| the PSHOLD safety rule rejects implausible values | 16 host checks, two-way 1M sweep |
| the touch wire protocol packs STOP correctly | 24 host assertions + patch drift |
| sweet's display behaviour is unchanged | ADT regression: 1125x2436 at 22,18 |
| the repo matches what the patches produce | drift check across 12 files |

## What is not verified, at all

- Whether the kernel starts on chiron.
- Whether the UFS host is running when XNU takes over.
- Whether the MDSS SMMU is bypassed, or whether the writeback path is usable.
- Whether `sweet_spmi_v1`'s arbiter assumptions match the silicon.
- Whether the volume key GPIO is enabled by the bootloader.
- Whether PS_HOLD reads as asserted.
- Whether iOS userland gets as far as SpringBoard on this SoC 鈥?the display
  geometry is right, but the GPU work is bounded to a 2D blit (see
  `docs/gpu-research.md`) and there is no 3D acceleration.

## The honest summary

Seven drivers and two EFI-side projects were added, every one with at least a
compile check and most with a host test that was demonstrated to fail when the
code was broken. That is a real amount of verified work.

None of it has met the phone.

The remaining gap is not testing effort 鈥?it is **one specific missing piece: an
EFI wrapper for the shim**, without which nothing else can be tried. Everything
above should be read as "ready to be tried", not "known to work".
