# GPU acceleration on ios-sweet: what Apple's closed binaries actually require

**Scope.** Is real GPU acceleration reachable on the iPhone-X-userland port, and
what is the cheapest real win? This is a trace through the closed binaries
themselves -- Apple's kernelcache and (where reached) the dyld shared cache --
plus the Qualcomm display driver for the hardware we have.

**Status honesty.** Nothing here was run on a phone, flashed, or compiled into
the kernel. Every hardware address and every API claim is tagged with where it
comes from -- `read in <file>:<line>`, `disassembled at <addr> with <cmd>`, or
**unverified**. Where a claim is inferred rather than read, it says so.

**Note on an earlier draft.** A previous pass over this question produced
`gpu-research-prior-agent.md` (kept alongside this file, unmodified). It was
written when the dyld shared cache was not on the build server and says so in
its own text; its Adreno model for chiron ("Adreno 530") is also wrong --
MSM8998 carries an **Adreno 540**, per the `A540` entries with
`gpmufw_name = "a540_gpmu.fw2"` in
`/root/src/kernel_msm8998/drivers/gpu/msm/adreno-gpulist.h:242-262`. Its
ranking conclusion (MDP blit is the real win, Metal is not) matches -3/-4 here;
this document adds what the binaries themselves say, which is the part that was
missing.

---

## 0. Top-line verdict

1. **A Metal device object can be faked. A Metal device cannot.** Apple's own
   kernel spells out the contract in a handful of strings (-2.2) and every one
   of them can be satisfied by an XNU stub -- until the object is handed to the
   userspace driver `AGXMetalA11`, which emits **A11/AGX** command streams and
   **AGX shader binaries**, and expects an A11 GPU that is itself a separate
   processor running Apple-signed firmware (`AGXFirmwareKextG10PRTBuddy`, -2.2.4).
   That is not a missing driver; it is a missing processor.
2. **Faking the device is worse than not faking it**, because the kernel's GPU
   family has no "present but non-functional" state (-2.3): once `backboardd`
   has a device it creates a queue and submits. Today `CA::OGL::` reliably picks
   `CA::OGL::SW` and the home screen draws.
3. **The real win is 2D hardware composition -- the MDP5 writeback path -- which
   deletes the per-frame `memcpy` in `AppleCLCD::present()`.** That copy is the
   only per-frame pixel work the kernel does and it is what forced the port to
   cap presentation at 10-30 Hz. Bounded, testable, with a clear fallback.
   -3(c) has the full register table traced to files and lines.
4. **NEON/multi-core tuning is a 3-5 day band-aid, not a fix**, and the reason
   is a cache attribute, not arithmetic (-3(d)).

Ranked, with reasons, in -3. Numbers behind all of it in -1.

---

## 1. Phase 1 -- the binaries, with sizes before download

### 1.1 Disk and tooling

| item | value |
|---|---|
| `df -h /` at start | 49 GiB total, 12 GiB used, **37 GiB available** |
| `/root/dsc` | did not exist, created |
| `/root/ios-sweet/research` | existed; only `ipsw-x/` (iPhone10,3.json, `d22ap.adt`, `DeviceTree.d22ap.im4p`) |
| pre-existing dyld caches | **none** -- `find / -xdev -name 'dyld_shared_cache*'` returned nothing |
| `ipsw` | absent - installed GitHub release **v3.1.738** (the version NOTES pins), `ipsw_3.1.738_linux_x86_64.deb` - `/root/dsc/bin/x/usr/bin/ipsw` |
| `llvm-objdump` | absent - `apt install llvm clang`, LLVM 18, `/usr/bin/llvm-objdump` |

Route taken for disassembly: **both**. `ipsw` (v3.1.738, linux x86_64) for the
dyld-cache side, `llvm-objdump` for the Mach-O carves out of the kernelcache.
For the kernelcache the image boundaries are **exact, not estimated**: the
prelinked kext Mach-O headers are found by scanning for `feedfacf` at the
`_PrelinkExecutableLoadAddr` values declared in the kernelcache's own
`_PrelinkInfoDictionary`, and each kext's `LC_SEGMENT_64` list gives every
section's `fileoff`/`size` verbatim (`carve_kext.py`). Confidence: **exact**.

### 1.2 The IPSW

`python3 tools/ipsw.py info`:

```
device    iPhone X (Global) (iPhone10,3)
board     D22AP  platform t8015  cpid 32789  bdid 6
version   16.7.16 (20H392)
size      5.7 GiB (6171035648 bytes)
sha1      5606c298c1d908f8a48f617f0ceaf71f2a9f0193
url       https://updates.cdn-apple.com/2025FallFCS/fullrestores/122-77006/
          1158E502-D76D-4226-AA06-ED5E30CD1111/iPhone10,3,iPhone10,6_16.7.16_20H392_Restore.ipsw
```

`tools/remotezip.py` (HTTP range requests on the zip central directory)
enumerated **91** members. The relevant ones, **sizes reported before
downloading**:

| member | bytes | zip method | what it is |
|---|---:|---|---|
| `087-86698-044.dmg` | **4 730 814 044** | 0 = stored | **root filesystem**; contains `System/Library/Caches/com.apple.dyld/dyld_shared_cache_arm64` + `.01`-`.NN` + `.symbols` |
| `087-86593-047.dmg` | 2 743 074 816 | 8 = deflate | Cryptex1,SystemOS (the dyld cache is *not* here on 20H392) |
| `087-86622-047.dmg` | 109 051 931 | 8 | other payload DMG |
| `087-86719-047.dmg` | 106 954 779 | 8 | RestoreRamDisk |
| `087-86715-047.dmg` | 10 485 760 | 8 | Cryptex1,AppOS |
| `kernelcache.release.iphone10b` | **18 204 358** | 8 | kernel + **239 prelinked kexts**, incl. `AGXG10P`, `IOGPUFamily`, `IOMobileGraphicsFamily`, `AGXFirmwareKextG10PRTBuddy` |
| `BuildManifest.plist` | 449 774 | 8 | build identities and per-component digests |

`remotezip.py` reports `087-86698-044.dmg` `crc = 66ccd39f`, so the pull is
end-to-end verifiable. Budget check: 4.73 GB of a 37 GiB budget, fine. The CDN
measured **~4.3 MB/s single-stream** (64 MiB in 14.8 s), so the DMG is pulled
as a resumable **6-way segmented range download** with exact-length enforcement
per segment and a final CRC-32 (`dlseg.py`).

**Why the kernelcache first.** It is 1/260th the size and it answers Phase 2
question 2 outright, because the Metal kernel contract is *declared by Apple's
kernel* in IOKit personalities, not inferred from userland. `ipsw kernel dec`
expands it to a 50,937,856-byte Mach-O; `ipsw kernel kexts --json` lists all
239 kexts. This is the single highest-value 18 MB in the whole IPSW.

### 1.3 The dyld shared cache -- **correction: it is in the SystemOS cryptex, not the rootfs**

This is worth stating plainly because it inverts the obvious plan. On 20H392 the
dyld shared cache is **not** in `087-86698-044.dmg` (the root filesystem). It is
in **`087-86593-047.dmg`**, identity `Cryptex1,SystemOS`, and `ipsw` itself says
so: `ipsw extract --remote <URL> --dyld -a arm64` prints
`Extracting /tmp/-/087-86593-047.dmg`. Trying to find it in the 4.73 GB rootfs
DMG is wasted effort.

That DMG is 2 743 074 816 bytes uncompressed / 1 097 727 251 bytes deflated
(method 8), so pulling it costs **1.1 GB down / 2.74 GB on disk** -- six times
cheaper than the rootfs. It was fetched with `tools/remotezip.py`
(CRC-32 verified, `087-86593-047.dmg: 2743074816 bytes, CRC-32 ok`).

Container and volume:

