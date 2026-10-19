#!/usr/bin/env python3
"""Build the minimal Apple Device Tree that the SWEET xnu needs to boot.

Only what the early boot path actually reads (see NOTES.md); every MMIO
address comes from the device table in tools/devices.py, which documents
where each number was measured. Usage:

    mkadt.py OUT.adt [--device chiron] [--dump] [--qemu] [--highmem|--lowmem]
"""
import hashlib, os, struct, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import adt        # noqa: E402
import devices    # noqa: E402

# /arm-io children are relative to this; it must not be 0 (serial_init bails)
SOC_BASE = 0x01000000

# QEMU virt (-M virt,gic-version=3 -cpu cortex-a76): GICD/GICR and the generic
# timer's CNTFRQ.  QEMU is a kernel test rig, not a device, so these stay here.
QEMU_GICD, QEMU_GICR, QEMU_TIMEBASE = 0x08000000, 0x080a0000, 62500000
QEMU_VTIMER_INTID = 27

BOOT_MANIFEST_HASH = hashlib.sha384(b'sweet boot manifest').digest()

# The iOS userland is the iPhone X (iPhone10,3 / D22AP) IPSW. MobileGestalt
# derives product type, device class, home-button-less UI, artwork etc. from
# the root identity and /product of the real D22AP device tree. That tree is
# an Apple artifact (kept out of git): extract it from the IPSW with
#   tools/ipsw.py fetch --product iPhone10,3 --extract DeviceTree.d22ap.im4p
#   ipsw img4 im4p extract DIR/Firmware/all_flash/DeviceTree.d22ap.im4p -o d22ap.adt
D22AP_ADT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '../research/ipsw-x/d22ap.adt')

u32 = lambda *v: struct.pack('<%dI' % len(v), *v)
u64 = lambda *v: struct.pack('<%dQ' % len(v), *v)
cstr = lambda s: s.encode() + b'\0'


def d22ap_product(path=D22AP_ADT):
    """/product properties of the real D22AP tree, without the syscfg
    placeholders iBoot would fill in (flag bit set) and the node bookkeeping."""
    if not os.path.exists(path):
        print(f'warning: {path} missing, no /product node', file=sys.stderr)
        return None
    root, _ = adt.parse(open(path, 'rb').read())
    for c in root.children:
        if dict((k, v) for k, f, v in c.props).get('name') == b'product\0':
            return [(k, v) for k, f, v in c.props if not f and k not in ('name', 'AAPL,phandle')]
    raise SystemExit(f'{path}: no /product node')


def node(name, props=(), children=()):
    n = adt.Node()
    n.props = [('name', 0, cstr(name))] + [(k, 0, v) for k, v in props]
    n.children = list(children)
    return n


