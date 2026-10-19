#!/usr/bin/env python3
"""Per-device hardware description for the ios-sweet XNU port.

The port started on the Redmi Note 10 Pro (sweet, SM7150) and every board
constant lived in three places: the boot shim, tools/mkadt.py and the XNU
patch series.  Adding the Mi MIX 2 (chiron, MSM8998) needs the same
information for a second SoC, so it all lives here and the tools read it:

    python3 tools/devices.py                # list the devices
    python3 tools/devices.py chiron         # dump one device
    python3 tools/gen_devcfg.py chiron      # -> shim/devcfg.h

Where the facts come from (observations, not guesses, unless marked):

sweet  (Redmi Note 10 Pro, global/Indian, 6 GiB)
  * live FDT of the phone, dumped with tools/unpack_bootv2.py and read in
    NOTES.md "Hardware facts (live DT + shim measurements)"
  * measured on hardware by the shim's probe mode (GIC redistributor base,
    timer INTID 19, UFS host at 0x1d84000)

chiron (Xiaomi Mi MIX 2, MSM8998, 6 GiB)
  * Platforms/Msm8998Pkg/Device/xiaomi/chiron/DeviceTreeBlob/Android/
    Android-chiron.dtb from edk2-porting/edk2-msm8998 (the FDT that UEFI
    passes on real hardware), decompiled with dtc
  * arch/arm64/boot/dts/qcom/{msm8998.dtsi,xiaomi/chiron-msm8998-mtp.dtsi}
    from LineageOS/android_kernel_xiaomi_msm8998
  * EDK2 PCDs: PcdMipiFrameBufferAddress (0x9d400000, the surface EDK2's
    SimpleFbDxe drives) and chiron.dsc PcdMipiFrameBufferWidth/Height
    (1080x2160).  The Linux splash region is cont_splash_mem@9d600000; both
    addresses are carried, the UEFI loader takes the real one from GOP.
"""

# --- helpers ---------------------------------------------------------------

def _cpus_uniform(count, clock, first_mpidr_shift=8, mem_freq=1866000000):
    """sweet: core N has MPIDR Aff1=N (all cores in one cluster)."""
    return [dict(mpidr=n << first_mpidr_shift, core=n, cluster=0, clock=clock)
            for n in range(count)]