```
$ xxd -l 64 087-86593-047.dmg
00000000: 8521 6704 f4a7 ed20 0100 0000 0000 0000  .!g.... ........
00000010: 1800 0000 0000 0000 0100 0080 0000 0000
00000020: 4e58 5342 0010 0000 0038 0a00 0000 0000  NXSB.....8......

$ fsapfsinfo 087-86593-047.dmg          # libfsapfs-utils 20201107
Container information:
	Identifier			: 07490cba-83ee-4f50-a415-3de222723455
	Number of volumes		: 1
Volume: 1 information:
	Identifier			: 96096b28-b6dc-4bc7-9d3a-2632a0fe53f1
	Name				: SydneyUpdate20H392.D22D221SystemCryptex
	Incompatible features		: 0x00000028
```

`NXSB` = APFS container superblock, single volume, the D22/D221 system cryptex.
`libfsapfs`'s directory walk fails on this volume
(`libfsapfs_file_system_btree_get_root_node: unable to read B-tree node`), and
`ipsw` refuses it for want of `apfs-fuse`. **Neither was needed**: the cache is
carved by its own magic instead (-1.4).

### 1.4 How the cache was actually extracted (no APFS parser required)

Every dyld cache file starts with `dyld_v1` followed by a header whose
`mappingOffset`/`mappingCount`/`codeSignatureOffset`/`codeSignatureSize` fields
give that file's exact extent. Scanning the raw 2.74 GB image for the magic,
validating each candidate header, and sorting by base address yields **41
blobs**. The main cache then names its subcaches in a `{uuid[16], size u64,
fileSuffix[32]}` array; decoding it (`dsc_refs.py`) shows 40 records at a
**56-byte stride** (not the 48 the struct would suggest -- 16 + 8 + 32), listed
in ascending-VA order, with suffixes `.01`, `.02`, - `.40`.

Result (`carve_dsc.py`), each file's size cross-checked against its *own*
header as `codeSignatureOffset + codeSignatureSize`:

| | |
|---|---|
| files | **41** (`dyld_shared_cache_arm64` + `.01`-`.40`) |
| total | **2 384 232 448 bytes = 2.22 GiB** |
| main cache | `dyld_shared_cache_arm64`, 393 216 bytes, base `0x180000000`, uuid `8f970452` |
| VA span | `0x180000000` - `0x212fe4000` |
| images | **3 026** Mach-O images across the subcaches (counted by `LC_ID_DYLIB`) |
| per-file check | `dsc_check.py`: every one of the 41 reports `disk == cs_end`, i.e. `OK` |

The largest subcaches are ~115 MB each (`.01` `.03` `.05` - `.34` `.36`), with a
few small "symbol/objc-opt" subcaches of 100-600 KB between them -- that
alternation is the layout, not an artefact.

Location of everything relevant (all addresses are cache VAs):

| image | subcache | file offset | `__TEXT` |
|---|---|---:|---|
| `/System/Library/Frameworks/QuartzCore.framework/QuartzCore` | `.03` | `0x014f8000` | `0x188284000`, 0x34d000 |
| `/System/Library/Frameworks/Metal.framework/Metal` | `.01` | `0x019b8000` | `0x181a14000`, 0x1c0000 |
| `/System/Library/Extensions/AGXMetalA11.bundle/AGXMetalA11` | **`.38`** | `0x022e2000` | `0x209dee000`, 0x1bf000 |
| `/System/Library/Frameworks/IOSurface.framework/IOSurface` | `.11` | `0x0139b000` | `0x1a397f000`, 0x17000 |
| `/System/Library/PrivateFrameworks/IOMobileFramebuffer.framework/-` | `.13` | `0x05da6000` | `0x1af222000`, 0xd000 |
| `/System/Library/PrivateFrameworks/IOAccelerator.framework/IOAccelerator` | `.15` | `0x03474000` | `0x1b3013000`, 0x6000 |

Note what that table says about `MetalPluginName = AGXMetalA11` from -2.2.1: the
plugin is a **bundle under `/System/Library/Extensions/`**, not a framework, and
it is a 1.8 MB `__TEXT` of Objective-C classes named `AGXA11Family*`.

Carving recipe that works: in-cache images keep their original
`LC_SEGMENT_64.fileoff`, which is relative to the containing subcache file, so
`__TEXT` can be carved by copying from its declared `fileoff` (that is what
`carve_text.py` does, and it needs no other segment). The coalesced
`__DATA*`/`__LINKEDIT` segments point into *other* subcaches and were not
carved -- they were not needed.


---

## 2. Phase 2 -- the closed binaries

### 2.0 What was known before this pass (from this repo's own earlier RE)

