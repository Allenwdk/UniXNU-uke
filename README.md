# UniXNU-uke

Device repository for the Xiaomi Pad 7 / POCO Pad X1 (`uke`, SM7675).

Stage 1 (UEFI firmware + UniXNU menu) works on the stock ABL. The first XNU
kernel is built in GitHub Actions with
`.github/workflows/build-kernel.yml`; the payload and boot image are built on
Linux. The userdata image still needs macOS `hdiutil` (or a Linux port of the
DMG conversion).

## Layout

- `board.toml` — board description, IPSW identity, userdata geometry, payload
  and ADT settings.
- `configs/uke_defconfig` — first-console kernel: 4 KiB XNU, source
  corecrypto, `ArmDTPlatform`, `ArmCpu`, arch timer, PAN, panic region and the
  GOP legacy display. No UFS, AES, VirtIO, RamBlock, PMGR, Adreno or owner-root
  stack.
- `configs/uke-full_defconfig` — the full sheng-derived configuration for
  later stages.
- `configs/bootstrap_defconfig` — no-device-archive bootstrap pass: compile
  records plus the prelinked candidate the legacy display ABI builder needs.
- `scripts/build-xnu-4k-baseline.sh` — base script plus a
  `native_legacy_display_probe` logging stub that is used only when
  `NATIVE_LEGACY_DISPLAY` is off (the bootstrap pass).
- `.github/workflows/build-kernel.yml` — the CI version of
  `docs/MACOS-BUILD.md`: bootstrap → corecrypto → ArmDTPlatform / ArmCpu →
  legacy display → first-console kernel + prelink.
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

The CI kernel is the first-console build: BootShim/UEFI, UniXNU, the payload
`xnu` entry, DarwinBoot, then XNU with the GOP legacy display. UFS, PMGR and
the owner-root/userland stack are off, so a panic that names a storage/root
provider or AMFI's `No PMGR` is the expected milestone. See `KERNEL-BUILD.md`
for the bootstrap/candidate/display sequence and the Xcode 26 requirement
(`macos-14` / Xcode 15.4 fails with `-mach_msg2` and MIG).

`board.toml` uses `size_mib = 128` for the payload; the prelinked kernel is
about 50 MB and `boot_a` is 96 MiB. For the first kernel-only payload, use
`size_mib = 64` or `80`. `fastboot boot` may accept a larger image, but
keeping it at or below 96 MiB also keeps a later `fastboot flash boot`
possible.

## Stage 1 test

`fastboot boot` does not flash anything; the stock boot partitions stay
untouched. Run this only when you are ready to leave Android:

```sh
adb reboot bootloader
fastboot boot out/uke/boot/boot.img
```

Expected: the panel shows the UniXNU menu (or the UEFI graphics console).
If the firmware stalls, retry the same image once; the sheng port is known to
stall occasionally. To return without booting the image:

```sh
fastboot reboot
```

## Known gaps

- `bootimg.py` now understands Android boot v4; the stock uke image is v4.
- `MemoryMapLib.c` is generated from the live stock DT plus the Mu-Silicium
  FD/stack layout. `check-uke.py` reports 0 static errors (72 descriptors).
- The first-console kernel has the GOP legacy display, but no UFS, PMGR,
  owner-root or GPU. `uke-full_defconfig` is the later configuration.
- The vendor UFS/clock/PMIC stack (`UKE_VENDOR_HW = 1`) is not imported yet;
  XNU native UFS root needs it, or a kernel-side UFS init.
- The touch firmware object is empty, so the NT36532 probe stops before the
  download stage. The protocol driver is present for the later firmware/pin
  adaptation.
- The userdata image needs macOS `hdiutil`; a Linux port using
  `qemu-img convert -f dmg -O raw` plus the existing `SHNGROOT` writer is
  possible but not implemented here.
- GPU, USB, audio and Wi-Fi are not part of the first console pass.