def build(seed=None, qemu=False, highmem=None, device='sweet'):
    """qemu=True: same tree, but with QEMU virt's GIC addresses and CNTFRQ
    (tools/qemu-sweet.py).  highmem: None = the device default (always the
    upper DDR bank on chiron), True/False force --highmem/--lowmem."""
    dev = devices.get(device)
    if highmem is None:
        highmem = device != 'sweet'   # sweet keeps its old low-bank default
    phys_base, mem_size = dev['ram_high'] if highmem else dev['ram_low']
    gicd, gicr, timebase = (QEMU_GICD, QEMU_GICR, QEMU_TIMEBASE) if qemu else \
        (dev['gic']['dist'], dev['gic']['redist'], dev['timebase'])
    gicr_size = dev['gic']['redist_size']
    vtimer_intid = QEMU_VTIMER_INTID if qemu else dev['vtimer_intid']
    seed = seed if seed is not None else os.urandom(64)
    phandle = iter(range(1, 1000))
    ph = lambda: ('AAPL,phandle', u32(next(phandle)))
    disp = dev['display']
    # tuple so that a kernel built before patch 0082 (which matches the old
    # '<device>,display' string) still finds the display node
    disp_compat = dev['display_match'] if isinstance(dev['display_match'], tuple) \
        else (dev['display_match'],)

    chosen = node('chosen', [
        ph(),
        ('dram-base', u64(phys_base)),
        ('dram-size', u64(mem_size)),
        ('random-seed', seed),
        ('debug-enabled', u32(1)),
        ('firmware-version', cstr(dev['name'] + '-shim')),
        ('system-firmware-version', cstr(dev['name'] + '-shim')),
        # iBoot's SHA-384 of the boot manifest; /sbin/mount -P 1 exits unless
        # it exists (bind mounts /private/preboot/<hex>/usr/standalone/firmware,
        # tolerating a missing directory). Fixed stand-in value.
        ('boot-manifest-hash', BOOT_MANIFEST_HASH),
        # libimg4 (cryptexd) aborts "failed to get crypto algorithm" without
        # it; real A11+ device trees say sha2-384.
        ('crypto-hash-method', cstr('sha2-384')),
        # MobileGestalt's screen class: display-scale (u32, default 0) comes
        # from /chosen, the pixel size from IOMobileFramebuffer. Scale 0 makes
        # SpringBoard's FBSDisplayMode "infxinf@0x" invalid. 3x = iPhone class.
        ('display-scale', u32(disp['scale'])),
        # XNU derives IOPlatformUUID (gethostuuid) from these on arm; without
        # them CFPreferences cannot use kCFPreferencesCurrentHost (no ByHost
        # files), so e.g. DataMigration never records LastSystemVersion and
        # migrates on every boot. Synthetic ECID (not the phone's); chip-id
        # of the A11 that the iPhone X userland expects.
        ('unique-chip-id', struct.pack('<Q', dev['ecid'])),
        ('chip-id', u32(dev['iphone_chip_id'])),
        # Security status as on a dev-fused, undemoted board (tuple 0,0,1,1):
        # MobileGestalt reads these from /chosen, and mobileactivationd's
        # DeviceType then takes its native "Hactivation" branch (state
        # Activated without an activation record; no SEP, no baseband here).
        ('certificate-production-status', u32(0)),
        ('effective-production-status-ap', u32(0)),
        ('certificate-security-mode', u32(1)),
        ('effective-security-mode-sep', u32(1)),
    ], [node('memory-map', [ph()]),
        # Local boot policy (kern_csr.c): SIP relaxed like `csrutil disable`
        # (lp-sip0: untrusted kexts, unrestricted FS, task_for_pid, kernel
        # debugger, dtrace, nvram), lp-sip1: unauthenticated (unsealed) root.
        # The iOS shared cache is in no trust cache, so dyld can only map it
        # with CSR_ALLOW_UNRESTRICTED_FS (vm_unix.c shared region checks).
        node('asmb', [ph(), ('lp-sip0', u64(0x6f)), ('lp-sip1', u32(1))])])

    # SMP: the device's cores in boot (device tree) order, one homogeneous
    # cluster (SWEET has no USE_APPLEARMSMP). The kernel defaults to one CPU
    # and uses cpus=N to select an initial prefix (N <= the core count).
    # QEMU keeps a single generic CPU: -M virt's MPIDRs differ from both SoCs.
    cpu_nodes = []
    cores = devices.cpu_list(dev)
    if qemu:
        little = next((k for _m, _c, k, big in cores if not big), dev['cpu_clock_big'])
        cores = [(0x0, 0, little, True)]
    for core, (mpidr, cluster, clock, _big) in enumerate(cores):
        cpu_nodes.append(node(f'cpu{core}', [
            ph(),
            ('device_type', cstr('cpu')),
            ('compatible', cstr(dev['cpu_compatible']) + cstr('ARM,v8')),
            ('reg', u32(mpidr)),         # actual MPIDR Aff1:Aff0, NOT logical ID
            ('cpu-id', u32(core)),       # descriptive: XNU assigns IDs by DT order
            ('cluster-id', u32(0)),      # descriptive; see ml_parse_cpu_topology
            ('cluster-type', u32(0)),    # homogeneous CLUSTER_TYPE_SMP, not AMP
            ('die-cluster-id', u32(0)),
            ('cluster-core-id', u32(core)),
            ('l2-cache-id', u32(core)),  # distinct per-core L2 nodes in live FDT
            ('l3-cache-id', u32(0)),     # shared L3
            ('state', cstr('running' if core == 0 else 'stopped')),
            ('timebase-frequency', u32(timebase)),
            ('clock-frequency', u32(clock)),
            ('bus-frequency', u32(100000000)),
            ('memory-frequency', u32(dev['mem_frequency'])),
            ('peripheral-frequency', u32(timebase)),
            ('fixed-frequency', u32(timebase)),
            ('cluster', u32(cluster)),   # real cluster number, for the log
            ('mpidr', u32(mpidr)),
        ]))
    cpus = node('cpus', [ph(), ('#address-cells', u32(1)),
                         ('#size-cells', u32(0))], cpu_nodes)

    gic = node('gic', [
        ph(),
        ('compatible', cstr('arm,gic-v3')),
        ('interrupt-controller', b''),
        ('virtual-timer-intid', u32(vtimer_intid)),
        ('reg', u64(gicd - SOC_BASE, dev['gic']['dist_size'],
                    gicr - SOC_BASE, gicr_size)),
    ])
    arm_io = node('arm-io', [
        ph(),
        ('device_type', cstr(dev['name'] + '-io')),
        ('compatible', cstr(dev['name'] + '-io') + cstr('arm-io')),
        ('#address-cells', u32(2)),
        ('#size-cells', u32(2)),
        ('ranges', u64(0, SOC_BASE, 0x100000000 - SOC_BASE)),
    ], [gic,
        # No hardware: watchdogd checks IODeviceTree:/arm-io/wdt for no-pmu and
        # otherwise crashes trying to set the PMU boot stage (customer builds
        # ignore the -wdt_no_pmu boot-arg).
        node('wdt', [ph(), ('no-pmu', u32(1))]),
        # Main display: IOMobileFramebuffer.framework counts /arm-io children
        # with this device_type and reads the canvas size from the node; the
        # kernel's AppleCLCD matches the board-independent compatible string
        # (xnu patch 0082).  canvas is the *logical* iPhone-class screen;
        # canvas + dots-per-inch + the real panel size are what let one kernel
        # binary serve panels of different sizes.  canvas-anchor says how the
        # panel sits in the canvas when the panel is smaller: 0 = centred
        # (sweet, 18 px off the top and bottom), 1 = top-aligned (chiron,
        # 1080x2160: 276 px off the bottom, so the status bar stays visible).
        node('disp0', [
            ph(),
            ('device_type', cstr('display-subsystem')),
            ('compatible', b''.join(cstr(c) for c in disp_compat)),
            ('canvas-width', u32(disp['logical_w'])),
            ('canvas-height', u32(disp['logical_h'])),
            ('canvas-anchor', u32(1 if disp.get('anchor') == 'top' else 0)),
            ('dots-per-inch', u32(disp['dpi'])),
            ('panel-width', u32(dev['fb']['width'])),
            ('panel-height', u32(dev['fb']['height'])),
        ])])

    fb = dev['fb']
    fbcon = node('sweet-fbcon', [
        ph(),
        ('reg', u64(fb['base'], fb['size'])),
        ('width', u32(fb['width'])),
        ('height', u32(fb['height'])),
        ('stride', u32(fb['stride'])),
    ])

    # bsd/dev/sweet_ufs.c: the UFS host ABL leaves running (absolute physical
    # reg).  The shim and the UEFI loader can override the node at runtime.
    ufs = node('sweet-ufs', [
        ph(),
        ('compatible', cstr('qcom,ufshc')),
        ('reg', u64(dev['ufs'][0], dev['ufs'][1])),
    ])

    # pmap_compute_max_asids requires the node even when no overrides are set.
    # An absent pmap-max-asids property selects XNU's own MAX_ASIDS default.
    # No effaceable storage: keybagd then reads /private/var/keybags/systembag.kb
    # as a plain plist instead of decrypting it with the effaceable key.
    defaults = node('defaults', [ph(), ('no-effaceable-storage', u32(1))])

    # MobileGestalt HomeButtonType = IODeviceTree:/buttons home-button-type
    # (D22AP: 2, no Home button; absent = 0 = Home-button device).
    buttons = node('buttons', [
        ph(),
        ('button-names', cstr('volup') + cstr('voldown') + cstr('hold')),
        ('home-button-type', u32(2)),
        ('opposed-power-vol-buttons', u32(1)),
    ])

    product_props = d22ap_product()
    product = [node('product', [ph()] + product_props)] if product_props else []

    # Identity of the iPhone X whose userland runs here: compatible[0] is
    # hw.model/hw.target (D22AP), model is hw.machine/hw.product
    # (iPhone10,3). The board strings after it are what the platform expert
    # and AppleCLCD personalities match.
    return node('device-tree', [
        ph(),
        ('compatible', b''.join(cstr(c) for c in devices.compatible(dev))),
        ('model', cstr('iPhone10,3')),
        ('target-type', cstr('D22')),
        ('target-sub-type', cstr('D22AP')),
        ('manufacturer', cstr('Apple Inc.')),
        ('#address-cells', u32(2)),
        ('#size-cells', u32(2)),
    ], [chosen, defaults, cpus, arm_io, fbcon, buttons] + product + ([] if qemu else [ufs]))


def main(argv):
    if len(argv) < 2:
        raise SystemExit(__doc__)
    out = argv[1]
    device = 'sweet'
    if '--device' in argv:
        device = argv[argv.index('--device') + 1]
    if '--highmem' in argv:
        highmem = True
    elif '--lowmem' in argv:
        highmem = False
    else:
        highmem = None
    root = build(qemu='--qemu' in argv, highmem=highmem, device=device)
    blob = adt.serialize(root)
    open(out, 'wb').write(blob)
    if '--dump' in argv:
        adt.dump(adt.parse(blob)[0])
    dev = devices.get(device)
    phys = dev['ram_high'] if (highmem or (highmem is None and device != 'sweet')) else dev['ram_low']
    print(f'{out}: {len(blob)} bytes, {device} ({dev["soc"]}), '
          f'dram {phys[0]:#x}+{phys[1]:#x}, '
          f'gicd {dev["gic"]["dist"]:#x} gicr {dev["gic"]["redist"]:#x}, '
          f'vtimer {dev["vtimer_intid"]}', file=sys.stderr)


if __name__ == '__main__':
    main(sys.argv)