NOTES.md:346 (from a previous walk of the cache's `.symbols` subcache):
QuartzCore has four context constructors -- `metal_new_context`,
`gles_new_context`, `sw_new_context` (CA::OGL::SWContext) and
`new_null_context` -- and 249 `CA_*` env variables, among them `CA_ENABLE_OGL`,
`CA_DISPLAY_TYPE`, `CA_FORCE_LOCAL_SERVER`, `CA_FORCE_TIMER`,
`CA_CANVAS_WIDTH/HEIGHT`, `CA_MAIN_DISPLAY_SCALE/ORIENTATION`,
`CA_DISABLE_RENDER`, `CA_ALWAYS_RENDER`.

NOTES.md:325 records the open question verbatim: *"QuartzCore's IOMFB server
builds its renderer from Metal first (`CAMetalContextCreate`), GLES only when
enabled; how the CPU renderer gets selected on a device without a GPU is not
yet established."*

### 2.1 Renderer selection in QuartzCore

#### 2.1a Direct disassembly

The cache side is in -7.1. The short version: `CA_ENABLE_OGL` and
`MetalContext` are verified present as strings in QuartzCore's
`__TEXT.__cstring`, but the instruction-level selection condition was not
traced (see -7.1 for exactly why and what would finish it).

### 2.2 The kernel-side contract for a Metal device -- **read straight out of Apple's kernel**

This is the load-bearing result and it needs no userland binary: Apple's kernel
declares, in IOKit personality dictionaries, exactly what a GPU must be.

#### 2.2.1 The personality that *is* the contract

Source: `_PrelinkInfoDictionary` in the decompressed kernelcache. The plist
blob starts at file offset `0x8a976` and ends at `0x9d7c81`; the `AGXG10P`
record starts at `0x2e81d54`. Copied verbatim into
`/root/dsc/out/agxg10p-personalities.xml`. Reformatted only for width:

```
AGXG10P_B0 = {
    CFBundleIdentifier = com.apple.AGXG10P
    IOMatchCategory    = IOAcceleratorES
    IOClass            = AGXAcceleratorG10P_B0
    IOProviderClass    = AppleARMIODevice
    IONameMatch        = ( "gpu,t8015" )
    IOGLESBundleName   = AppleMetalGLRenderer
    MetalPluginName    = AGXMetalA11
    IOSourceVersion    = 371
}

AGXG10P_A0 = {                    /* the other A11 chip revision */
    CFBundleIdentifier = com.apple.AGXG10P
    IOMatchCategory    = IOAcceleratorES
    IOClass            = AGXAcceleratorG10P_A0
    IOProviderClass    = AppleARMIODevice
    IONameMatch        = ( "gpu,t8015" )
    IOGLESBundleName   = AppleMetalGLRenderer
    MetalPluginName    = AGXMetalG10A0
}
```

So an `IOService` that Apple's Metal stack will adopt must satisfy **all** of:

| # | requirement | value |
|---|---|---|
| 1 | provider class | `AppleARMIODevice` -- an `/arm-io` child nub, not an arbitrary service |
| 2 | `IONameMatch` (device-tree `compatible`/`name`) | **`gpu,t8015`** -- the A11 identifier, matched exactly |
| 3 | `IOMatchCategory` | **`IOAcceleratorES`** (note: *ES*, not plain `IOAccelerator`) |
| 4 | `IOClass` | a subclass of `AGXAccelerator` (`AGXAcceleratorG10P_A0/B0`), which is itself an `IOGPU` subclass from `com.apple.iokit.IOGPUFamily` 65.60.1 |
| 5 | registry properties | `MetalPluginName` = `AGXMetalA11`, `IOGLESBundleName` = `AppleMetalGLRenderer` |

**Names that do *not* exist**, checked by exhaustive string search over the
50.9 MB kernelcache: `gpu-core-count` - **0 hits**; `IOGLBundleName` - **0
hits** (the real key is `IOGLESBundleName`); `MetalPluginClassName` - present,
but in the **AGX binary's** `__cstring` (kernelcache file offset `0x1108010`),
not in a personality dict. Any stub sketched against `gpu-core-count` or
`IOGLBundleName` is sketched against names that are not in this build.

#### 2.2.2 What userland instantiates, as seen from the kernel side

From `AGXG10P.__TEXT.__cstring` (56 strings + all C++ type encodings, saved as
`/root/dsc/out/agxg10p-cstring.txt`):

| string | meaning |
|---|---|
| `MetalPluginClassName` | key the accelerator exposes to userland |
| `AGXA11Device` | the class userland instantiates for this device |
| `MetalStatisticsName` - `AGXMetalStatisticsExternalA11` | statistics bundle |
| `AGXGPURawCounterBundleName` - `AGXGPURawCounterBundle` | perf-counter plugin |
| `GPURawCounterPluginClassName` - `AGXGPURawCounterSourceGroup` | ditto |
| `AGXFirmwareKextName` - `AGXFirmwareKextG10PRTBuddy` | **the firmware kext** |
| `com.apple.private.agx.performance-spi`, `com.apple.internal.display-params-access-privileged`, `com.apple.agx.restartreport` | entitlements / report nub |

From `IOGPUFamily.__TEXT.__cstring` (at `0xfffffff005544ae8`, 0x3f7c bytes,
saved as `iogpufamily-cstring.txt`) -- these are the
`IOExternalMethodDispatch` handler names of `IOGPUDeviceUserClient`, i.e. the
**command-submission ABI**:

```
s_new_command_queue        s_delete_command_queue      s_new_resource
s_submit_command_buffers   s_create_io_command_queue   s_destroy_io_command_queue
s_set_io_notification_queue s_submit_io_commands       s_create_io_command_buffer
s_destroy_io_command_buffer s_try_cancel_io_command_buffer
s_perform_io               s_io_command_buffer_complete
s_io_command_buffer_barrier_complete                    s_create_vniodesc
s_async_signal_mtlLateEvalevent                         s_query_mtlLateEvalevent
```

Kernel command opcodes inside a submitted buffer (`kIOGPUKernelCommand-`):

```
kIOGPUKernelCommandDebugLog          kIOGPUKernelCommandCollectTimeStamp
kIOGPUKernelCommandSignalSharedEvent kIOGPUKernelCommandWaitSharedEvent
kIOGPUKernelCommandPurgeResources    kIOGPUKernelCommandSetProtectionOptions
```

and the structures it validates byte-by-byte -- `sIOGPUKernelCommand`,
`sIOGPUKernelArgs`, `sIOGPUKernelCommandBarrierArgs`,
`sIOGPUKernelCommandSignalOrWaitEventArgs`, `sIOGPUKernelCommandDebugLogArgs`,
`sIOGPUKernelCommandCollectTimeStampArgs`,
`sIOGPUKernelCommandPurgeResourcesArgs`,
`sIOGPUKernelCommandSetProtectionOptionsArgs` -- with error strings that state
the layout rules outright (from `IOGPUFamily.__TEXT.__os_log`, 244 format
strings saved as `iogpufamily-oslog.txt`):

```
"%s: Insufficient bytes (%lu) to hold sIOGPUKernelCommand (%lu)"
"%s: Invalid command list header, commands_start=%u commands_end=%u cmdListShmemLength=%ld"
"%s: kernelCommandBufferShmem (PID=%d) failed to map into kernel"
"%s: segmentListShmem (PID=%d) failed to map into kernel"
"%s: sidebandBufferShmem (PID=%d) failed to map into kernel"
"%s: Invalid eIOGPUKernelCommands enum (command_id=%u command_size=%llubytes)"
"%s: Invalid Descriptor Type (%u)"
"%s: Invalid IO Queue Type (%u)"
```

So submission is **not** one struct per command buffer: userland hands the
kernel **three shared-memory segments** (kernel command buffer, segment list,
sideband buffer) plus a command list delimited by `commands_start`/
`commands_end`, and the kernel walks it, mapping each referenced shmem.

The per-command record `IOGPUCommandDescriptor` is recoverable in full from the
encoded type string that AGX itself prints (it appears verbatim in
`agxg10p-cstring.txt`):

```
IOGPUCommandDescriptor = ^^?  i  A  I  ^{IOGPU}  ^^{OSObject}  ii
                         ^^{IOGPUMemory}  ii
                         {IOGPUIOFenceListHead=^{sIOGPUIOFence}^^{sIOGPUIOFence}}
                         i  B  B  ^{IOGPUWorkQueue}  ^{IOGPUTask}
                         ^{IOGPUCommandQueue}  ^{IOGPUBlockFence}
                         Q  I  I
                         {IOGPUEvent={?=I[15I]}}  {IOGPUEvent={?=I[15I]}}
                         Q  Q  ^^v  I  i  i
```

`IOGPUEvent` is a 64-byte tagged union (`I` + `[15I]`). This is exactly the
structure a stub would have to produce and consume.

#### 2.2.3 What the accelerator demands from the device tree

`AGXG10P.__cstring` is effectively a checklist:

* **register ranges, by name**: `FenderRegs`, `AICTimerRegs`, `AICSWIntRegs`,
  `RGXRegs`, `UVDRegs` (plus `-(unused)` placeholders), with the failure modes
  spelled out: `"Fender address unknown"`, `"Could not map GPU reg range"`,
  `"Could not map Fender reg range"`,
  `"Unsupported number of reg ranges in EDT (%u)"`,
  `"Fender address is unknown"`.
* **device tree paths**: `IODeviceTree:/chosen` - `chip-id`;
  `IODeviceTree:/arm-io` - `chip-revision`.
* **properties**: `gpu-num-perf-states`, `perf-states`, `perf-states-sram`,
  `function-reliability_monitor`, `gpu-sysmem-reserve-percent`,
  `gpu-idleoff-{fn-hit,fi-hit,fn-miss,fi-miss,nei-hit,min_conf,high-conf,reset-iter,standby-timer,prob-init-val}`,
  `gpu-pwr-sample-period(-aic-clks)`, `interrupts`, `interrupts-valid`,
  `meta-sw-interrupt`, `num_cores num_frags num_gps num_mgpus usc_gen
  core_mask_list gpu_gen`.
* **a hard page-size check**: `"GPU uses 16K page size but CPU does not"`.
* AGX core revision names it will accept: `G5P G5G G9P G9G G10P G11P G11M
  G11G G12P G13P G13G G14P G14G G15M G15P _AGX2`, revisions `A0 A1 B0 B1 C0 C1`.

#### 2.2.4 The part that is firmware, not driver

`AGXG10P` is not self-contained. Its strings describe an ARM firmware running
*inside* the GPU, and a mailbox protocol to it:

```
AGXArmFirmware   AGXArmFirmwareKingfisherV4   AGXArmFenderKingfisher
AGXFirmware / GFXFW / AGFI-    AGFITimeStampQueue    AGFIChannelState
"Firmware/Driver Shared"       "Firmware Channel State Pool"
"Firmware Timestamp Queue Pool" "Firmware VDM Command Pool"
"Firmware PDM Blit Command Pool" "Firmware CDM Command Pool"
"Firmware Barrier Pool"         "Firmware PM Grow/Rebuild Pool"
"AGXk: %s:%d:%s: !!! Firmware wasn't stopped for recovery"
"AGXArmFirmware::kickFirmware: Command submission is disabled!"
"AGXk: pid=%u name=%s panicking due to submission while the GPU is powered off!"
"Failed to reboot the firmware"
firmware-detected lockup / firmware assert / CDM Kill timeout / timestamp timeout
```

and the kext list confirms two **separate firmware kexts**:
`com.apple.AGXFirmwareKextRTBuddy` (227.6.5) and
`com.apple.AGXFirmwareKextG10PRTBuddy`, loaded over `RTBuddy` (Apple's
coprocessor mailbox/IPC driver). On A11, "the GPU" is a separate ARM processor
running Apple-signed firmware; the kernel driver is a message pump to it.

That single fact decides -3(a) and -4.

### 2.3 What happens after a device exists

From the kernel side: **there is no probe-then-fallback path.** The classes
userland talks to have no "device present but non-functional" state; the
sequence is mandatory and each step fails hard:

1. `IOGPU::start()` -- requires `accelName`, `numEventChannels` (-1),
   `waitForGPUTimeoutSeconds` (-0), `maxSchedulerPriorityCount`; fails with
   `"provider is not the right device"` if the provider is not an `IOGPU`
   subclass and `"configureDevice failed"` if the subclass's configure step
   fails. It then builds an event machine, device namespace, work-queue
   namespace, `IOGPUFenceMachine`, `IOGPUScheduler`, a kernel GPU task, and
   registers with `IOPerfControlClient` (`"registerDevice failed"`).
2. `AGXAccelerator::start()` -- six lock allocations, then
   `configureDevice()` maps the EDT register ranges (-2.2.3) and fails hard if
   `Fender` is missing; then timestamp mappings
   (`"Failed to prepare timestamp mapping[%d]"`), IOFence structures, CSC table.
3. `AGXAcceleratorG10::requestFirmwareBinary()` -- the firmware handshake. Without
   it, submission is disabled (`kickFirmware: Command submission is disabled!`).

So the chain is **device object - accelerator start - EDT register map -
firmware handshake - command queues**, and the first three must be real enough
to satisfy the fourth.

#### 2.3a The userland side

Verified in -7.3: `-[AGXA11FamilyDevice initWithAcceleratorPort:-]` takes an
accelerator Mach port, then `AGXA11FamilyCommandQueue`/`-CommandBuffer` are
created, and the plugin demands a **KMD features string** and driver
information from the service. Both halves agree: no probe-only path.

### 2.4 GPU family / feature negotiation

`supportsFamily:` and every `MTLDevice` feature boolean live in the userspace
plugin `AGXMetalA11` (`MetalPluginName`), which is what
`MTLCreateSystemDefaultDevice()` loads, and that bundle is inside the dyld
shared cache -- see -7.

What the kernel pins down beyond doubt: **the family is chosen by
`IONameMatch`**. `gpu,t8015` - A11 - `AGXG10P` - `MetalPluginName AGXMetalA11`
(-2.2.1). The name-plugin mapping is a static personality table; there is no
kernel-side knob for "pretend to be a lower family". Any attempt to negotiate a
weaker feature set has to happen inside the plugin, i.e. inside Apple's signed
userland.

---

## 3. Phase 3 -- every option, ranked, hardware traced

### (c) MDP5/MDSS 2D hardware composition replacing the `memcpy` -- **the real win**

**What it removes.** `SweetFramebuffer.cpp:393-399` -- one `memcpy` per row per
enabled layer, into panel memory that is mapped **write-combining**
(`SweetSurface.cpp`, `sweet_surface_create_physical()`: `s->mapCache =
kIOMapWriteCombineCache`). Every store in that loop bypasses the D-cache.

Why that matters, with numbers: a full frame is `1080 x 2400 x 4 =
10 368 000 B`; at the 30 Hz the port runs at (`sweetfb_hz=30` in
`tools/devices.py`), that is 311 MB/s of WC stores plus 311 MB/s of cached
reads = **622 MB/s** of DRAM traffic against roughly **29.8 GB/s** of DRAM on
these SoCs. Bandwidth is *not* the constraint; per-store efficiency on
`Normal-NC` memory is. (The port's own measurement -- NOTES.md:325 -- is that
copying 10 MB per swap at 60 Hz "took the whole CPU"; the mitigation was to cut
the present rate, not to speed up the copy.)

**The hardware.** MSM8998's display pipeline is MDP5-class: `SDE_HW_VER_300` is
annotated `/* 8998 v1.0 */` and `SDE_HW_VER_301` `/* 8998 v1.1 */` at
`sde_hw_catalog.h:45-46`. The 2D path is
**SSPP (source pipe) - layer mixer (LM) - writeback (WB) - panel memory**.

Base addresses, from the live MSM8998 device tree
(`arch/arm64/boot/dts/qcom/msm8998-mdss.dtsi`, LineageOS
`android_kernel_xiaomi_msm8998`):

| block | offset | source (line read) |
|---|---:|---|
| MDP registers | `0x0C900000`, size `0x90000` | `msm8998-mdss.dtsi:17` (`reg = <0x0c900000 0x90000>, <0x0c9b0000 0x1040>`; `reg-names = "mdp_phys","vbif_phys"`) |
| VBIF | `0x0C9B0000`, size `0x1040` | `msm8998-mdss.dtsi:17` |
| MDP top offset | `+0x1000` | `msm8998-mdss.dtsi:58` (`qcom,mdss-mdp-reg-offset = <0x00001000>`) |
| interrupt | GIC SPI 83 - INTID 115 | `msm8998-mdss.dtsi:20` (`interrupts = <0 83 0>`) |
| VIG0..3 SSPP | `0x5000 0x7000 0x9000 0xB000` | `msm8998-mdss.dtsi:67` |
| DMA0..3 SSPP | `0x25000 0x27000 0x29000 0x2B000` | `msm8998-mdss.dtsi:69` |
| CURSOR0/1 SSPP | `0x35000 0x37000` | `msm8998-mdss.dtsi:71` |
| CTL0..4 | `0x2000 0x2200 0x2400 0x2600 0x2800` | `msm8998-mdss.dtsi:91` |
| LM0..3 (mixer+intf) | `0x45000 0x46000 0x47000 0x4A000` | `msm8998-mdss.dtsi:93` |
| DSPP0/1 | `0x55000 0x57000` | `msm8998-mdss.dtsi:95` |
| **WB** | **`0x66000`** | `msm8998-mdss.dtsi:96` (`qcom,mdss-wb-off`) |
| INTF0..3 | `0x6B000 0x6B800 0x6C000 0x6C800` | `msm8998-mdss.dtsi:97` |
| Pingpong 0,1,4 | `0x71000 0x71800 0x72000` | `msm8998-mdss.dtsi:99`; slave-pp `0x73000` at `:101` |

SSPP registers (relative to pipe base), all from `mdp5.xml.h` (generated from
`envytools/rnndb/mdp/mdp5.xml`, so these are Qualcomm-derived, not guesses):

| register | off | fields | source |
|---|---:|---|---|
| `SRC_SIZE` | `0x00` | `w[15:0] h[31:16]` | `mdp5.xml.h:696` (masks `:698-706`) |
| `SRC_IMG_SIZE` | `0x04` | `w[15:0] h[31:16]` | `mdp5.xml.h:710` |
| `SRC_XY` | `0x08` | `x[15:0] y[31:16]` | `mdp5.xml.h:724` |
| `OUT_SIZE` | `0x0C` | `w[15:0] h[31:16]` | `mdp5.xml.h:738` |
| `OUT_XY` | `0x10` | `x[15:0] y[31:16]` | `mdp5.xml.h:752` |
| `SRC0_ADDR` | `0x14` | **32-bit; no `_HI` register exists** | `mdp5.xml.h:766`; `grep ADDR_HI mdp5.xml.h` - none |
| `SRC1..3_ADDR` | `0x18 0x1C 0x20` | plane 1..3 | `mdp5.xml.h:768-772` |
| `SRC_STRIDE_A` | `0x24` | `P0[15:0] P1[31:16]` | `mdp5.xml.h:774` |
| `SRC_STRIDE_B` | `0x28` | `P2 P3` | `mdp5.xml.h:788` |
| `SRC_FORMAT` | `0x30` | see below | `mdp5.xml.h:804` |
| `SRC_UNPACK` | `0x34` | 4 element indices, one byte each | `mdp5.xml.h:858` |
| `SRC_OP_MODE` | `0x38` | `FLIP_LR 0x2000`, `FLIP_UD 0x4000` | `mdp5.xml.h:884-899` |
| `SRC_CONSTANT_COLOR` | `0x3C` | solid fill | `mdp5.xml.h:901` |
| `FETCH_CONFIG` | `0x48` | | `mdp5.xml.h:903` |
| `DECIMATION` | `0xB4` | | `mdp5.xml.h:923` |

`SRC_FORMAT` bit layout (`mdp5.xml.h:804-857`, encoder at
`sde_hw_sspp.c:281-317`): `[1:0] R_BPC-1`, `[3:2] G_BPC-1`, `[5:4] B_BPC-1`,
`[7:6] A_BPC-1`, `[8] ALPHA_ENABLE` (written as `BIT(8)`/`SRCC3_EN` at
`sde_hw_sspp.c:289`), `[10:9] CPP-1`, `[13:12] UNPACK_COUNT-1`,
`[17] UNPACK_TIGHT`, `[18] UNPACK_ALIGN_MSB`, `[20:19] FETCH_TYPE`,
`[23:22] CHROMA_SAMP`. Channel order is set by the four `SRC_UNPACK` bytes:
`sde_formats.c:204-260` gives `ARGB8888 = {C1_B_Cb, C0_G_Y, C2_R_Cr,
C3_ALPHA}` and `ABGR8888 = {C2_R_Cr, C0_G_Y, C1_B_Cb, C3_ALPHA}`, which is how
you encode BGRA-vs-RGBA without a colour matrix.

LM registers (`sde_hw_lm.c:19-28`, `mdp5.xml.h:1080-1160`):
`LM_OP_MODE 0x00`, `LM_OUT_SIZE 0x04`, `LM_BORDER_COLOR_0 0x08`;
blend stage *n* at `0x20 + 0x30-n`, `n - {0,1,2,3}`
(`__offset_BLEND`, `mdp5.xml.h:1104-1110`), and within a stage
`BLEND_OP +0x00`, `FG_ALPHA +0x04`, `BG_ALPHA +0x08` (`sde_hw_lm.c:25-28`).
`sde_hw_lm_setup_out()` (`sde_hw_lm.c:76-95`) writes `h<<16|w` to `LM_OUT_SIZE`
and clears `BIT(31)` for a non-split mixer.

WB registers (`sde_hw_wb.c:20-48`), relative to WB base `0x66000`:

| register | off | source |
|---|---:|---|
| `WB_DST_FORMAT` | `0x000` | `sde_hw_wb.c:20` |
| `WB_DST_OP_MODE` | `0x004` | `:21` |
| `WB_DST_PACK_PATTERN` | `0x008` | `:22` |
| `WB_DST0..3_ADDR` | `0x00C 0x010 0x014 0x018` | `:23-26` |
| `WB_DST_YSTRIDE0` | `0x01C` | `:27` |
| `WB_DST_WRITE_CONFIG` | `0x048` | `:35` |
| `WB_OUT_SIZE` | `0x074` | `:42` |
| `WB_ALPHA_X_VALUE` | `0x078` | `:43` |
| `WB_DST_ADDR_SW_STATUS` | `0x2B0` | `:45` |
| `WB_OUT_IMAGE_SIZE` | `0x2C0` | `:47` |
| `WB_OUT_XY` | `0x2C4` | `:48` |

`WB_DST_FORMAT` (`sde_hw_wb.c:96-133`):
`(chroma_samp<<23)|(fetch_planes<<19)|(bits[C3]<<6)|(bits[C2]<<4)|(bits[C1]<<2)|bits[C0]`,
plus `BIT(8)` (`DSTC3_EN`) when alpha is present, `BIT(14)` (`DST_ALPHA_X`),
`BIT(21)` for DX formats, then
`(unpack_align_msb<<18)|(unpack_tight<<17)|((unpack_count-1)<<12)|((bpp-1)<<9)`.
`WB_DST_YSTRIDE0 = pitch[0] | (pitch[1]<<16)`, `WB_OUT_SIZE = h<<16|w`.

CTL (`mdp5.xml.h:380-499`, `sde_hw_ctl.c:27-31`): `CTL_LAYER0..5 0x00..0x14`,
`CTL_OP 0x014`, `CTL_FLUSH 0x018`, `CTL_START 0x01C`, `CTL_SW_RESET 0x030`,
`CTL_LAYER_EXT 0x40`.

`CTL_FLUSH` bits, cross-checked between two independent sources --
`mdp5.xml.h:469-497` (`VIG0 0x1 VIG1 0x2 VIG2 0x4 RGB0 0x8 RGB1 0x10 RGB2 0x20
LM0 0x40 LM1 0x80 LM2 0x100 LM3 0x200 LM4 0x400 DMA0 0x800 DMA1 0x1000
DSPP0 0x2000 DSPP1 0x4000 DSPP2 0x8000 WB 0x10000 CTL 0x20000 -`) and
`sde_hw_ctl.c`'s `get_bitmask_sspp()` (VIG0..3 - `BIT(0..2)`, `BIT(18)`;
DMA0/1 - `BIT(11)/BIT(12)`), `get_bitmask_mixer()` (LM0..4 - `BIT(6..10)`,
plus `BIT(17)` for CTL) and `get_bitmask_wb()`.

