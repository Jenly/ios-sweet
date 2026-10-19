# XNU for ios-sweet

Built on GitHub's macOS arm64 runners by `.github/workflows/xnu.yml`, using
[blacktop/darwin-xnu-build](https://github.com/blacktop/darwin-xnu-build) (MIT) at a pinned commit.

- `MACOS_VERSION=13.5` -> xnu-8796 (same generation as iOS 16.6/16.7, the last iOS for iPhone 8/X).
- `patches/*.patch` are copied into the build script's patch dir and applied to the xnu tree with `git apply`.
- Output: `kernel.development.sweet` as a workflow artifact.

Fetch the latest build: `gh run download -R dipa-studio/ios-sweet -n xnu-kernel -D xnu/out`

## One kernel, several boards

The MACHINE_CONFIG is still called `SWEET` (it selects the generic-ARM, no-PAC,
16K-page, GICv3 platform code), but since patch **0082** the binary is not tied
to the Redmi Note 10 Pro any more:

- `SweetBoard.h` (installed as `pexpert/pexpert/SweetBoard.h`) reads the board
  from the Apple Device Tree that `tools/mkadt.py` generates: the root
  `compatible` list names the SoC (`qcom,sm7150` / `qcom,msm8998`) and the
  board-independent `ios-sweet`, and `/arm-io/disp0` carries the panel size,
  the presented canvas, the crop anchor and the DPI.
- The display driver (`SweetFramebuffer.cpp`) and the touch driver
  (`hid/SweetHID.cpp`) take their geometry from there, so a 1080x2400 panel and
  a 1080x2160 panel both present the iPhone X screen correctly.
- The SM7150-only drivers (`bsd/dev/sweet_touch.c`, `sweet_keys.c`,
  `sweet_usb.c`) check the SoC and stay off elsewhere; `sweettouch=1`,
  `sweetkeys=1`, `sweetusb=1` force them.
- Personalities match `ios-sweet` / `ios-sweet,display`; the old
  `xiaomi,sweet` / `sweet,display` strings stay in the ADT so a kernel built
  before 0082 still boots with a new ADT.

The per-device numbers live in `../tools/devices.py`, not in the patches.

## Checking a patch without a kernel build

- `tools/xnu-cc.py LOG BTREE FILTER` replays CI compile commands for single files.
- `tools/test-board-header.py ADT...` compiles and runs the device-tree helpers
  (`SweetBoard.h`) against real generated ADTs, including C++ parse checks.
- `git apply --check` the whole series onto a fresh xnu-8796.141.3 checkout: the
  staging scripts (`0019-stage-libpthread.sh`, `0020-stage-bearssl.sh`,
  `0026-stage-hfs.sh`) must run first, exactly as CI does.
