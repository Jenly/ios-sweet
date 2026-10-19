# GPU acceleration on ios-sweet — feasibility study and groundwork

**Scope.** Can the MSM8998 (chiron / Mi MIX 2, Adreno 530) or the SM7150 (sweet,
Adreno 618) give iOS 16 real hardware acceleration, and what is the cheapest
real win? This document is a hardware-and-software trace, not a driver.

**Status honesty.** Nothing here was run on a phone, flashed, or compiled into
the kernel. No GPU driver was written. What *was* done: read the Qualcomm msm-4.4
driver that drives exactly this hardware on this exact SoC, extract the register
map for a 2D blit from it, read the XNU side to find where a blit would plug in,
and rank the possible goals by whether they are possible at all. Every register
number below is traced to a file and line that I read on the build server.
Things I could not verify are marked **unverified** or **TODO(unverified)**.

---

## 0. Verdict

**The GPU (3D) path is a dead end, and not for the reason people usually assume.**
It is not that writing an Adreno driver is too much work (it is a lot, but it is
done — freedreno exists). It is that the *client* is Apple's closed Metal/GLES
stack, whose shader compiler emits machine code for **Apple's own GPU ISA**, and
whose kernel ABI is Apple's AGX/IOGPU accelerator family. An Adreno 530 cannot
execute Apple GPU shaders. No amount of kernel work fixes that; you would have to
also replace the shader compiler inside the unmodified iOS userland, or write an
AGX→Adreno binary translator. Both are out of scope by orders of magnitude.

**What *is* real and worth doing:** the display controller's 2D engine (MDSS/MDP)
can do the copy that `SweetFramebuffer::present()` currently does with `memcpy`,
and it can scale while doing it. That is genuine hardware acceleration of the
present path — the copy whose cost forced the port to cap presenting at 10 Hz
(`SweetFramebuffer.cpp:293-298`). It is a bounded, testable project with a clear
failure mode (fall back to `memcpy`). It does **not** make the CPU rasteriser
faster: the UI would still be rasterised on the CPU, just presented by DMA.

**Ranking (details in §3).** Two orders are given because they differ, and
conflating them is how projects pick the wrong one:

*By pure feasibility:* (d) trivial > (b) possible with real risk > (c) possible
after (b) > (e) hard > (a) impossible.

*By what actually helps (recommended order of work):*

| rank | approach | feasible? | value |
|---|---|---|---|
| **1** | **(b) MDSS/MDP hardware blit for present** | **yes, with real risk** | **high — removes the CPU copy, unblocks 60 Hz present** |
| 2 | (c) MDP composition + scaling (superset of b) | yes, after (b) | medium-high — fixes chiron's cropped 276 rows |
| 3 | (d) CPU/NEON tuning of the present path | yes, trivial | very low — the copy is not where the CPU time goes; only useful as (b)'s fallback |
| 4 | (a) Adreno 5xx + Apple Metal | **no** | — (see §3(a): three independent fatal blockers) |
| — | (e) *not asked, but the ceiling*: take over scanout (MDP+DSI) so present is zero-copy | very hard | highest, and makes (b)/(c) optional |

(d) ranks first on feasibility and last on usefulness: it is the thing to do only
if (b) turns out to be blocked by the SMMU (§4.4).

---

## 1. What I could verify here, and what I could not

**Available on the build server** (`/root/src`, `/root/ios-sweet`):

* `/root/src/kernel_msm8998` — LineageOS `android_kernel_xiaomi_msm8998`
  (Qualcomm msm-4.4). This is the authoritative register source for MSM8998:
  `drivers/video/fbdev/msm/` (MDSS/MDP), `drivers/gpu/msm/` (kgsl/Adreno),
  `arch/arm64/boot/dts/qcom/msm8998*.dtsi` (addresses, clocks, IOMMU),
  `include/dt-bindings/clock/msm-clocks-hwio-8998.h` (clock register offsets).
* `/root/src/xnufinal` — the patched XNU tree (patches 0001-0083).
* `/root/ios-sweet` — project repo, `NOTES.md` (511 lines) with the prior
  reverse-engineering log, and `tools/devices.py`.

**NOT available: the dyld shared cache.** `/root/ios-sweet/research/` contains
only `ipsw-x/` (the iPhone10,3 `DeviceTree.d22ap` and its JSON), i.e.
`research/ipsw-x/d22ap.adt` and `research/ipsw-x/dt/DeviceTree.d22ap.im4p`.
There is **no** `dyld_shared_cache_arm64` anywhere on this machine
(`find / -iname '*dyld_shared_cache*'` → nothing) and the `ipsw` tool is not
installed. So **I could not disassemble QuartzCore, IOMobileFramebuffer or
Metal for this report.**

Consequence: everything in §2 about *how* CoreAnimation picks a renderer is
either (i) quoted from the project's own previously-recorded RE results in
`NOTES.md` (done on the user's workstation, where the cache lives at
`research/dsc/dyld_shared_cache_arm64`), or (ii) observed behaviour in the
port's boot logs, also recorded in `NOTES.md`. Nothing in §2 is my own fresh
disassembly, and I have flagged which is which.

Also not available: a macOS/XNU build host. `NOTES.md:62` records that kernel
compilation happens on a macOS CI runner, not on this Linux server, so the
skeleton in §6 has **never been compiled**.

---

## 2. How iOS 16 gets pixels, and what would make it pick a GPU

### 2.1 The chain that exists today

Established by the port's own logs (`NOTES.md`):

1. `backboardd` starts the window server: `MTLSetShaderCachePath`,
   `[CAWindowServer serverWithOptions:...]`, `setRendererFlags:`, then asks for
   `mainDisplay` (`NOTES.md:198`, from the crash backtrace of an early boot).