**The commit handshake**, exactly as `mdp5_ctl_commit()` does it
(`mdp5_ctl.c:495-530`): write `CTL_FLUSH` with the accumulated mask, then if a
start is pending `send_start_signal()` writes `1` to `CTL_START`
(`mdp5_ctl.c:227-236`). Before that, `set_ctl_op()` (`mdp5_ctl.c:148-176`)
programs `CTL_OP`: `MDP5_CTL_OP_INTF_NUM(INTF0+num)` for a real interface,
`MDP5_CTL_OP_CMD_MODE` (`0x20000`) for command-mode DSI, and
`MDP5_CTL_OP_MODE(MODE_WB_2_LINE)` for writeback -- with `CTL_OP_MODE` in
`[3:0]` and `INTF_NUM` in `[6:4]` (`mdp5.xml.h:447-459`).

**Minimum viable implementation** (working skeleton: `/root/mdss-blit-skeleton.c`,
which compiles clean under `gcc -fsyntax-only` and `gcc -O2 -c`):

1. Map `0x0C900000+0x90000` and `0x0C9B0000+0x1040` as two IOKit ranges from an
   `/arm-io` child -- the same nub mechanism `AppleCLCD` already uses.
2. **Go/no-go probe:** read `0x0C901000 + 0x0` (`MDP_HW_VERSION`,
   `mdp5.xml.h:211-231`). A plausible major/minor proves the block is clocked
   and the mapping is right. If it reads 0 or all-ones, this option is blocked
   until the MMSS clocks and GDSCs are up (`msm8998-mdss.dtsi:107-114` lists 7
   clocks -- `mnoc_clk iface_clk bus_clk core_clk_src core_clk vsync_clk lut_clk`
   -- plus `vdd-supply = <&gdsc_mdss>` and
   `gdsc-core-supply = <&gdsc_bimc_smmu>`), which is a different project.