DEVICES = {
    # ---------------------------------------------------------------- sweet
    "sweet": dict(
        name="sweet",
        product="Redmi Note 10 Pro",
        vendor="Xiaomi",
        soc="sm7150",
        soc_name="Snapdragon 732G",
        # ADT root compatible list.  The first two are the iPhone identity
        # the userland comes from, then the board strings the kernel and the
        # device tree matching use.
        identity=("D22AP", "iPhone10,3"),
        board_compat=("xiaomi,sweet", "qcom,sm7150"),
        # Board-independent match strings the kernel personalities use
        # (xnu patch 0082): one kernel binary serves every device.
        platform_match="ios-sweet",
        # AppleCLCD matches any of these; the board-specific string keeps a
        # kernel built before patch 0082 working with a new ADT.
        display_match=("ios-sweet,display", "sweet,display"),
        cpu_compatible="qcom,kryo470",
        # RAM windows for the kernel.  low: above every Qualcomm carve-out
        # (see NOTES.md); high: the whole upper DDR bank (--highmem).
        ram_low=(0xb0000000, 0x4d000000),
        ram_high=(0x100000000, 0xfcc00000),
        gic=dict(dist=0x17a00000, dist_size=0x10000,
                 redist=0x17a60000, redist_size=0x100000, redist_stride=0x20000),
        timebase=19200000,
        vtimer_intid=19,      # measured: Qualcomm routes it off the usual 27
        ptimer_intid=18,
        ufs=(0x01d84000, 0x3000),
        psci_method="smc",
        # reboot path used by the shim's probe mode: PSHOLD register and the
        # IMEM restart-reason cell (live DT: restart@10ac000 / imem@146bf000)
        pshold=0x0c264000,
        imem_restart=0x146aa65c,
        # frame buffer ABL leaves the panel running on
        fb=dict(base=0x9c000000, size=0x1700000, width=1080, height=2400,
                stride=1080 * 4, splash="splash_region"),
        # logical display presented to the iOS userland (iPhone X panel)
        display=dict(logical_w=1125, logical_h=2436, dpi=458, scale=3, anchor="center"),
        cpus=_cpus_uniform(8, 1804800000),
        cpu_clock_big=2304000000,     # cores 6-7 are Cortex-A76
        cpu_big_first=6,
        mem_frequency=1866000000,
        ecid=0x5357454554000001,      # 'SWEET' + 1, synthetic
        iphone_chip_id=0x8015,        # A11, what the iPhone X userland wants
        # touch: Goodix GT9896 on QUP SE7
        touch=dict(controller="goodix-gt9896", i2c_base=0xa84000, addr=0x5d),
        keys="pmic-spmi",             # PM6150 PON + PM6150L GPIO2
        # default boot-args (README "Build the boot image")
        cmdline=("debug=0xc serial=3 cpus=6 keepsyms=1 -enable_kprintf_spam "
                 "-noprogress rd=ufsroot0 unentitled_ios_sim_launch=1 "
                 "ignition_level=1 wdt_test=no_user_checkin ufsrw=1 rootrw=1 "
                 "launchd_unsecure_cache=1 wdt=-1 sweetfb_noconsole=1 sweetfb_hz=30"),
        notes="working port; boots the iOS 16 home screen",
    ),
    # --------------------------------------------------------------- chiron
    "chiron": dict(
        name="chiron",
        product="Mi MIX 2",
        vendor="Xiaomi",
        soc="msm8998",
        soc_name="Snapdragon 835",
        identity=("D22AP", "iPhone10,3"),
        board_compat=("xiaomi,chiron", "qcom,msm8998"),
        platform_match="ios-sweet",
        display_match=("ios-sweet,display", "chiron,display"),
        cpu_compatible="qcom,kryo280",
        # Live DT: memory@0 = <0x0 0x40000000 0x0 0xc0000000>, memory@1 =
        # <0x1 0x0 0x0 0xbd8c0000> (6 GiB variant).  The low bank holds every
        # PIL/splash carve-out up to 0x9fa00000, so the kernel window starts
        # at 0xa0000000; the upper bank has no carve-outs at all.
        ram_low=(0xa0000000, 0x60000000),
        ram_high=(0x100000000, 0xbd8c0000),
        # Live DT: interrupt-controller@17a00000 (dist) + redist@17b00000,
        # 8 redistributors with a 0x20000 stride.  sweet's GICR is 0x17a60000,
        # so this is a real per-SoC difference.
        gic=dict(dist=0x17a00000, dist_size=0x10000,
                 redist=0x17b00000, redist_size=0x100000, redist_stride=0x20000),
        timebase=19200000,
        # Live DT timer node: <1 3> virtual = PPI 11 = INTID 27 (the usual
        # ARM numbering; on sweet it is 19).
        vtimer_intid=27,
        ptimer_intid=30,
        ufs=(0x01da4000, 0x2500),     # live DT: ufshc@1da4000, reg size 0x2500
        psci_method="smc",
        # live DT: restart@10ac000 (qcom,pshold) and
        # imem@146bf000 { restart_reason@65c }
        pshold=0x010ac000,
        imem_restart=0x146bf65c,
        # Framebuffer: the edk2 chiron DTB (the FDT that UEFI passes on real
        # hardware) has splash_region@0x9d400000 + 36 MiB, and edk2's
        # SimpleFbDxe drives exactly that (PcdMipiFrameBufferAddress). Linux's
        # cont_splash_mem is 2 MiB later at 0x9d600000; the shim prefers
        # /chosen/ios-sweet,fb-* overrides if a bootloader disagrees.
        # 1080x2160 from chiron.dsc and the panel node (0x438 x 0x870).
        fb=dict(base=0x9d400000, base_linux=0x9d600000, size=0x2400000,
                width=1080, height=2160, stride=1080 * 4,
                splash="splash_region"),
        # Same logical iPhone X screen, but the panel is 276 px shorter, so
        # the present path anchors it at the top and crops the bottom (see
        # xnu patch 0082).  sweetfb_native=1 uses the 1080x2160 panel size.
        display=dict(logical_w=1125, logical_h=2436, dpi=458, scale=3,
                     anchor="top"),
        # 4x Kryo 280 Gold (Cortex-A73 class, cluster 0, MPIDR 0x000-0x003)
        # + 4x Kryo 280 Silver (Cortex-A53, cluster 1, MPIDR 0x100-0x103).
        cpus=[dict(mpidr=0x000 + n, core=n, cluster=0, clock=2457600000) for n in range(4)] +
             [dict(mpidr=0x100 + n, core=4 + n, cluster=1, clock=1900800000) for n in range(4)],
        cpu_clock_big=2457600000,
        cpu_big_first=0,      # the big cluster comes first in the live DT
        mem_frequency=1866000000,
        ecid=0x434849524f4e0001,      # 'CHIRON' + 1, synthetic
        iphone_chip_id=0x8015,        # same iPhone X userland as sweet
        # touch: Synaptics DSX (S3330 / TD4322) on BLSP i2c, IRQ GPIO 125,
        # reset GPIO 89.  No driver yet -> the kernel leaves touch disabled.
        touch=dict(controller="synaptics-dsx", i2c_base=0xc179000, addr=0x20,
                   irq_gpio=125, reset_gpio=89),
        keys="pm8998-pon",            # volume/power on the PM8998 PMIC
        # cpus=4 selects the fast cluster only (DT order); the little cores
        # come next in the tree, so cpus=8 enables them too.
        cmdline=("debug=0xc serial=3 cpus=4 keepsyms=1 -enable_kprintf_spam "
                 "-noprogress rd=ufsroot0 unentitled_ios_sim_launch=1 "
                 "ignition_level=1 wdt_test=no_user_checkin ufsrw=1 rootrw=1 "
                 "launchd_unsecure_cache=1 wdt=-1 sweetfb_noconsole=1 sweetfb_hz=30"),
        notes="bring-up: same iPhone X userland, MSM8998, no touch driver yet",
    ),
}