2. Displays come from **IOMobileFramebuffer kernel services**: `CA::WindowServer`
   `_detectDisplays` iterates static tables of `AppleCLCDDisplay`,
   `AppleRGBOutDisplay`, `AppleWirelessDisplay`, each `count()` =
   `display_count_of_type()` over `IOMobileFramebufferCreateDisplayList()`
   (`NOTES.md:196`). The port satisfies this with `AppleCLCD :
   IOMobileFramebuffer` on `/arm-io/disp0` (`SweetFramebuffer.cpp:85-93`,
   `:199-202`).
3. Surfaces are IOSurface kernel objects: `IOCoreSurfaceRoot` +
   `IOSurfaceRootUserClient` (`SweetSurface.cpp`), selectors and traps
   documented in `NOTES.md:180-186`.
4. Frames arrive as `SwapEnd` records (0x46c bytes) naming up to 4 IOSurfaces
   with source/destination rects (`SweetFramebuffer.cpp:14-18`, `:327-345`).
5. `present()` copies those surfaces into the panel memory with `memcpy`
   (`SweetFramebuffer.cpp:393-399`), at most every `fFlushTicks` vsyncs, default
   10 Hz (`:194-197`). The comment at `:293-298` says why: copying 10 MB on
   every swap at 60 Hz "took the whole CPU".

### 2.2 What a GPU renderer would require — the evidence

The decisive observation is in the port's own IOKit trace: with no GPU driver,
`backboardd` *polls* `IOProviderClass IOAcceleratorES`, and also looks for
`AGXAccelerator` and `IOAcceleratorESLegacy`, repeatedly, and finds nothing
(`NOTES.md:162`, `NOTES.md:164`). CA then renders with the CPU rasteriser
`CA::OGL::SW` — the "Hello" screen and the migration progress bar were drawn this
way (`NOTES.md:144`, `NOTES.md:161`).

From the cache RE already on record (`NOTES.md:194-197`, `NOTES.md:234`):
QuartzCore contains four renderer constructors — `metal_new_context`,
`gles_new_context`, `sw_new_context` (`CA::OGL::SWContext`), and
`new_null_context` — plus env knobs `CA_ENABLE_OGL`, `CA_DISPLAY_TYPE`,
`CA_FORCE_LOCAL_SERVER`, `CA_DISABLE_RENDER`, `CA_ALWAYS_RENDER`. **The exact
selection logic has never been traced** (`NOTES.md:174` calls it an open
question; `NOTES.md:195` says "selection logic not traced yet").

So the honest statement of the decision point is:

> CoreAnimation selects a GPU renderer by finding and opening an IOService of
> Apple's accelerator families (`AGXAccelerator` for Metal,
> `IOAcceleratorES`/`IOAcceleratorESLegacy` for GLES — the class names observed
> in the port's trace). With no such service it falls back to the CPU
> rasteriser. The *only* thing that has been proven is the fallback direction:
> no service → `CA::OGL::SW`. The exact match list, the properties read, and the
> user-client ABI behind those families are **unverified** here (no cache on
> this server).

### 2.3 What a kernel-side stub would have to expose

Minimum to make CA *believe* a GPU exists, in increasing order of difficulty:

1. **A matching IOService.** `IOServiceMatching("IOAcceleratorES")` (or
   `AGXAccelerator`) must find a registered service, attached where the
   framework expects it (the accelerator services in iOS live under the
   platform/`arm-io` subtree; the port already creates `/arm-io` child nubs in
   `SweetPlatformExpert.cpp`).
2. **A user client** implementing that family's selector ABI: device
   information, memory allocation, command-queue submission, completion
   notification. Undocumented; would have to be re-derived from
   `IOAcceleratorFamily2`/`IOGPU` binaries in the cache — which are not on this
   server.
3. **A GPU that executes Apple's command stream and Apple's shader ISA.**
   This is the wall. Metal's shaders are compiled *on the device* by Apple's
   runtime compiler into AGX machine code; the driver's job is to feed that to
   Apple's GPU. Adreno 5xx/6xx execute a completely different ISA (documented
   only by reverse engineering, e.g. freedreno). A stub that satisfies (1) and
   (2) but not (3) produces a device that CA will select and then crash on.

### 2.4 The cheap experiment that would replace guesswork with data

Publish a **decoy** service matching `IOAcceleratorES` (and optionally
`AGXAccelerator`) with a user client that returns `kIOReturnUnsupported` for
every selector, behind a boot-arg (so it can be turned off if CA crash-loops),
with the IOKit trace patch (0049) enabled. One boot then yields: the exact match
strings, the properties read before `IOServiceOpen`, the open type, and the first
selectors attempted. That is ~30 lines of code and one boot, and it is the only
way to learn the ABI without the cache. **Risk:** CA may select GLES and crash;
gate it and be ready to revert.