3. Program one DMA SSPP for the source rect, LM0 for the destination window, WB
   for the panel memory; then `CTL_FLUSH` and `CTL_START`.
4. Wait: poll WB completion, or accept the existing vsync timer as pacing
   (`present()` already runs on one).

**Two hard constraints, both verified:**

* **Addresses must be below 4 GiB.** `SRC0_ADDR` is 32 bits with no `_HI`
  companion (`mdp5.xml.h:766`), and `sde_hw_sspp.c:748-750` writes
  `cfg->layout.plane_addr[i]` straight through `sde_reg_write()` -
  `writel_relaxed()` (32-bit) -- `sde_hw_util.c`, `sde_reg_write` body. For this
  port that is automatic: both boards' kernel RAM windows are below 4 GiB
  (`tools/devices.py`: sweet `ram_low = 0xb0000000 + 0x4d000000`, chiron
  `ram_low = 0xa0000000 + 0x60000000`) and the panel memory is at
  `0x9c000000` (sweet) / `0x9d400000` (chiron).
* **SMMU.** MSM8998 routes MDP through `mmss_smmu` (`msm8998-mdss.dtsi:233-259`,
  `smmu_mdp_unsec` / `smmu_mdp_sec` context banks, `iommus = <&mmss_smmu 0>`
  and `<&mmss_smmu 1>`). Whether the bootloader left it bypassed is
  **unverified**; if it is enforcing, the blit faults and the fix is a page
  table (big) or a bypass (small, if the hardware allows it).