def names():
    return sorted(DEVICES)


def get(name):
    try:
        return DEVICES[name]
    except KeyError:
        raise SystemExit("unknown device %r (have: %s)" % (name, ", ".join(names())))


def compatible(dev, with_board=True):
    """ADT root compatible list.

    Order matters: XNU takes hw.target from the first entry and the iPhone
    userland (MobileGestalt) expects the real device identity there, so the
    D22AP identity stays first.  The board strings after it are what the
    platform expert and the display driver match; 'ios-sweet' is the
    board-independent one that lets a single kernel binary serve every phone.
    """
    out = list(dev["identity"])
    if with_board:
        out += list(dev["board_compat"])
    out.append(dev["platform_match"])
    out.append("AppleARM")
    return out


def cpu_list(dev):
    """[(mpidr, cluster, clock, is_big), ...] in device-tree (boot) order."""
    out = []
    for c in dev["cpus"]:
        big = (c["core"] >= dev["cpu_big_first"])
        clock = dev["cpu_clock_big"] if big else c["clock"]
        out.append((c["mpidr"], c["cluster"], clock, big))
    return out


if __name__ == "__main__":
    import json
    import sys
    if len(sys.argv) < 2:
        for n in names():
            d = DEVICES[n]
            print("%-8s %-20s %-10s %s" % (n, d["product"], d["soc"], d["notes"]))
    else:
        print(json.dumps(get(sys.argv[1]), indent=2, default=str))