Also worth knowing: `IOSurface` has selector `0xc BindAccel` and selectors
`0x24-0x26`/`0x2d` for `IOSurfaceAccelerator` (`NOTES.md:185`), which the port
currently accepts and drops. On real hardware that is the *2D* accelerator hook
(Apple's scaler/format-conversion block). Nothing in the current logs shows CA
using it, so treat it as a lead, not a plan.

---

## 3. Candidate approaches, ranked

Common ground for (b) and (c): the MSM8998 display subsystem is **MDSS** with an
**MDP** (the "mdp5"-generation block; Qualcomm's fbdev driver, not DRM) at
`0x0C900000`. Registers in §5. The DSI panel is already being scanned out
continuously by the boot chain from a fixed physical framebuffer, so the port
does not need to touch DSI at all for (b)/(c) — it only needs to write pixels
into that framebuffer with DMA instead of the CPU.

### (d) Multi-threaded NEON / SIMD CPU rasteriser optimisation — *feasible, nearly worthless*

* **What it would touch:** `SweetFramebuffer.cpp:393-399` (the copy) and the
  fade-out loop at `:235-241`.
* **Reality check.** The surface is 10,368,000 bytes (1080x2400x4 = 9.89 MiB);
  copying it at 60 Hz is 622 MB/s of read traffic plus 622 MB/s of write traffic,
  against ~29.8 GB/s of LPDDR4X theoretical bandwidth on this SoC class. So the
  copy is *not* bandwidth-bound — it is a single-threaded, cache-hostile 633-page
  walk, and my per-frame estimate of 2-5 ms is **inferred, not measured**. What
  *is* measured (by the port, not by me) is that it was expensive enough to
  force the present rate down to 10 Hz: "copying 10 MB into the panel on every
  swap (backboardd presents at 60 Hz) took the whole CPU"
  (`SweetFramebuffer.cpp:293-298`). Note also that a NEON copy cannot be the
  real fix, because the CPU time in this system is dominated by **QuartzCore's
  closed rasteriser**, and we cannot optimise `CA::OGL::SW`'s inner loops at
  all: it is closed-source ARM64 in the shared cache, and the port explicitly
  runs the unmodified userland.
* **The one real lever:** the present rate. `fFlushTicks` was lowered to 10 Hz
  *because* of the copy cost (`:293-298`), so the UI is capped at 10 fps for a
  reason that hardware DMA would remove entirely. That is an argument for (b),
  not for (d).
* **Effort:** 2-5 days for a NEON copy + row-parallel threads. **Verdict:** only
  worth it as a fallback path, or as a stopgap if (b) fails.

### (b) Accelerate the copy/present path with the MDP — *the real win, medium risk*

* **Hardware blocks:** MDP (SSPP/DMA pipe → layer mixer → writeback), i.e.
  DMA1 @ `0x0C928000`, LM1 @ `0x0C947000`, WB @ `0x0C967000`, CTL1 @
  `0x0C903200`, all inside the MDSS window at `0x0C900000`
  (derivations in §5). The display keeps scanning out the same physical
  framebuffer, so nothing about the panel changes.
* **Where the registers are documented:** Qualcomm's GPL driver —
  `drivers/video/fbdev/msm/mdss_mdp_hwio.h` (block-internal offsets),
  `mdss_mdp_intf_writeback.c` (WB programming), `mdss_mdp_pipe.c` (source pipe
  programming), `mdss_mdp_ctl.c` (CTL/LAYER/FLUSH/START and the pipe→stage
  encoding table), `mdss_mdp_formats.h` (pixel-format encoding),
  `msm8998-mdss.dtsi` (block bases, IRQ, clocks). Full table in §5.
* **What it buys:** the 10.4 MB/frame CPU copy disappears; present can go back
  to 60 Hz for ~0 CPU; the vsync workloop stops being a bottleneck; the fade-out
  can become a hardware constant-alpha blend (or just keep the CPU loop).
* **Minimum viable implementation:** the skeleton in §6 — one kickoff,
  poll-for-completion, `memcpy` fallback on timeout.
* **The real risk, stated plainly:** the MDP is a **bus master behind the MMSS
  SMMU** (`mmss_smmu: arm,smmu-mmss@cd00000`, `msm-arm-smmu-8998.dtsi:98-101`;
  MDSS's context-bank children at `msm8998-mdss.dtsi:233-259`). XNU has **no
  IOMMU support at all**: `osfmk/arm/pmap/pmap_iommu.c` is a 33-line
  "Placeholder for pmap IOMMU functionality", and there is no SMMU driver in
  `iokit/`. Therefore the MDP can only be pointed at **physical** addresses, and
  only if the SMMU is left unprogrammed (bypass). Evidence that this is the
  boot state: the Linux driver deliberately does *not* attach the SMMU during
  the continuous-splash handoff because it "may still be working with phys
  addr" (`mdss_mdp.c:1521-1526`) — i.e. the bootloader's splash runs with
  physical addresses. **unverified on our hardware**: whether the UEFI chain
  leaves the SMMU in that state on chiron. `edk2-msm8998` contains no SMMU code
  (`grep -ril smmu` over the tree → nothing), which is consistent with "never
  programmed", but absence of code is not proof of bypass.
* **Second real risk:** the source surfaces are *not* physically contiguous.
  `SweetSurface` allocates pixels with
  `IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task, kIODirectionInOut |
  kIOMemoryKernelUserShared, allocSize, page_size)` (`SweetSurface.cpp:691-692`)
  — page-based, no contiguity guarantee. Fix is in our own code (we own the
  allocator): allocate with `kIOMemoryPhysicallyContiguous | kIOMemoryMapperNone`
  and fall back to the CPU copy if the ~10.4 MB allocation fails. Note the
  subtlety in `IOBufferMemoryDescriptor.cpp:302-326`: a contiguous kheap
  allocation (`IOKernelAllocateWithPhysicalRestrict`, `:326`) is only taken when
  `contig` is set, and `kIOMemoryPhysicallyContiguous` sets it only when the
  descriptor is *not* using the default mapper (`:304-306`:
  `contig |= (!mapped); contig |= (kIOMemoryMapperNone & options);`) — hence the
  explicit `kIOMemoryMapperNone`.
* **Effort:** 2-4 weeks to a *proven* blit (power/clock sequencing, timeouts,
  fallback), 6-8 weeks to something you would leave enabled (tearing, vsync
  alignment, error recovery).

### (c) MDP 2D composition + scaling — *the superset, do it after (b)*

* Same block as (b), plus the **VIG** pipes (`0x5000/0x7000/0x9000/0xB000`,
  `msm8998-mdss.dtsi:67`) which carry the QSEED scaler
  (`MDSS_MDP_REG_VIG_QSEED2_*` / `MDSS_MDP_REG_SCALER_*`,
  `mdss_mdp_hwio.h:306-344`; phase-step registers at `0x210/0x214` and init
  phase at `0x220/0x224`), and the swap record already carries up to 4 layers
  with independent `a`/`b` rects (`SweetFramebuffer.cpp:327-345`), so the ABI to
  composite several layers already exists.
* **Concrete user-visible win on chiron:** the port presents a 1125x2436
  logical screen into a 1080x2160 panel by *cropping* — `test-board-header`
  shows "panel shows y 0..2159 (top-aligned, 276 rows off-panel)"
  (`notes-handoff.md:47-52`). With the MDP scaler the same content can be
  scaled to fit instead of losing 276 rows.
* **Effort:** +2-4 weeks over (b). Scaling brings its own register surface
  (phase steps, coefficient RAM, decimation limits) and the VIG pipe's QSEED
  version for this MDP revision is **unverified** (`mdss_mdp_hwio.h` carries both
  QSEED2 and QSEED3 register sets; which one this MDP uses depends on
  `mdata->mdp_rev`, read from `MDSS_REG_HW_VERSION` at runtime —
  `mdss_mdp.c:2249-2255`).

### (a) Adreno 5xx driver in XNU + Apple's Metal — **dead end**

Three independent blockers, any one of which is fatal:

1. **ISA mismatch (fatal, unfixable in the kernel).** iOS Metal shaders are
   compiled on-device by Apple's closed compiler to Apple-GPU (AGX) machine
   code. Adreno 5xx has a different ISA. A kernel driver cannot change what the
   userland compiler emits. Fixing this means replacing Metal's compiler inside
   the unmodified userland, or writing an AGX→Adreno translator.
2. **Closed kernel ABI.** The accelerator user-client ABI (the family CA and
   Metal open) is undocumented and not on this machine to reverse. Even a
   perfect Adreno driver would be invisible to Apple's stack.
3. **Signed firmware / TrustZone.** The Adreno 530 will not start without
   Qualcomm's signed microcode: `a530_pm4.fw`, `a530_pfp.fw`,
   `a530_zap`, `a530_gpmu.fw2`/`a530v3_gpmu.fw2`, `a530v2_seq.fw2`
   (`adreno-gpulist.h:158-172` for patchid 1, `:183-189` for the ANY_ID entry,
   `:142-143` for the v1 entry). The zap shader is loaded through
   PIL and the driver *aborts* if it fails: `zap_handle_ptr =
   subsystem_get(adreno_dev->gpucore->zap_name); ... if (IS_ERR_OR_NULL(...))
   return -ENODEV` (`adreno_a5xx.c:2278-2286`). That path authenticates through
   the secure world (`scm_call2*`, e.g. `adreno.c:2741-2748`). XNU in this port
   has no TrustZone/SCM client, and the images are signed for Qualcomm's keys —
   you cannot substitute your own.
   GPU base/IRQ for reference: `qcom,kgsl-3d0@5000000`, 0x40000, SPI 300
   (`msm8998-gpu.dtsi:52-59`), chipid `0x05040000` (`:62`), GPU MMU at
   `0x05040000`+0x10000 (`:248-252`), GPUCC at `0x5065000` (`msm8998.dtsi:973`).
* **If someone insisted anyway:** a freedreno-class Adreno bring-up under XNU
  (power/clock/CP/ringbuffer/IOMMU, no Apple userland support) is a
  6-12 person-month project with zero effect on the iOS UI, because of (1) and
  (2). **Do not spend time here.**

### (e) The actual ceiling (not asked, but it is where (b)/(c) lead)

Take over **scanout** — program VIG0→LM0→INTF0→DSI0 yourself and point it at the
compositor's surface — and present becomes zero-copy and tear-free. That needs
the DSI PHY/PLL and the panel's init sequence from the device tree, i.e. weeks to
months, and a panel-timing mistake bricks the display until reflash. (b) and (c)
are the sane steps that make (e) optional rather than necessary.

---

## 4. MSM8998 display/GPU hardware facts (all traced)

### 4.1 Addresses

| block | address | length | source |
|---|---|---|---|
| MDSS top + MDP | `0x0C900000` | `0x90000` | `msm8998-mdss.dtsi:17` |
| MDP register window | `0x0C900000 + 0x1000` | — | `msm8998-mdss.dtsi:58` (`qcom,mdss-mdp-reg-offset`) |
| VBIF | `0x0C9B0000` | `0x1040` | `msm8998-mdss.dtsi:17-19` |
| MDP interrupt | GIC SPI 83 | — | `msm8998-mdss.dtsi:20` |
| MMSS clock controller | `0x0C8C0000` | `0x40000` | `msm8998.dtsi:948-950` |
| MDSS GDSC (power) | `0x0C8C2304` | 4 | `msm-gdsc-8998.dtsi:116` |
| MMSS SMMU | `0x0CD00000` | `0x40000` | `msm-arm-smmu-8998.dtsi:98-101` |
| Adreno 530 (kgsl-3d0) | `0x05000000` | `0x40000` | `msm8998-gpu.dtsi:52-57` |
| Adreno GPU MMU | `0x05040000` | `0x10000` | `msm8998-gpu.dtsi:248-252` |
| GPU clock controller | `0x05065000` | — | `msm8998.dtsi:973-974` |

### 4.2 Block offsets inside the MDP window (`msm8998-mdss.dtsi`)

| block | offsets | line |
|---|---|---|
| CTL0..CTL4 | `0x2000 0x2200 0x2400 0x2600 0x2800` | `:91` |
| VIG0..VIG3 (scaling pipes) | `0x5000 0x7000 0x9000 0xB000` | `:67` |
| DMA0..DMA3 (no scaler) | `0x25000 0x27000 0x29000 0x2B000` | `:69` |
| CURSOR0/1 | `0x35000 0x37000` | `:71` |
| LM0..LM3 (layer mixers) | `0x45000 0x46000 0x47000 0x4A000` | `:93` |
| DSPP0/1 | `0x55000 0x57000` | `:95` |
| WB (named "WB_2") | `0x66000` | `:96`, name at `:200` |
| INTF0..INTF3 | `0x6B000 0x6B800 0x6C000 0x6C800` | `:97` |
| PingPong0..3 | `0x71000 0x71800 0x72000 0x72800` | `:99` |
| Dest scaler | `0x61000` (block), scalers `+0x800`, `+0x1000` | `:227-229` |

XIN ids (VBIF clients): VIG `0 4 8 12`, DMA `1 5 9 13`, CURSOR `2 10`
(`msm8998-mdss.dtsi:73-75`).

### 4.3 Clocks and power (what must be on before *any* register works)

* Clocks: `mnoc_clk, iface_clk, bus_clk, core_clk_src, core_clk, vsync_clk,
  lut_clk` (`msm8998-mdss.dtsi:116-125`).
* Bounds: `qcom,max-clk-rate = <412500000>` (`msm8998-mdss.dtsi:62`),
  `qcom,max-mixer-width = <2560>` / `max-pipe-width` (`:40-41`).
* Their MMSS-CC branch registers (`include/dt-bindings/clock/msm-clocks-hwio-8998.h`):
  `MMSS_MDSS_AHB_CBCR 0x2308` (`:354`), `MMSS_MDSS_AXI_CBCR 0x2310` (`:355`),
  `MMSS_MDSS_MDP_CBCR 0x231C` (`:373`), `MMSS_MDSS_MDP_LUT_CBCR 0x2320`
  (`:374`), `MMSS_MDSS_VSYNC_CBCR 0x2328` (`:378`), `MMSS_MDSS_ROT_CBCR 0x2350`
  (`:377`), source RCG `MMSS_MDP_CMD_RCGR 0x2040` (`:259`).
  Branch enable = **bit 0** of the CBCR (`clock-local2.c:68`
  `#define CBCR_BRANCH_ENABLE_BIT BIT(0)`). The halt/ack bit is **unverified**.
* GDSC: power-on is **bit 31** (`gdsc.c:29 #define PWR_ON_MASK BIT(31)`), status
  is polled with `udelay(1)` (`gdsc.c:87-118`); the status bit position is
  **unverified** (do not guess it — read `gdsc.c` or dump the register on a
  working boot).
* `qcom,mdp-settings` (a table the Linux driver writes at init,
  `msm8998-mdss.dtsi:127-145`) includes `0x01190 <- 0`, `0x012ac <- 0xc0000ccc`,
  `0x012b4 <- 0xc0000ccc`, `0x012bc <- 0x00cccccc` (`:130`),
  `0x012c4 <- 0x0000cccc`, `0x013a8 <- 0x0cccc0c0`, `0x013b0 <- 0xccccc0c0`,
  `0x013b8 <- 0xcccc0000`, `0x013d0 <- 0x00cc0000`, and `0x?06c <- 0` for each
  pipe.
  **These are not optional**: they ungated the per-pipe AXI clock fields. XNU
  must replay them. (Offsets in this table are relative to the MDSS base, i.e.
  `0x1000` higher than the same register's offset inside the MDP window —
  compare `0x012bc` here with `ctx->clk_ctrl.reg_off = 0x2BC`,
  `mdss_mdp_intf_writeback.c:101`.)
* Panel/DSI are **not** needed for (b)/(c): the boot chain leaves DSI0 scanning
  out the splash framebuffer, and the port writes into that same memory today
  (`SweetFramebuffer.cpp:161-165`, `fPhys` from `PE_current_console`).

### 4.4 The SMMU question (the one that decides feasibility)

* MDSS's streaming master goes through `mmss_smmu`; the MDP has *no* `iommus`
  property of its own — instead two context-bank child nodes
  `qcom,smmu_mdp_unsec_cb` (stream 0) and `qcom,smmu_mdp_sec_cb` (stream 1)
  (`msm8998-mdss.dtsi:233-259`), and the rotator has its own
  (`:610-637`, streams `0xe00`/`0xe01`).
* XNU has no IOMMU driver: `osfmk/arm/pmap/pmap_iommu.c` is a placeholder
  ("Eventually all code relating to the IOMMU extensions will be placed into
  this file", 33 lines total). `iokit/` has no SMMU/DART driver.
* The boot state is bypass/no-translation: the Linux driver defers its SMMU
  attach until the continuous splash handoff completes, because the splash "may
  still be working with phys addr" (`mdss_mdp.c:1521-1526`).
* ⇒ **A blit is only possible with physical addresses, and only if the SMMU
  stays unprogrammed.** This must be *measured* on chiron before writing any
  driver code (§8).

---

## 5. MDSS/MDP 2D blit register map

This is the concrete table a kernel driver would use. Path:
**DMA1 (source, linear BGRA8888) → LM1 (stage 0, opaque) → WB (linear
BGRA8888)**, kicked by **CTL1** in writeback mode. All offsets inside a block are
from `mdss_mdp_hwio.h`; the block bases are from `msm8998-mdss.dtsi`.

### 5.1 Absolute addresses for this path

| register block | absolute base | derivation |
|---|---|---|
| MDP window | `0x0C901000` | `0x0C900000 + 0x1000` (`msm8998-mdss.dtsi:17,58`) |
| CTL1 | `0x0C903200` | `+0x2200` (`:91`) |
| DMA1 | `0x0C928000` | `+0x27000` (`:69`) |
| LM1 | `0x0C947000` | `+0x46000` (`:93`) |
| WB | `0x0C967000` | `+0x66000` (`:96`) |
| MDP INTR_STATUS | `0x0C901014` | `+0x14` (`hwio.h:67`) |
| MDP INTR_CLEAR | `0x0C901018` | `+0x18` (`hwio.h:68`) |
| MDP HW_VERSION | `0x0C900000` | `hwio.h:61` (MDSS-relative) |

### 5.2 Source pipe (DMA1) — offsets in the DMA1 block

| off | register | value for a BGRA8888 copy | source |
|---|---|---|---|
| `0x000` | `SSPP_SRC_SIZE` | `(h<<16)\|w` | `hwio.h:241` |
| `0x008` | `SSPP_SRC_XY` | `(src_y<<16)\|src_x` | `hwio.h:243` |
| `0x00C` | `SSPP_OUT_SIZE` | `(h<<16)\|w` | `hwio.h:244` |
| `0x010` | `SSPP_OUT_XY` | `0` | `hwio.h:245` |
| `0x014` | `SSPP_SRC0_ADDR` | `src_phys + y*stride + x*4` | `hwio.h:246`; write site `mdss_mdp_pipe.c:2283` |
| `0x024` | `SSPP_SRC_YSTRIDE0` | `src_stride` in `[15:0]` | `hwio.h:250`; `mdss_mdp_pipe.c:1914` |
| `0x028` | `SSPP_SRC_YSTRIDE1` | `0` | `hwio.h:251`; `mdss_mdp_pipe.c:1915` |
| `0x030` | `SSPP_SRC_FORMAT` | **`0x000236FF`** (derived) | `hwio.h:253`; encoding `mdss_mdp_pipe.c:2123-2155` |
| `0x034` | `SSPP_SRC_UNPACK_PATTERN` | **`0x03020001`** | `hwio.h:254`; `mdss_mdp_pipe.c:2140-2141` |
| `0x038` | `SSPP_SRC_OP_MODE` | `0` (no flip/scale) | `hwio.h:255`; `mdss_mdp_pipe.c:2178` |
| `0x048` | `SSPP_FETCH_CONFIG` | **do not write** for linear formats | `mdss_mdp_pipe.c:2158-2168` |

**How `SRC_FORMAT = 0x236FF` is derived** (this is arithmetic on the driver's own
code + its own format table, not a value read off hardware):

* `MDP_BGRA_8888` → `FMT_RGB_8888(..., alpha_en=1, e0=C1_B_Cb, e1=C0_G_Y,
  e2=C2_R_Cr, e3=C3_ALPHA)` (`mdss_mdp_formats.h:320-322`).
* `FMT_RGB_8888` gives `fetch_planes=INTERLEAVED(0)`, `unpack_tight=1`,
  `unpack_align_msb=0`, `unpack_count=4`, `bpp=4`, all four channels
  `COLOR_8BIT=3` (`mdss_mdp_formats.h:75-94`; enum at `:25-32`).
* Encoding (`mdss_mdp_pipe.c:2123-2155`):
  `(chroma<<23)|(planes<<19)|(b3<<6)|(b2<<4)|(b1<<2)|(b0<<0) | ((count-1)<<12)
  | (tight<<17) | (align<<18) | ((bpp-1)<<9)`
  = `0xFF | 0x3000 | 0x20000 | 0x600` = **`0x000236FF`**
  (channel bits `0xC0|0x30|0xC|0x3 = 0xFF`; `count-1` → `0x3000`;
  `tight` → `0x20000`; `bpp-1` → `0x600`).

These constants are **machine-checked**, not hand-arithmetic: a standalone C
program re-implements the two encoding functions from `mdss_mdp_pipe.c:2123-2155`
and `mdss_mdp_intf_writeback.c:444-479`, applies them to the `MDP_BGRA_8888`
entry, and asserts the results (`SRC 0x236FF`, `DST 0x237FF`,
`PATTERN 0x03020001`, `CTL_FLUSH 0x11200`). My first hand-computed values were
wrong in four places; the assertions caught them. Anyone re-deriving these should
run the same check rather than trust the table.

### 5.3 Destination — WB block

| off | register | value | source |
|---|---|---|---|
| `0x000` | `WB_DST_FORMAT` | **`0x000237FF`** (= src format + `BIT(8)` DSTC3_EN) | `hwio.h:520`; encoding `mdss_mdp_intf_writeback.c:444-479` |
| `0x004` | `WB_DST_OP_MODE` | **`0`** for a plain line-mode blit | `hwio.h:521`; set from `ctx->opmode`, which `mdss_mdp_writeback_prepare_wfd()` zeroes (`intf_writeback.c:636`) |
| `0x008` | `WB_DST_PACK_PATTERN` | `0x03020001` | `hwio.h:522`; `intf_writeback.c:468-471` |
| `0x00C` | `WB_DST0_ADDR` | `dst_phys + y*stride + x*4` | `hwio.h:523`; `intf_writeback.c:242` |
| `0x01C` | `WB_DST_YSTRIDE0` | `dst_stride` | `hwio.h:527`; `intf_writeback.c:506` |
| `0x074` | `WB_OUT_SIZE` | `(h<<16)\|w` | `hwio.h:542`; `intf_writeback.c:485,508` |
| `0x078` | `WB_ALPHA_X_VALUE` | `0xFF` | `hwio.h:543`; `intf_writeback.c:502` |
| `0x2B0` | `WB_DST_ADDR_SW_STATUS` | `0` for non-secure | `hwio.h:549`; `intf_writeback.c:1054` |

**Trap worth stating explicitly — there are two different "opmode" registers and
they do not hold the same value.** `mdata`-level `CTL_TOP` receives
`ctl->opmode` (`mdss_mdp_ctl.c:5887`), which for a writeback session is
`MDSS_MDP_CTL_OP_WB0_MODE` = `0x3` (`hwio.h:200`), chosen by
`mdss_mdp_wfd_setup()` from the *mixer* number (`mdss_mdp_wfd.c:259-266`). The
writeback block's own `WB_DST_OP_MODE` receives `ctx->opmode`, which for the
line/wfd path is explicitly **0** (`intf_writeback.c:636`) — the CSC/BWC/rotation
bits are only OR-ed in for YUV or UBWC or the rotator
(`intf_writeback.c:423-442`, `:487-494`, `:690-712`). Writing `0x3` into
`WB_DST_OP_MODE` (as my first draft of the skeleton did) would be wrong.

Write order used by the driver: alpha, format, opmode, pattern, strides,
out_size, write_config (`intf_writeback.c:502-509`).

### 5.4 Layer mixer (LM1)

| off | register | value | source |
|---|---|---|---|
| `0x000` | `LM_OP_MODE` | `0` (+ per-stage "keep fg alpha" bits in normal use) | `hwio.h:457`; `mdss_mdp_ctl.c:5198` |
| `0x004` | `LM_OUT_SIZE` | `(h<<16)\|w` | `hwio.h:458`; `mdss_mdp_ctl.c:5055` |
| `0x008` | `LM_BORDER_COLOR_0` | `0` | `hwio.h:459`; `mdss_mdp_ctl.c:5200` |
| `0x010` | `LM_BORDER_COLOR_1` | `0` | `hwio.h:460`; `mdss_mdp_ctl.c:5202` |
| `0x20 + 0x30*stage + 0x00` | `LM_BLEND_OP(stage)` | `FG_ALPHA_FG_CONST \| BG_ALPHA_BG_CONST` = `0x00000100` (opaque) | `hwio.h:462-463`; `mdss_mdp_ctl.c:5115-5116`; bits `hwio.h:494,503` |
| `0x20 + 0x30*stage + 0x04` | `LM_BLEND_FG_ALPHA` | `0xFF` | `hwio.h:464`; `ctrl.c:5179` |
| `0x20 + 0x30*stage + 0x08` | `LM_BLEND_BG_ALPHA` | `0x00` | `hwio.h:465`; `mdss_mdp_ctl.c:5181` |
| `0x150` | `LM_BLEND_STAGE4` (extra stages) | — | `hwio.h:474`; `mdss_mdp_ctl.c:5103-5104` |

`stage` is the blend-stage ordinal; the driver shifts later stages by
`LM_BLEND_STAGE4` for `stage > 3` (`mdss_mdp_ctl.c:5103-5104`). **Which ordinal
the first pipe lands on is unverified here** — see §5.6.

### 5.5 Control block (CTL1): binding, flush, kickoff

| off | register | value | source |
|---|---|---|---|
| `0x00C` | `CTL_LAYER(3)` | `2 << 21` (DMA1 at stage 0) **unverified variant**, see §5.6 | `hwio.h:180-181`; encoding `mdss_mdp_ctl.c:46-61` + `:4823-4847` |
| `0x014` | `CTL_TOP` | `0x3` (writeback mode) | `hwio.h:189`, `hwio.h:200`; write `mdss_mdp_ctl.c:5887`; value chosen at `mdss_mdp_wfd.c:259-266` |
| `0x018` | `CTL_FLUSH` | `BIT(12) \| BIT(6+3) \| BIT(16)` = `0x00011200` | `hwio.h:190`; pipe bit `mdss_mdp_ctl.c:5446`, LM bit `:5189`, WB bit `intf_writeback.c:1053` |
| `0x01C` | `CTL_START` | `1` | `hwio.h:191`; `intf_writeback.c:1069` |

Completion: `MDP_INTR_STATUS` bit 4 = `MDSS_MDP_INTR_WB_2_DONE`
(`mdss_mdp_hwio.h:95`), cleared via `MDP_INTR_CLEAR` (`hwio.h:68`). The Linux
driver instead enables the WB interrupt (`intf_writeback.c:1049-1059`) and
sleeps on a completion (`:922-999`); XNU has no MDSS interrupt handler yet, so
polling is the minimum viable approach.

### 5.6 What is still unverified in this table

Stated explicitly so nobody mistakes the table for a working driver:

1. **Which `CTL_LAYER<n>` register the writeback mixer uses.**
   `mdss_mdp_ctl.c:4863-4870` documents "INTF: 0,1,2,5 / WB: 3,4", and
   `mdss_mdp_ctl.c:4875-4884` picks hw mixer 1 or 3 depending on
   `MDSS_CAPS_MIXER_1_FOR_WB`. Which applies to this MDP revision is not
   determined here. Note the coupling: `CTL_TOP = 0x3` (WB0 mode) is only
   correct if the assigned mixer reported `MDSS_MDP_WB_LAYERMIXER0`
   (`mdss_mdp_wfd.c:259-266`); the register number and the CTL_TOP value must
   be chosen together.
2. **The blend stage ordinal** for a single pipe, and the exact `LM_OP_MODE`
   bits the driver sets for an opaque stage (`mdss_mdp_ctl.c:5110`, `:5166-5167`).
3. **VBIF / QoS / danger-safe LUTs** were not programmed in §5. The driver sets
   them from DT (`qcom,mdss-vbif-qos-*`, `mdss_mdp_intf_writeback.c:170-196`);
   defaults may or may not cause underruns at 1080p.
4. **The GDSC status bit** and the **clock halt bit** (§4.3).
5. **Whether the SMMU is really in bypass** (§4.4).

The cheapest way to resolve 1, 2, 3 and 5 is not more reading — it is a register
dump from a working Android boot on the same phone (§8).

---

## 6. The skeleton

`/root/mdss-blit-skeleton.c` (also `E:\ios-sweat\artifacts\gpu\mdss-blit-skeleton.c`)
implements exactly the table in §5: power on GDSC, ungate the five clocks, replay
`MDP_CLK_CTRL`, program DMA1 → LM1 → WB, kick CTL1, poll `INTR_STATUS`. It is
**not wired in, not compiled, and not tested**; every uncertain spot carries a
`TODO(unverified)` comment rather than a guess.

The only three XNU APIs it calls were checked to exist in this tree:
`IODelay(unsigned)` (`iokit/IOKit/IOLib.h:590`),
`kIODirectionOutIn` (`iokit/IOKit/IOMemoryDescriptor.h:64`) and
`IOMemoryDescriptor::withPhysicalAddress()` (`:519`). Everything else is
`volatile uint32_t *` accesses plus arithmetic, so the register logic is directly
reviewable even though it cannot be built on this Linux host.

**How it would replace the `memcpy`.** Today, per layer
(`SweetFramebuffer.cpp:393-399`):

```c
if (src != 0 && bpe == 4) {
        for (int32_t y = 0; y < h; y++) {
                memcpy((void *)(fPixels + (uint64_t)(dy + y) * fRowBytes +
                    (uint64_t)dx * 4),
                    (const void *)(src + s->offset +
                    (uint64_t)(sy + y) * s->bytesPerRow + (uint64_t)sx * 4),
                    (size_t)w * 4);
        }
}
```

becomes one call:

```c
sweet_mdss_blit(&g_mdss,
    src_phys_of(s) + s->offset, s->bytesPerRow, sx, sy,
    fPhys,                      fRowBytes,       dx, dy, w, h);
```

with `memcpy` retained as the fallback when the blit times out or the surface is
not contiguous. Three things make this safe to try:

* the destination is already a fixed physical window (`fPhys`, `:162`), so
  nothing about scanout changes;
* the interface (`present(const uint8_t *rec)`) and the swap record are
  untouched;
* failure is observable (timeout + `CTL_START` readback) and recoverable.

**The one thing that must be solved first** is `src_phys_of(s)`: surfaces are
page-allocated today (`SweetSurface.cpp:691-692`). Since the port owns the
surface allocator, the fix is a flag change plus a fallback, but the 10.4 MB
contiguity has to be *measured* (§8, step 3).

---

## 7. Effort, honestly

**Two weeks, realistic deliverable** (and it is not "a driver"):

* **Days 1-3 — resolve the unknowns (§8 steps 1-3).** Dump MDSS/MDP registers
  from a working Android boot of the same phone; decide CTL_LAYER variant, stage
  ordinal, VBIF defaults, SMMU state, and whether a 10.4 MB contiguous
  allocation succeeds. This is reading and measuring, and it is where the
  project either becomes easy or dies. *If the SMMU turns out to be programmed,
  everything below changes and the honest answer becomes "write an SMMU driver
  first", i.e. months.*
* **Days 4-6 — MDSS power/clock bring-up in XNU.** A ~100-line driver that
  powers the domain, ungates clocks, and reads `MDSS_HW_VERSION` at
  `0x0C900000` plus a MDP register round-trip. No display, no risk to the UI.
  This alone is a real milestone: it proves XNU can own the MMSS power domain.
* **Days 7-10 — boot-time blit self-test.** Blit a 256x256 pattern from a
  contiguous scratch buffer to another scratch buffer, verify by CPU read-back.
  No panel involvement, no interaction with CA.
* **Days 11-14 — wire into `present()` behind a boot-arg**, with `memcpy`
  fallback and a counter of hardware vs software presents in the log; measure
  CPU time in the vsync path and try `sweetfb_hz=60` again.

**What two weeks does not buy:** scaling (that is (c), another month with
coefficient tables and VIG pipes), tear-free page flipping, or anything
resembling 3D.

**What a real Adreno driver would take:** 6-12 person-months for a
freedreno-class bring-up under XNU (power/clock/GMU, CP ringbuffer, GPU SMMU,
microcode loading), plus the TrustZone/PIL problem which is **not solvable**
without Qualcomm-signed firmware and a secure-world client — and it would still
be invisible to Apple's Metal (§3(a)). **Do not start it.**

**What is impossible without Apple's closed source:** (i) making Metal or GLES
target non-Apple silicon (the shader compiler is in the userland we must keep
unmodified); (ii) knowing the accelerator user-client ABI without the dyld
shared cache; (iii) optimising `CA::OGL::SW`'s inner loops; (iv) the AGX command
stream format.

---

## 8. Verification recipe (cheapest path to certainty)

1. **Dump the MDP from a working Android boot on the same device.** Boot a
   LineageOS kernel on chiron with `mdss` debug enabled and capture
   `qcom,regs-dump-mdp` ranges (`msm8998-mdss.dtsi:147-188` lists exactly the
   ranges, names at `:189-205`, e.g. `"CTL_0" 0x02000-0x02094`, `"DMA1_SSPP"
   0x27000-0x27184`, `"WB_2" 0x66000-0x662c0`, `"LAYER_*"`,
   `"DEST_SCALER_*"`). Kick a known 2D operation (screen rotation or a
   screenshot, both use the rotator/WB) and dump again. The diff *is* the
   working programming sequence, including CTL_LAYER variant and blend stage.
2. **Read the MDP revision**: `MDSS_REG_HW_VERSION` at `0x0C900000`
   (`mdss_mdp.c:2249-2255` reads it into `mdata->mdp_rev`); it selects between
   QSEED2/QSEED3 and several CTL quirks.
3. **Measure the SMMU state and contiguity from XNU** (or from the shim, which
   already runs before XNU): read `0x0CD00000` (SMMU `sCR0`/`sACR`) and try a
   `kIOMemoryPhysicallyContiguous | kIOMemoryMapperNone` allocation of
   1080x2400x4 at boot; log both. Two numbers that decide the whole project.
4. **Only then** write the driver, using the skeleton as the checklist.

---

## 9. Artefacts

| path | what |
|---|---|
| `/root/gpu-research.md` | this report |
| `/root/mdss-blit-skeleton.c` | the skeleton blit driver (§6) |
| `E:\ios-sweat\gpu-research.md` | copy of this report (via `remote.py put`) |
| `E:\ios-sweat\artifacts\gpu\` | local copies of every file cited here, for line-number verification: `mdss_mdp_hwio.h`, `mdss_mdp_ctl.c`, `mdss_mdp_pipe.c`, `mdss_mdp_intf_writeback.c`, `mdss_mdp_formats.h`, `mdss_mdp.h`, `mdss_mdp.c`, `msm8998-mdss.dtsi`, `SweetFramebuffer.cpp`, `SweetSurface.{h,cpp}` |
| `/tmp/fmtcheck.c` (generated by `E:\ios-sweat\s_gpu_fmtcheck.sh`) | the constant-verification program of §5.2: re-implements the two format encoders and asserts `SRC 0x236FF`, `DST 0x237FF`, `PATTERN 0x03020001`, `CTL_FLUSH 0x11200`, plus the §5.1 address arithmetic |

No file in `/root/ios-sweet` was modified (`git status --porcelain` is empty at
`0cc62e6`); nothing was committed, flashed, rebooted or run on hardware.