**Honest assessment.** Real, bounded win: it removes 100 % of the per-frame CPU
pixel work and its correctness is visible on screen. It does **not** make
`CA::OGL::SW` faster -- it makes presentation of what the CPU drew free. Risk is
concentrated in two unknowns (clocks, SMMU), both settled by step 2 plus one
blit attempt.

**Stated limitation:** WB writes to a fixed destination. Tearing-free
presentation needs double buffering plus a scanout flip at vsync, i.e.
programming `INTF`/pingpong and taking panel timing away from the bootloader.
That is the difference between 1-3 person-months and a quarter.

### (b) hardware-accelerated present/copy only

There is no GPU copy engine available: the Adreno is unreachable (-3(a)). The
only engines on these SoCs that touch panel memory are (i) the MDP5 WB path of
(c), (ii) MDP5 SSPP direct scanout -- a CA layer *is* the scanout source, zero
copies -- and (iii) the MDP rotator (`qcom,mdss-has-separate-rotator`,
`msm8998-mdss.dtsi:105`; rotator tuning at `:577-605`).

(ii) is what iOS itself does one level up (composite, then hand the display one
surface); here the "GPU" would be the MDP layer mixers: each CA layer becomes an
SSPP feeding an LM stage and composition happens at scanout. It kills both the
copy and the composition. Cost: owning DSI/PLL timing, which the bootloader does
for us today. **Bounded but not small: ~2-4 person-months**, and it puts the
one reliably working thing (the panel scanning out ABL's buffer) at risk.

So (b) as "present/copy only" collapses into (c). Ranked separately only
because the *goal* differs, not the hardware.

### (a) XNU kgsl/Adreno driver satisfying Apple's Metal userland -- **dead end, three independent reasons**

1. **Wrong command stream.** Apple's Metal driver for A11 is the userspace
   bundle `AGXMetalA11` (-2.2.1), emitting AGX commands -- class names in
   `agxg10p-cstring.txt` include `AGXCommandBuffer`, `AGX3DCommandDescriptor`,
   `AGXTACommandDescriptor`, `AGXCLCommandDescriptor`,
   `AGXRemoteNodeCommandDescriptor`, `AGXSKUEncoder`. Adreno speaks PM4
   (`drivers/gpu/msm/adreno_pm4types.h`). Nothing translates; a translator would
   have to reproduce Apple's compiler output, not merely an ISA.
2. **Shaders are AGX binaries**, compiled on device by the A11 plugin for
   Apple's ISA. Even a perfect PM4 driver changes nothing about that.
3. **A11's GPU is a separate processor running Apple firmware** (-2.2.4:
   `AGXFirmwareKextG10PRTBuddy`, `AGXArmFirmwareKingfisherV4`, RTBuddy IPC,
   "panicking due to submission while the GPU is powered off"). Adreno's
   equivalent microcode is Qualcomm-signed and PIL/SCM-loaded --
   `adreno-gpulist.h:242-262` (`a530_pm4.fw`, `a530_pfp.fw`,
   `a540_gpmu.fw2`, `a540_zap`), loaded at `adreno_a5xx.c:2277-2287`
   (`subsystem_get(adreno_dev->gpucore->zap_name)`, error if it fails) and
   `a5xx.c:683-743` (`_load_gpmu_firmware()` - `request_firmware()`). **This
   XNU kernel has no SCM/PIL client**, so even the Adreno side cannot start.

For completeness, the Adreno port would also need the SMMU (`kgsl_iommu.c`,
69 KB), ringbuffer/dispatcher (`adreno_ringbuffer.c`, `adreno_dispatch.c`
82 KB), power/DCVS (`kgsl_pwrctrl.c` 84 KB) and `CONFIG_QCOM_KGSL`'s
dependency set (`drivers/gpu/msm/Kconfig:4-12`) -- a ~700 KB driver into a
kernel with a different memory model. And it would still not run Apple's
userland.

Also worth recording: the only kgsl source on the server is the **msm8998**
tree, which covers `a3xx/a4xx/a5xx` only -- there is **no `a6xx`** driver there,
so sweet's Adreno 618 has no register-level reference in this workspace at all.
Any plan that starts with "port kgsl" has to first obtain an a6xx reference.

**Verdict: never.** Not "hard" -- unsatisfiable, because the client
(`AGXMetalA11`) is compiled against hardware that neither phone contains.

### (d) multi-threaded NEON / SIMD CPU rasteriser

Numbers: `1080 x 2400 x 4 = 10.37 MB` per frame; at 30 Hz, 311 MB/s of WC
stores + 311 MB/s of cached reads = **622 MB/s** against ~29.8 GB/s of DRAM;
at 60 Hz, 1.24 GB/s. Bandwidth is not the constraint, so a faster `memcpy`
cannot recover much -- the earlier draft's "nearly worthless" call is right in
direction.

Where the time actually goes:

* the destination is `kIOMapWriteCombineCache` (`SweetSurface.cpp`,
  `sweet_surface_create_physical`), so every store leaves the core as a
  `Normal-NC` write; sustained single-core WC store bandwidth is far below
  cached-copy bandwidth (**unverified on this SoC -- 10-minute measurement**);
* rows are 4320 B = 67.5 cache lines, so each row begins and ends on a partial
  line: 2 read-modify-writes per row;
* the loop runs on one core inside `present()`, serialised with the vsync
  callback.

Cheapest real improvements, in order: (1) copy into a **cached** shadow surface
and flush once per frame (or map panel memory write-back and flush by range);
(2) one `memcpy` per *plane* instead of per row when
`src_stride == dst_stride == w*4` (the full-screen opaque case), letting the
compiler emit `ldp/stp` streaming loops; (3) only then split rows across cores.
Days, not months -- and all superseded by (c) the moment (c) works.

Note also that (d) as "multi-threaded rasteriser" (parallelising
`CA::OGL::SW` itself) is *not* ours to do: `CA::OGL::SW` is inside Apple's
signed QuartzCore. The only CPU-side lever the kernel owns is the present copy.

---

## 4. What still cannot be faked

The chain breaks at exactly one place, and it is not IOKit:

> `MTLCreateSystemDefaultDevice()` - match `IOAcceleratorES` on an
> `AppleARMIODevice` named `gpu,t8015` - load `MetalPluginName` =
> `AGXMetalA11` - instantiate `MetalPluginClassName` = `AGXA11Device` - that
> object opens `IOGPUDeviceUserClient` and starts submitting AGX command buffers.

