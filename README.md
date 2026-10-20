# UniXNU-uke

Device repository for the Xiaomi Pad 7 / POCO Pad X1 (`uke`, SM7675).

Stages 1–5 are reached: the UEFI firmware, the UniXNU menu, the payload,
DarwinBoot and the XNU kernel entry (green bar) work on the stock ABL. After
the green bar XNU waits for the root device; `SecurityLock ...
LockCapsuleInterface` is UEFI output, not the kernel hang.

Stage 6 now **compiles**: `uke-full_defconfig` builds the 4 KiB XNU with source
corecrypto, pthread, NativeUfs, NativeAES, mockfs, PMGR, the public storage
headers and the GOP legacy display, and prelinks it into `uke-kernel-full`.
The device is not connected yet, so the storage/root runtime path is untested.

Progress, remaining work and the update rule: **[docs/PROGRESS.md](docs/PROGRESS.md)**.
The kernels are built in GitHub Actions with a cached `out/` tree; the payload
and boot image are built on Linux. The userdata image still needs macOS
`hdiutil` (or a Linux port of the DMG conversion). GPU port preparation and the
A732 values are in **[docs/GPU-PORT.md](docs/GPU-PORT.md)**.

## Layout

- `board.toml` — board description, IPSW identity, userdata geometry, payload
  and ADT settings.
- `configs/uke_defconfig` — first-console kernel: 4 KiB XNU, source
  corecrypto, `ArmDTPlatform`, `ArmCpu`, arch timer, PAN, panic region and the
  GOP legacy display. No UFS, AES, VirtIO, RamBlock, PMGR, Adreno or owner-root
  stack.
- `configs/uke-full_defconfig` — the reproducible stage-6 full configuration:
  corecrypto, pthread, NativeUfs (bring-up transport), NativeAES with the
  recorded original bindings, storage headers, mockfs, PMGR, arch timer, PAN,
  panic region and the GOP legacy display. `CONFIG_XNU_BUILD_ARCHIVES=y` runs
  the board archive list.
- `configs/uke-full-plus_defconfig` — sheng parity: the full set plus the
  storage ABI probes, verified root, VirtIO/RamBlock and owner-root. It needs
  live storage metadata, signed policy bundles and the Adreno 732 firmware, so
  it is not built by CI.
- `configs/uke-full-minus-pmgr_defconfig` — PMGR + legacy display only; used
  to show that PMGR is not the root-device blocker.
- `configs/bootstrap_defconfig` — no-device-archive bootstrap pass: compile
  records plus the prelinked candidate the legacy display ABI builder and the
  UFS binder need.
- `docs/PROGRESS.md` — current bring-up stage, completed work, remaining work,
  artifacts and the update rule. Keep it current after every CI/hardware run.
- `docs/GPU-PORT.md` — Adreno 732 values, the A740/A732 differences and the
  firmware the vendor partition must supply.
- `scripts/build-xnu-4k-baseline.sh` — base script plus a
  `native_legacy_display_probe` logging stub that is used only when
  `NATIVE_LEGACY_DISPLAY` is off (the bootstrap pass).
- `.github/workflows/build-kernel.yml` — the CI version of
  `docs/MACOS-BUILD.md`: bootstrap → cached archive chain → first-console or
  full kernel + prelink. The `full` job uses the board archive list.
- `scripts/make-uke-adt.py` — XNU Apple device tree generator. GIC, UFS,
  timebase and DRAM base are the values read from the live stock DT; the root
  compatible is `xiaomi,uke` / `qcom,sm7675`.
- `scripts/check-uke.py` — compares the live DT reserved-memory ranges with
  `uefi-uke/Platforms/Xiaomi/ukePkg/Library/MemoryMapLib/MemoryMapLib.c`.
- `drivers/NativeNovatekTouch` — NT36532 protocol (from the sheng port).
- `package/` — kernel-side probes/kits copied from the sheng port; the
  device package is linked as `kits/uke`.
- `KERNEL-BUILD.md` — the CI build notes; `docs/MACOS-BUILD.md` is the manual
  macOS guide.

## Hardware data captured from the device

- Device: `2410CRP4CC`, 12 GB RAM, 256 GB Samsung UFS, Android 17
  (`OS4.0.4.0.XPYCNXM`), slot `_a`.
- `userdata`: `/dev/block/sda32`, first 4 KiB LBA `0x2d3000`,
  last `0x3b147fa` (225 GiB).
- `splash_region`: `0xe3940000` + `0x2b00000` (the live DT; sheng's
  `0xb8000000` is not valid here).
- UFS: `0x1d84000`, compatible `qcom,ufshc`, stream ID `0x60`.
- Touch: NT36532 on GENI SPI `0xa90000`, IRQ GPIO 54, lcd_id GPIO 26,
  19.2 MHz.
- Panel: O82, selected by `msm_drm.dsi_display0=qcom,mdss_dsi_o82_42_02_0b_dualdsi_dsc_vid:`.
- Stock boot: Android header v4, 4096-byte pages, 96 MiB partition.

## Build (Linux, firmware)

```sh
cd /path/to/UniXNU
./unixnu defconfig uke_defconfig -D ../UniXNU-uke
./unixnu fetch -D ../UniXNU-uke
./unixnu build firmware --skip-preflight -D ../UniXNU-uke \
    --uefi ../uefi-uke -j 8
./unixnu bootimg -D ../UniXNU-uke --uefi ../uefi-uke \
    --dtb ../uke-port/data/vendor_boot.dtb --ramdisk none
```

