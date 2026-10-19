# ios-sweet

**Apple's iOS 16 running on a Redmi Note 10 Pro — and, in bring-up, a Xiaomi Mi MIX 2.**

This project ports Apple's open-source XNU kernel (xnu-8796, the iOS 16 generation) to the Xiaomi Redmi Note 10 Pro (`sweet`, Qualcomm SM7150 / Snapdragon 732G). On top of it runs the **unmodified iOS 16.7.16 (20H392) userland of the iPhone X**: launchd, the system daemons, backboardd, SpringBoard and the stock apps.

As of **2026-10-18** the phone boots to the **iOS home screen**. You can unlock it, open Settings, pull down Control Center and the notification center, and use the touchscreen and the hardware buttons. Rendering runs entirely on the CPU; there is no GPU driver.

Since **2026-10-19** the port is board-agnostic (patch `0082`): every SoC and panel constant lives in one table, `tools/devices.py`, the boot shim and the kernel read their hardware from the generated device tree, and the same kernel binary is meant to serve both boards. `chiron` (Mi MIX 2, MSM8998 / Snapdragon 835) is wired up as the second device but has **not been booted on hardware yet**; see [Supported devices](#supported-devices).

> Experimental bring-up, not a usable phone OS. Many things are missing or stubbed (see below), and Apple's security model is mostly switched off. It installs into the `userdata` partition and so **erases Android's user data**.

## Supported devices

| Device | `--device` | SoC | Panel | State |
| --- | --- | --- | --- | --- |
| Xiaomi Redmi Note 10 Pro | `sweet` | SM7150, 8 cores (6× A55 + 2× A76) | 1080×2400 | **Working**: boots to the iOS home screen, touch, buttons, display |
| Xiaomi Mi MIX 2 | `chiron` | MSM8998, 8 cores (4× Kryo 280 Gold + 4× Silver) | 1080×2160 | **Bring-up**: shim, device tree, kernel config and build path ready; not yet booted on hardware |

Both run the same iPhone X userland, because both SoCs are plain ARMv8.2 without pointer authentication (the A11 in the iPhone X has none either).

Adding a device means adding an entry to `tools/devices.py` (addresses, GIC layout, timer INTID, RAM windows, panel, cores). Everything else — the shim's `devcfg.h`, the ADT from `tools/mkadt.py`, the kernel's display geometry and the SoC checks in the drivers — derives from it.

## What works

| Area | State |
| --- | --- |
| Boot | Stock Xiaomi bootloader → `fastboot boot` of our shim → XNU. Nothing is flashed to the boot partitions |
| CPU | 6 of the 8 cores (the six Cortex-A55s) via PSCI, 16K pages, GICv3 interrupts, timers |
| Memory | About 3.95 GiB of the upper DDR bank |
| Storage | Own UFS host driver; the iOS root is a journaled HFS+ volume in `userdata`, read/write |
| iOS userland | Real iPhone X 16.7.16 root filesystem and shared cache; launchd and the system daemons run |
| Display | Kernel IOSurface + IOMobileFramebuffer (`AppleCLCD`) services; CoreAnimation renders in software; frames are copied into the bootloader's framebuffer. Presented to iOS as an iPhone X panel (1125×2436) |
| GUI | backboardd, SpringBoard, lock screen, home screen, Control Center, notification center, Settings |
| Touch | Goodix GT9896 over I²C (GENI), rail enabled through RPMh, delivered as HID digitizer events |
| Buttons | Power, volume up/down (PMIC through SPMI) as HID consumer keys; lock/wake works |
| Battery | IOPMPowerSource service (static 100 %, charging icon; real fuel-gauge readout prepared) |
| Device identity | iPhone X (`iPhone10,3` / `D22AP`): no Home button, gesture UI |
| Activation | Native "Hactivation" path of mobileactivationd, chosen through device-tree security status. No Apple servers involved |

## What does not work (yet)

- **Navigation:** you cannot leave an app (the home gesture and AssistiveTouch Home do nothing yet).
- **No Wi-Fi, Bluetooth, cellular, audio, camera, sensors, GPS or SEP / Face ID.** Daemons for them start and fail or keep relaunching.
- **No GPU.** Everything is drawn by the CPU at roughly 20–30 fps. The two Cortex-A76 cores are not used yet.
- **Few apps.** The image has only what is on the system volume. There is no App Store, no Apple ID, and some Settings pages are empty.
- **Slow boot:** about 4 minutes on the Apple logo.
- **Security is off:** permissive code signing (AMFI) and sandbox policies, a stub AppleKeyStore, no data protection. Do not put anything personal on it.

## How it boots

```
ABL (stock Xiaomi bootloader, unlocked)
  -> fastboot boot shim-boot.img              (temporary; nothing is flashed to boot)
     -> shim: own MMU setup, reads the live FDT, loads the payload from the ramdisk
        -> XNU Mach-O + generated Apple Device Tree (ADT) + boot-args
           -> XNU -> UFS driver -> HFS+ root in the userdata partition
              -> dyld + shared cache -> launchd -> daemons -> backboardd + SpringBoard
```

There is no serial port (the debug UART is wired as SPI on these boards). The kernel draws its console into the framebuffer that the bootloader leaves running, and keeps a persistent 4 MiB log ring at the end of `userdata`. You read that log from recovery with `tools/sweet-log-pull.sh`, so you don't have to film the screen.

The pieces that are board-specific are read from the device tree the shim passes on, not compiled in: `/chosen` (RAM window, identity, security status), `/cpus` (MPIDRs and clocks), `/arm-io/gic` (GICD/GICR addresses and the virtual timer INTID), `/arm-io/disp0` (panel size, presented canvas, DPI) and `/sweet-ufs` (UFS host registers). See `pexpert/pexpert/SweetBoard.h` in the kernel and `tools/devices.py` for where every number comes from.

## How to do it yourself

You need a supported phone (a Redmi Note 10 Pro `sweet`, global or Indian variant, SM7150, or a Mi MIX 2 `chiron`, MSM8998) with an **unlocked bootloader**, a Linux PC, and a GitHub account for the kernel build. The sweet steps below are the exact path used for the screenshots above; other variants or firmware may need changes, and the chiron path is written down but not yet verified on hardware. **Everything on `userdata` is lost.** Bootloaders, recovery, modem and persist partitions are never touched.

### 0. Back up the phone

Boot OrangeFox (or any recovery with adb root) temporarily and pull at least `persist`, `modemst1`, `modemst2`, `fsg`, `fsc` and `boot`, `dtbo`, `vbmeta`. They contain per-device calibration data. Keep them off git; `.gitignore` already excludes `backup/`, `dtb/` and `recovery/`.

The shim needs a device tree to read `/chosen/linux,initrd-start` from. For sweet, unpack the stock `boot.img`:

```sh
python3 tools/unpack_bootv2.py backup/boot.img dtb/boot_unpacked   # -> dtb/boot_unpacked/dtb
```

For chiron, use the one [edk2-porting/edk2-msm8998](https://github.com/edk2-porting/edk2-msm8998) ships (it is the FDT that UEFI passes on real hardware):

```sh
git clone --depth 1 https://github.com/edk2-porting/edk2-msm8998
export DTB=$PWD/edk2-msm8998/Platforms/Msm8998Pkg/Device/xiaomi/chiron/DeviceTreeBlob/Android/Android-chiron.dtb
```

### 1. Build the kernel

The kernel builds on GitHub's macOS runners with [blacktop/darwin-xnu-build](https://github.com/blacktop/darwin-xnu-build). The workflow fetches xnu-8796, applies `xnu/patches/` in order, and also stages the pinned open-source libpthread, HFS and BearSSL sources it needs. Fork the repo (public repos get free macOS minutes) and run:

```sh
gh workflow run xnu.yml -R YOU/ios-sweet
gh run download RUN_ID -R YOU/ios-sweet -n xnu-kernel -D xnu/out
```

The same kernel (`kernel.development.sweet`) serves every device: the board differences come from the device tree, not from the build.

To check a patch locally before spending a CI run, `tools/xnu-cc.py` replays the compile commands of a previous CI log for single files. `tools/test-board-header.py` compiles and *runs* the device-tree helpers of `xnu/SweetBoard.h` against real generated ADTs, which catches display-geometry mistakes without a kernel build.

### 2. Build the iOS root image

Download the iPhone X (iPhone10,3) iOS 16.7.16 (20H392) IPSW from Apple — `tools/ipsw.py` does that and checks the checksum:

```sh
tools/ipsw.py info                  # release, size, SHA-1/SHA-256, URL (ipsw.me API)
tools/ipsw.py fetch                 # ~5.7 GiB into research/ipsw-x, resume + verify
tools/ipsw.py extract --list        # what is inside
```

Then, on Linux, with `apfs-fuse`, `hfsprogs` and sudo:

```sh
# OS, SystemOS and AppOS disk images from the IPSW (see BuildManifest.plist for their names)
tools/mkiosroot.sh OS.dmg SystemOS.dmg AppOS.dmg ios16-root.img 16G
sudo fsck.hfsplus -fy ios16-root.img
sudo mount -t hfsplus -o loop,force ios16-root.img /mnt/iosroot
sudo python3 tools/sweet-root-tweaks.py /mnt/iosroot
sudo umount /mnt/iosroot
img2simg ios16-root.img ios16-root.simg
```

`mkiosroot.sh` copies the OS volume and both cryptexes into one case-sensitive journaled HFS+ volume. It adds the dyld cache links, the plain system keybag and the log directories the daemons expect. `sweet-root-tweaks.py` applies the bring-up changes: it skips Setup, seeds the activation state, removes the baseband daemon, disables the watchdogs that kill slow apps and the crash-looping media paths, and so on. The script's header explains each change.

The device tree builder copies `/product` from the real iPhone X device tree, which MobileGestalt needs. Extract it once, without downloading the whole IPSW:

```sh
tools/ipsw.py extract DeviceTree.d22ap               # -> research/ipsw-x/Firmware/all_flash
python3 tools/adt.py unwrap research/ipsw-x/Firmware/all_flash/DeviceTree.d22ap.im4p \
    research/ipsw-x/d22ap.adt                       # IM4P -> raw ADT (needs lzfse)
```

`tools/ipsw.py extract` reads only the bytes it needs with HTTP range requests, so a 34 KiB file does not cost 5.7 GiB of download.

### 3. Build the boot image

Needs `aarch64-linux-gnu-gcc` and `mkbootimg`. Defaults are `sweet`; add `--device chiron` for the Mi MIX 2 (the ADT, `devcfg.h` and the UFS/GIC/timer addresses follow the device).

```sh
python3 tools/mkadt.py shim/out/sweet.adt --highmem                  # or --device chiron
python3 tools/mkpayload.py xnu/out/kernel.development.sweet shim/out/sweet.adt shim/out/payload.bin \
    --cmdline 'debug=0xc serial=3 cpus=6 keepsyms=1 -enable_kprintf_spam -noprogress rd=ufsroot0 unentitled_ios_sim_launch=1 ignition_level=1 wdt_test=no_user_checkin ufsrw=1 rootrw=1 launchd_unsecure_cache=1 wdt=-1 sweetfb_noconsole=1 sweetfb_hz=30'
shim/build.sh ../shim/out/payload.bin      # -> shim/out/shim-boot-sweet.img
```

Per device:

| | sweet | chiron |
| --- | --- | --- |
| ADT | `tools/mkadt.py OUT --device sweet --highmem` | `tools/mkadt.py OUT --device chiron` (upper DDR bank by default) |
| `cpus=` | `6` (the A55s; `8` after patch 0078) | `4` (the four Gold cores; `8` adds the Silver cluster) |
| shim DTB | `dtb/boot_unpacked/dtb` from your own `boot.img` | `DTB=.../Android-chiron.dtb` from [edk2-msm8998](https://github.com/edk2-porting/edk2-msm8998) |
| shim build | `shim/build.sh PAYLOAD` | `DTB=... shim/build.sh --device chiron PAYLOAD` |
| boot | `fastboot boot shim/out/shim-boot-sweet.img` | UEFI/`fastboot boot` of `shim-boot-chiron.img` (not tested on hardware yet) |

`shim/build.sh` regenerates `shim/devcfg.h` from `tools/devices.py`, so the shim never has two copies of an address. Debian's `mkbootimg` package is broken (it wants the AOSP `gki` module); point `MKBOOTIMG=` at any working copy, e.g. the plain `mkbootimg.py` in `edk2-msm8998`.

What the less obvious boot-args do:

| Boot-arg | Effect |
| --- | --- |
| `rd=ufsroot0 ufsrw=1 rootrw=1` | Root on the `userdata` partition, writable |
| `cpus=6` | Start the six A55 cores; `cpus=1` is the safe fallback |
| `launchd_unsecure_cache=1` | launchd accepts its job cache without AMFI |
| `wdt=-1 wdt_test=no_user_checkin` | No hardware watchdog |
| `sweetfb_noconsole=1` | Stop drawing kernel text over the GUI. It still goes to the persistent log; panics are drawn |
| `sweetfb_hz=30` | Panel refresh rate of the CPU copy |
| `sweettouch=0`, `sweetkeys=0` | Disable the touch or button driver. On a board that is not SM7150 those drivers switch themselves off; `=1` forces them on |

### 4. Flash the root and boot

```sh
fastboot -S 512M flash userdata ios16-root.simg   # ~7 minutes; do not unplug
# Xiaomi's fastboot often hangs after a large flash: re-enter fastboot with Power + Volume down
fastboot boot shim/out/shim-boot.img
```

The first boot takes about 4–5 minutes on the Apple logo, then the lock screen appears. Swipe up to unlock. To get back to fastboot, hold Power + Volume down.

### 5. Read the kernel log

```sh
fastboot boot recovery/recovery.img        # OrangeFox, temporary
tools/sweet-log-pull.sh logs/boot.txt      # 4 MiB console ring from the end of userdata
adb reboot bootloader
```

The log contains crash reports with backtraces (`CRASH name ...`), IOKit traces of backboardd and SpringBoard, and the drivers' `SWEET ...` lines. `tools/sweet-crash.py` symbolicates them against the shared cache.

### Bringing up chiron (Mi MIX 2)

What is already done and checked (no hardware access needed):

- `tools/devices.py` entry for chiron, with the live-DT references for every number.
- `shim/build.sh --device chiron` builds `shim/out/shim-boot-chiron.img` with the chiron `devcfg.h` and the edk2 `Android-chiron.dtb` embedded.
- `tools/mkadt.py --device chiron` emits a 6 GiB upper-bank ADT with the chiron GIC redistributor (`0x17b00000`), timer INTID 27, UFS at `0x1da4000` and a 1080×2160 panel presented as the iPhone X screen.
- Patch 0082 makes the kernel read those values from the ADT instead of assuming sweet, and keeps the SM7150-only drivers (GENI touch, PM6150 keys, the DWC3 wrapper at `0x0a600000`) off on MSM8998.

What is still missing, in the order it will be needed on hardware:

1. **Boot path.** MSM8998 boots through [edk2-msm8998](https://github.com/edk2-porting/edk2-msm8998) (UEFI), not through a fastboot `boot` image with a stock ABL. Either load the shim from UEFI as an EFI application (`shim/` linked at a UEFI-safe address, or a small EFI wrapper that reads the payload and jumps), or use the `fastboot boot` path with the UEFI-provided DTB — the shim itself only needs a DTB in `x0` and the payload in the ramdisk.
2. **Framebuffer address.** The shim defaults to the address the SoC's bootloader leaves the splash at — for chiron `0x9d400000`, which is also what EDK2's `SimpleFbDxe` drives (`PcdMipiFrameBufferAddress`) and what the edk2 chiron DTB calls `splash_region` (Linux's `cont_splash_mem` is 2 MiB later, `0x9d600000`). A UEFI wrapper that knows the real GOP surface can override it at runtime with `/chosen/ios-sweet,fb-base`, `-width`, `-height`, `-stride`; the shim logs what it picked (`FB base/w/h`).
3. **Console.** `ADT /arm-io/wdt` has `no-pmu`, so watchdogd stays happy. The kernel console draws to the panel memory the shim reports, which is how panics will be read.
4. **Storage.** The UFS host is at `0x1da4000` (`qcom,ufshc`, same UFSHCI register layout), hardwired to the UFSHCI register block the bootloader leaves running. What is missing is the MSM8998 GCC/PHY state: if the host is not left enabled by UEFI, the `sweet_ufs.c` driver needs the MSM8998 clock and PHY registers.
5. **Touch and buttons.** No driver yet: the Mi MIX 2 uses a Synaptics DSX (I²C `0x20`, IRQ GPIO 125, reset GPIO 89) and the PM8998 PON for the power/volume keys. The existing drivers are SM7150-specific and switch themselves off on chiron, so the phone will boot without input.

### QEMU

`tools/qemu-sweet.py` boots the same kernel on QEMU `virt` with a test RAM-disk root (`--device chiron` decodes that device's panel instead of sweet's). It is useful for kernel-only changes; see `NOTES.md`.

## Repository layout

| Path | What |
| --- | --- |
| `shim/` | Bare-metal boot shim (C + asm) packed as an Android boot.img v2; `devcfg.h` is generated per device |
| `xnu/patches/` | The XNU patch series (applied in order by CI), plus staging scripts for libpthread, HFS and BearSSL |
| `xnu/SweetBoard.h` | Kernel-side device-tree helpers (board identity, panel/presented geometry); installed by patch 0082 |
| `.github/workflows/xnu.yml` | Kernel CI build |
| `tools/devices.py` | **The device table**: every per-SoC address, IRQ, RAM window, panel and core, with the source of each fact |
| `tools/gen_devcfg.py` | `devices.py` -> `shim/devcfg.h` |
| `tools/mkadt.py` | Generates the Apple Device Tree from the device table and the iPhone X identity |
| `tools/ipsw.py` | ipsw.me queries, IPSW download with SHA-1/SHA-256 check, single-member extraction |
| `tools/test-board-header.py` | Runs the device-tree helpers from `xnu/SweetBoard.h` against generated ADTs (host test, no kernel build) |
| `tools/mkpayload.py` | Packs kernel + ADT + boot-args into the shim payload |
| `tools/mkiosroot.sh` | Builds the HFS+ iOS root from the IPSW images |
| `tools/sweet-root-tweaks.py` | Bring-up changes to the root (Setup skip, activation, daemons) |
| `tools/sweet-log-pull.sh`, `sweet-log.py` | Read the persistent kernel log from recovery |
| `tools/sweet-crash.py` | Symbolicate console crash reports |
| `tools/xnu-cc.py` | Local single-file compile check from a CI log |
| `tools/qemu-sweet.py` | QEMU test boots with framebuffer text decoding |
| `tools/adt.py`, `remotezip.py`, `unpack_bootv2.py`, `sparsediff.py` | Helpers |
| `NOTES.md` | Detailed engineering log: every boot, crash and decision, plus hardware facts |

Not in git: partition backups, live device trees, recovery images, Apple firmware and root images, and downloaded or generated build trees. They contain device serials and calibration data, or Apple's copyrighted files.

## Kernel patches

| # | What |
| --- | --- |
| 0001–0009 | Toolchain compatibility, SWEET machine config, non-PAC arm64, stubs for Apple-internal pieces |
| 0010–0018 | Early framebuffer console, physmap fix, PRNG without corecrypto, device-tree platform expert, timer PPI, boot CPU driver |
| 0019–0020 | Apple open-source libpthread kernel provider; BearSSL AES |
| 0021–0027 | RAM-disk root, mockfs, SHA-256, minimal AMFI, Apple open-source HFS+, MD5 |
| 0028–0034 | UFS host driver and root device, read/write root, DMA above 4 GiB |
| 0035–0045 | Entitlements, bindfs, AppleKeyStore stub, IOWatchdog, console crash reports |
| 0046 | IOSurface (IOCoreSurfaceRoot) and IOMobileFramebuffer (AppleCLCD) services |
| 0047–0055 | Diagnostics: persistent log, /dev/null tee, IOKit trace, heap strings and user stack sampler on crash; permissive Sandbox policy; HFS rename-swap; AppleKeyStore selectors |
| 0056–0057 | Throttled panel copy; no external identity resolver |
| 0058 | HID event services. Apple's IOHIDFamily-1915.140.3 user client and event queue, matching the iOS 16.7 userland |
| 0059 | iPhone X identity (`hw.product` / `hw.target` from the device tree) |
| 0060 | Power and volume keys over SPMI; touch over GENI I²C |
| 0061 | Present the display as 1125×2436 |
| 0062 | RPMh regulator vote for the touch rail; battery power source |
| 0063 | GENI FIFO packing fix and the Goodix GT9896 protocol |
| 0064 | Grant PRIV_WORK_INTERVAL (UIKit's CoreAnimation work interval) |
| 0065 | SMP: secondary A55 cores via PSCI CPU_ON |
| 0066–0067 | Fault-region crash info; console mute |
| 0068 | IOSurface lookup semantics for already-mapped surfaces |
| 0069 | Batched persistent-log flushes |
| 0070–0081 | Hibernation PID, touch edges, DWC3 USB device + ECM networking, 8-core SMP, display power, network tracing |
| 0082 | Board-agnostic display and device detection: the presented screen size, the panel crop and the DPI come from `/arm-io/disp0`; the SM7150-only drivers (touch, keys, USB) switch off on other SoCs (`pexpert/SweetBoard.h`) |

## Key hardware facts

sweet (Redmi Note 10 Pro, SM7150):

- RAM: payloads use the upper bank, `0x100000000` + `0xfcc00000` (about 3.95 GiB), above Qualcomm's protected carve-outs.
- GICv3 with security extensions: the timer goes through Group 1 IRQ (virtual timer INTID 19), not FIQ as on Apple silicon.
- PSCI through `smc`, 19.2 MHz timer, ARMv8.2 without pointer authentication. MPIDR uses Aff1 for the core number.
- Panel: 1080×2400 DSC video mode, continuous splash from ABL, no refresh kick needed.
- Touch: Goodix GT9896 at I²C `0x5d` on QUP SE7 (`0xa84000`), powered by PM6150L LDO11 through RPMh.
- Buttons: power and volume down on the PM6150 PON block, volume up on PM6150L GPIO2 (SPMI SID 4).

chiron (Mi MIX 2, MSM8998) — from the device tree that edk2-porting/edk2-msm8998 ships (`Android-chiron.dtb`) and LineageOS's `android_kernel_xiaomi_msm8998`:

- RAM: `memory@0` = `0x40000000` + 3 GiB, `memory@1` = `0x100000000` + `0xbd8c0000` (6 GiB variant). The low bank is full of PIL and splash carve-outs up to `0x9fa00000`, so the kernel window starts at `0xa0000000`; the upper bank has none.
- GICv3: distributor at `0x17a00000`, redistributors at `0x17b00000` (sweet: `0x17a60000`), 8 × 0x20000.
- Timer: the usual ARM PPI numbering, virtual timer INTID 27 (sweet: 19).
- Cores: 4× Kryo 280 Gold (MPIDR `0x000`–`0x003`) + 4× Silver (`0x100`–`0x103`); the big cluster comes first in the device tree, which is why `cpus=4` selects it.
- UFS host at `0x1da4000` (sweet: `0x1d84000`), `qcom,pshold` at `0x10ac000`, IMEM restart reason at `0x146bf65c`.
- Panel: 1080×2160 (panel node `0x438`×`0x870`); the edk2 chiron DTB's `splash_region` and EDK2's `SimpleFbDxe` both use `0x9d400000` (Linux's `cont_splash_mem` is `0x9d600000`). Presenting the iPhone X screen (1125×2436) on it crops 22 px left/right and the bottom 276 rows, top-aligned so the status bar stays visible.
- Touch: Synaptics DSX (S3330 / TD4322) on i2c `@0x20`, IRQ GPIO 125, reset GPIO 89 — **no driver yet**, so touch stays off.

More in [`NOTES.md`](NOTES.md).

## Credits

Built on Apple's open-source releases (xnu, IOHIDFamily, libpthread, hfs), [BearSSL](https://bearssl.org), [blacktop/darwin-xnu-build](https://github.com/blacktop/darwin-xnu-build), [ipsw](https://github.com/blacktop/ipsw), [edk2-porting/edk2-msm8998](https://github.com/edk2-porting/edk2-msm8998) (UEFI for MSM8998 and its chiron device tree), LineageOS's `android_kernel_xiaomi_msm8998` for the MSM8998/chiron hardware facts, Qualcomm/Xiaomi's published Linux kernel sources for the hardware details, and [OrangeFox Recovery](https://orangefox.download). iOS itself is Apple's and is not distributed here.