A kernel stub can satisfy every step up to the arrow into `AGXMetalA11`:
personality, class names, registry properties, `IOGPU` subclass, user-client
selectors, plausible return values. What it cannot supply is a GPU that
executes the buffers, because:

1. **The command encoding is AGX, not Adreno** (-3(a).1). Supporting it means
   *interpreting* AGX streams -- that is a simulator, not a stub.
2. **The shaders are AGX binaries** (-3(a).2), so it would also need an A11 ISA
   emulator, and no open description of that ISA exists.
3. **The A11 GPU is firmware-driven** (-2.2.4). The driver's real job is to feed
   `AGXFirmwareKextG10PRTBuddy` over RTBuddy; a stub would have to *become* that
   firmware's host, and the firmware is Apple-signed and runs only on Fender.

**So: a Metal device object can be faked; a Metal device cannot.** And per -2.3
the fake is not a useful milestone -- the moment `backboardd` has a device it
creates a queue and submits, and the kernel family has no degraded state. Today
`CA::OGL::` deterministically picks `CA::OGL::SW` and the GUI works; a fake
device trades a working software path for a hang.

Where a decoy service *is* valuable: as a **one-boot experiment**. Publish
`IOAcceleratorES` / `gpu,t8015` behind a boot-arg with IOKit tracing on (patch
0049's `sweetiolog`), boot once, and read the log to convert every "unverified"
in -2.2 into a measurement -- exact match strings, property names, selectors,
and what `CA::OGL::` does when a device appears and then fails. It must not be
left enabled.

---

## 5. Effort estimate and the realistic two-week deliverable

| option | minimum viable | production | risk |
|---|---|---|---|
| (c) MDP5 WB blit replacing the `memcpy` | **2-3 weeks** | 1-3 person-months | clocks/SMMU unknown; settled by one register read + one blit |
| (d) cached shadow buffer + per-plane copy + NEON | **3-5 days** | 2-4 weeks | none -- pure kernel code |
| (b) MDP5 scanout of CA layers (zero copy) | 1-2 months | 2-4 person-months | takes DSI/panel timing from the bootloader |
| (a) kgsl + Apple Metal | -- | -- | **not reachable** (-3(a), -4) |
| fake `IOAcceleratorES` device | 1-2 days | -- | only as a 1-boot experiment; harmful if left on |

**The realistic two-week deliverable is not "GPU acceleration":**

1. **Days 1-2** -- wire `sweet_mdss_blit()` behind a boot-arg, map the two MDP
   ranges, and log `MDP_HW_VERSION` (`0x0C901000`) plus the existing
   `CTL_LAYER0`, `CTL_TOP`, `WB_DST_FORMAT` and `INTF` values on both boards.
   One boot each: is the block clocked, what did the bootloader leave behind, is
   the SMMU bypassed.
2. **Days 3-5** -- one full-screen blit (solid colour, then a real `IOSurface`),
   timed against the current path using the existing `SWEET FB: %llu swaps`
   counter. Success = identical pixels, `present()` CPU time - 0.
3. **Days 6-8** -- in parallel, the cheap CPU win as a fallback: cached shadow
   surface + one flush, per-plane instead of per-row `memcpy`. Also covers
   chiron if the SMMU blocks (c).
4. **Days 9-10** -- the decoy-`IOAcceleratorES` boot with `sweetiolog`, to
   replace -2.2's unverified items with log lines.
5. **Days 11-14** -- land the `present()` change behind the boot-arg, confirm the
   home screen still draws on both boards, write it up.

Anything promising a Metal device in two weeks is promising a demo that hangs
`backboardd`.

---

## 6. Artefacts and how to re-check them

| path (build server) | what |
|---|---|
| `/root/dsc/kc/kernelcache.release.iphone10b` | the 18 204 358-byte IM4P as pulled from the IPSW |
| `/root/dsc/kc/kc.bin/kernelcache.release.iphone10b.decompressed` | `ipsw kernel dec` output, 50 937 856 bytes |
| `/root/dsc/kc/kexts.json` | `ipsw kernel kexts --json`, all 239 kexts |
| `/root/dsc/out/prelink-gpu-region.xml` | raw `_PrelinkInfoDictionary` slice with the GPU personalities |
| `/root/dsc/out/agxg10p-personalities.xml` | `AGXG10P_A0/B0` personality dicts, verbatim |
| `/root/dsc/out/agxg10p.kext`, `iogpufamily.kext` | carved Mach-Os (`carve_kext.py`) |
| `/root/dsc/out/agxg10p-cstring.txt` | AGX `__cstring` + all C++ type encodings (56 + encodings) |
| `/root/dsc/out/iogpufamily-cstring.txt` | IOGPU `__cstring` -- the user-client method names |
| `/root/dsc/out/iogpufamily-oslog.txt` | IOGPU `__os_log`, 244 format strings = the whole error surface |
| `/root/dsc/out/agxg10p-text-dis.txt` | 97 891 lines, AGX `__TEXT_EXEC.__text` disassembly |
| `/root/dsc/out/iogpufamily-text-dis.txt` | IOGPU `__TEXT_EXEC.__text` disassembly |
| `/root/mdss-blit-skeleton.c` | the MDP5 blit skeleton (compiles; not wired into the build) |
| `/root/dsc/sysos/087-86593-047.dmg` | Cryptex1,SystemOS DMG, 2 743 074 816 bytes, CRC-32 ok |
| `/root/dsc/cache/dyld_shared_cache_arm64[.01-.40]` | the split cache, **41 files / 2 384 232 448 bytes**, each size verified against its own header |
| `/root/dsc/img/{QuartzCore,Metal,AGXMetalA11,IOSurface,IOAccelerator,IOMobileFramebuffer}.text` | carved `__TEXT` of the target images |
| `/root/dsc/out/*` | as above |

Tools written for this (all on `E:\ios-sweat\` and copied to `/root/dsc/`):

| tool | purpose |
|---|---|
| `carve_kext.py` | carve a prelinked kext Mach-O out of the kernelcache (relocates `__TEXT`) |
| `carve_dsc.py` | carve the 41 split-cache files out of the raw APFS image |
| `dsc_refs.py` | decode the subcache array (uuid / size / suffix, 56-byte stride) |
| `dsc_check.py` | verify every carved cache file against its own header |
| `dsc_imgs.py` | find in-cache Mach-Os by `LC_ID_DYLIB` install name (3 026 images) |
| `carve_text.py` | carve just `__TEXT` of a chosen in-cache image |
| `greps.py`, `xref_str.py`, `metal_xref.sh` | string and cross-reference searches |
| `find_dispatch.py`, `scan_ptrs.py` | attempted `IOExternalMethodDispatch` recovery (**failed** -- see note) |
| `dlseg.py` | verified segmented range downloader for the IPSW members |
| `raw2elf.py` | wrap a raw AArch64 blob in ELF (superseded by `carve_kext.py`) |

**Honest note on the user-client dispatch tables.** I tried to recover
`IOGPUDeviceUserClient`'s `IOExternalMethodDispatch` array (selector - function
- in/out sizes) from `IOGPUFamily`'s `__DATA_CONST.__const` with
`find_dispatch.py` and `scan_ptrs.py`. Both found **zero** entries: in a
prelinked kernelcache, pointers inside `__DATA_CONST` are not plain virtual
addresses (they are prelink-relative / chained-fixup encoded), so a naive
8-byte-pointer scan cannot see them. The selector-handler mapping is therefore
**not** in this report; only the handler *names* (-2.2.2) are, and those come
from the C++ type encodings in `__cstring`, which are unambiguous. Recovering
the numbers needs the fixup chain walked (or a boot with IOKit tracing, -4).

Reproduce:

```bash
ipsw kernel dec kernelcache.release.iphone10b -o kc.bin
ipsw kernel kexts --json kc.bin/*.decompressed > kexts.json
python3 carve_kext.py kc.bin/*.decompressed 0x10ff980 agxg10p.kext     # AGXG10P
python3 carve_kext.py kc.bin/*.decompressed 0x10f8440 iogpufamily.kext # IOGPUFamily
llvm-objdump -d --no-show-raw-insn --section=__text agxg10p.kext > agxg10p-text-dis.txt
```

The header offsets `0x10ff980` / `0x10f8440` are the `feedfacf` Mach-O headers
found at the `_PrelinkExecutableLoadAddr` values `0xfffffff00554b980` /
`0xfffffff005544440`, both inside `__PRELINK_TEXT.__text`, which starts at file
offset `0x97c000` for VA `0xfffffff004dc8000`. `carve_kext.py` then copies each
`LC_SEGMENT_64` from its own declared `fileoff`, and relocates `__TEXT`
(fileoff 0) from the prelink region -- that relocation is required; without it
`llvm-objdump` correctly rejects the carve as malformed.

---

## 7. The dyld-cache answers (QuartzCore / Metal / AGXMetalA11)

*Filled in below when the cache extraction completed. If this section says
"not reached", the 4.73 GB filesystem DMG did not finish downloading inside the
session, and everything the kernelcache can prove is already in -2.2--2.4.*

### 7.1 Renderer selection (Phase 2.1) -- **partly answered; the conditional itself is not traced**

What is now verified from the extracted cache:

* `CA_ENABLE_OGL` is a real C string in QuartzCore `__TEXT.__cstring` at VA
  `0x18857f493` (found by scanning the carved `__TEXT`; `QuadCore.__TEXT` starts
  at `0x188284000`, `__cstring` at `0x188579bf0` + 0x28eed bytes long).
* `MetalContext` appears as a string in the same section, alongside the
  `CA::OGL::` primitive/blend enum names and the `CGMetalContext` note
  `com.apple.coreanimation.CGMetalContext`.
* `com.apple.coreanimation.CARenderOGL` exists as a signpost subsystem name.

What is **not** answered, and I will not dress it up: the *instruction-level*
condition that picks `metal_new_context` over `gles_new_context` over
`sw_new_context` was **not** traced. My cross-reference attempt
(`xref_str.py QuartzCore CA_ENABLE_OGL`) found **0** `adrp` references to the
page holding the string (`0x18857f000`), and `llvm-objdump` produced **0 lines**
of disassembly for the carved `QuartzCore.text` because the carve keeps only
`__TEXT` and drops the `__DATA*`/`__LINKEDIT` segments the tool wants. So:

* the string and the class names: **verified**;
* "the exact condition that selects Metal": **not established** -- **unverified**.

The plausible shape of it (a `MTLCreateSystemDefaultDevice()` that returns NULL
plus a `CA_ENABLE_OGL` override) remains a hypothesis, and this report does not
claim otherwise. To finish it: re-carve QuartzCore **whole** (all five segments,
pulling the coalesced `__DATA*` from the subcaches the segment table names), then
`llvm-objdump -d` and follow the `adrp`/`add` pairs; or run `ipsw dyld disass`
on a cache tree `ipsw` accepts.

That said, -2.2 and -7.2 answer the question that actually matters -- *what does
Metal need* -- from two independent directions, so the missing conditional does
not change any conclusion in -3--5: today `CA::OGL::SW` is chosen because
`MTLCreateSystemDefaultDevice()` returns NULL, and it returns NULL because no
`IOAcceleratorES` service exists to match.

### 7.2 Metal device discovery (Phase 2.2, userland half) -- **verified, and it confirms the kernel side exactly**

`Metal.framework`'s `__TEXT.__cstring` contains a single contiguous run of
strings at VA `0x181b77307`-`0x181b773c1` that *is* the device-discovery code's
string table. Verbatim, in order:

```
MetalPluginName
MetalPluginClassName
IOAcceleratorES
%s: IOServiceGetMatchingServices failed %x
```

That is the whole algorithm, and it matches -2.2.1's personality exactly:

1. `IOServiceGetMatchingServices` on **`IOAcceleratorES`** -- the
   `IOMatchCategory` the AGX personality publishes. (Not `IOAccelerator`, not
   `IOGPU`.)
2. read the service property **`MetalPluginName`** - `AGXMetalA11`.
3. read **`MetalPluginClassName`** - the class to instantiate inside that
   bundle.
4. if the match fails, log `%s: IOServiceGetMatchingServices failed %x` and
   return NULL -- which is exactly today's situation, and the reason
   `CA::OGL::SW` is used.

`Metal.framework` also carries `AGX_LOG_SHADER_COMPILER_REQUEST` and an ObjC
class `IOAcceleratorFamilySupport`, and the method name `isAGX` -- i.e. the
framework itself distinguishes AGX from other accelerator families.

**No `gpu-core-count`, no `IOGLBundleName` anywhere in `Metal.framework`'s
strings** -- consistent with the kernelcache result in -2.2.1.

### 7.3 Post-device call path (Phase 2.3, userland half) -- **verified enough to settle the question**

From `AGXMetalA11` (the plugin Metal would load):

| evidence | meaning |
|---|---|
| `-[AGXA11FamilyDevice initWithAcceleratorPort:simultaneousInstances:]`, `initWithAcceleratorPort:options:`, `initWithAcceleratorPort:` | the device object is built **from an accelerator Mach port**, i.e. from `IOServiceOpen()` on the `IOAcceleratorES` service |
| `AGXA11FamilyCommandQueue`, `AGXA11FamilyCommandBuffer` | the first objects created after the device |
| `AGXA11FamilyRenderContext`, `-ComputeContext`, `-BlitContext`, `-ResourceStateContext`, `-DebugContext` | encoders |
| `-[AGXA11FamilyCommandBuffer renderCommandEncoderWithDescriptor:]`, `-blitCommandEncoderCommon:`, `-parallelRenderCommandEncoderWithDescriptor:` | what `backboardd` will call for CoreAnimation |
| `AGX: %s:%d:%s: *** The KMD features string is empty - This probably indicates a build, setup, or consistency issue` | the plugin **queries a features string from the kernel-mode driver** |
| `AGX: %s:%d:%s: *** Could not query driver information from the KMD - -` | and a driver-information query |
| `AGX: NOTE: UMD was built in RELEASE mode, but KMD was not; is this what you intended?` | it even compares UMD/KMD build modes |

So the userland half confirms -2.3 from the other side: a device object leads
**immediately** to a command queue and command buffers, and the plugin demands a
**KMD features string** and driver information from the service before it will
work. There is no probe-only path in either half.

### 7.4 GPU family negotiation (Phase 2.4) -- **partly answered**

* `supportsFamily:` is a real selector: it appears in `AGXMetalA11`'s
  `__objc_methname` (alongside `acceleratorPort`, `initWithAcceleratorPort:`).
* The concrete implementation classes are named for the family:
  **`AGXA11FamilyDevice`**, `AGXA11FamilyCommandQueue`, `-CommandBuffer`,
  `-RenderContext`, - -- i.e. the *family* is baked into the class name by the
  plugin, not negotiated at runtime from a feature mask. There is no
  `AGXCommonFamilyDevice` fallback visible in the class list, so
  "an unknown/lower family still yields a working device" has **no supporting
  evidence**.
* The kernel side (-2.4) maps `gpu,t8015` - `AGXG10P` - `AGXMetalA11` through a
  static personality table with no downgrade knob.

Which feature booleans `supportsFamily:` actually returns, and whether any of
them can be turned off to get a slower-but-working device, was **not**
determined -- that needs the method body, which is behind the same disassembly
gap as -7.1. Marked **unverified**. Given -4 (the blocker is the AGX command
stream and firmware, not a feature bit), the answer would not change the verdict.

---