The firmware is `uefi-uke/Build/ukePkg/DEBUG_CLANGPDB/FV/SILICIUM_UEFI.fd`
(3 MiB); the UEFI-only boot image is `out/uke/boot/boot.img` (Android v4,
~1.5 MiB). The UEFI build uses `UKE_VENDOR_HW = 0`: Mu drivers,
`SimpleFbDxe`, the UniXNU boot manager and DarwinBoot; no uke vendor UEFI
drivers yet.

## Build the kernel in GitHub Actions

Push this repository to GitHub, then:

```sh
gh workflow run build-uke-kernel
```

Download the `uke-kernel` artifact and use `kernel.macho` for the payload:

```sh
cd UniXNU
./unixnu defconfig uke_defconfig -D ../UniXNU-uke
./unixnu build payload -D ../UniXNU-uke --kernel /path/to/kernel.macho \
    --unique-chip-id <hex>
./unixnu bootimg -D ../UniXNU-uke --uefi ../uefi-uke \
    --dtb /path/to/vendor_boot.dtb
```

The CI has two kernel jobs. `uke-kernel` is the first-console kernel:
BootShim/UEFI, UniXNU, the payload `xnu` entry, DarwinBoot, then XNU with the
GOP legacy display. `uke-kernel-full` is the stage-6 kernel: the same bootstrap
plus the board archive chain (corecrypto, ArmDTPlatform, ArmCpu, NativeAES with
the recorded original bindings, storage headers, the NativeUfs bring-up
transport with the original block-class bindings, mockfs, PMGR, pthread and the
legacy display). Both reach the XNU entry (green bar); the full kernel can now
publish a UFS block device, but the device has not been booted with it yet.
Verified-root/owner-root, VirtIO/RamBlock and boot progress stay off until
their live audit inputs exist. See `docs/PROGRESS.md` and `KERNEL-BUILD.md`.

`board.toml` uses `size_mib = 80` for the payload; the prelinked kernel is
about 50 MB and `boot_a` is 96 MiB. The uke ABL's `fastboot boot` returns
`Failed to load/authenticate boot image: Load Error`, so use the normal boot
path (`fastboot flash boot` + `flash vendor_boot` + `reboot`) for hardware
tests.

## Current hardware test

The uke ABL rejects `fastboot boot` with
`Failed to load/authenticate boot image: Load Error`, so test by flashing the
normal boot path:

```sh
adb reboot bootloader
fastboot flash boot out/uke/boot/boot.img
fastboot flash vendor_boot /path/to/vendor_boot_uke_full.img
fastboot set_active a
fastboot reboot
```

Expected: the UniXNU menu, the `xnu` entry after 5 s, DarwinBoot output, the
green bar and XNU. After the green bar XNU waits for the root device; see
`docs/PROGRESS.md`. The stock boot and vendor_boot images restore Android:

```sh
fastboot flash boot /path/to/boot_a.img
fastboot flash vendor_boot /path/to/vendor_boot_a.img
fastboot set_active a
fastboot reboot
```

Use `fastboot set_active a` before each test: the ABL marks the slot
unbootable after repeated non-Android boots.

If the firmware stalls, retry the same image once; the sheng port is known to
stall occasionally.

## Known gaps

- `bootimg.py` now understands Android boot v4; the stock uke image is v4.
- `MemoryMapLib.c` is generated from the live stock DT plus the Mu-Silicium
  FD/stack layout. `check-uke.py` reports 0 static errors (72 descriptors).
- `UkeBootMemoryDxe` publishes the initrd after rounding its end down to a
  512-byte sector, maps the initrd range in the GCD, and adds up to 2 GiB of
  DRAM. The ADT uses `owned_memory_size = 0x10000000` because the largest
  contiguous free run on uke is about 326 MiB.
- Stages 1–5 are reached (XNU entry, green bar). Stage 6 now compiles: the
  full kernel links NativeUfs and NativeAES, and the UFS block class binds to
  the pinned original candidate. Runtime storage/root is untested; the device
  is not connected.
- The UFS block reads are **not authenticated** in the bring-up build. The
  verified-root policy bundle (built from the real OS/SystemOS DMGs) and the
  live storage ABI audit are the remaining gates for authenticated reads.
- `userdata`, the root UUID and the APFS/launchd chain still need a device run.
- `uke-full-plus_defconfig` lists the sheng-parity flags that are not built:
  VirtIO/RamBlock, verified root, owner-root and boot progress.
- The uke SM7675 GCC UFS block is at `0x177000`/`0x19e000`; the device
  overrides `drivers/NativeUfs` with those offsets.
- The vendor UFS/clock/PMIC stack (`UKE_VENDOR_HW = 1`) is not imported.
- `panic_screen = false` because the panic-screen builder needs `Font.h`;
  enable it after fixing the include path so a real panic is captured.
- The touch firmware object is empty, so the NT36532 probe stops before the
  download stage.
- The userdata image needs macOS `hdiutil`; a Linux port using
  `qemu-img convert -f dmg -O raw` plus the existing `SHNGROOT` writer is
  possible but not implemented here.
- GPU: the Adreno 732 values are recorded and checked; the driver still needs
  the A732 firmware from the vendor partition and the GPU power path. See
  `docs/GPU-PORT.md`.
- USB, audio, Wi-Fi and sensors are not part of the current pass.

See [docs/PROGRESS.md](docs/PROGRESS.md) for the full state, artifacts and
maintenance rule.
