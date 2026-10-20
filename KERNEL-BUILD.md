# Building the uke kernel in CI

The kernel step needs macOS with Xcode: it drives xnu's makefiles with
`xcodebuild` and the iPhoneOS SDK, and generates Mach stubs with Apple's MIG.
GitHub's macOS runners have both. `.github/workflows/build-kernel.yml` is the
CI version of `docs/MACOS-BUILD.md`.

## What it builds

The first-console kernel from `configs/uke_defconfig`:

- 4 KiB XNU with the source corecrypto provider;
- the Apple-format device tree platform expert (`ArmDTPlatform`);
- the architectural CPU and interrupt provider (`ArmCpu`);
- the architectural timer, PAN, DT panic region and trust-cache/CS-hash
  runtime;
- the GOP legacy display provider (`NativeLegacyDisplay`), so the kernel can
  draw a console/panic screen through the frame buffer the UEFI firmware
  handed over.

UFS, NativeAES, VirtIO, RamBlock, PMGR, Adreno and the owner-root/userland
stack stay off. A panic that names a storage/root provider, or AMFI's
`No PMGR`, is the expected result of this pass.

`configs/uke-full_defconfig` is the later sheng-derived configuration.
`configs/bootstrap_defconfig` is the no-device-archive bootstrap pass used by
the workflow.

## Why the workflow has a bootstrap pass

Two conditions in the base `scripts/build-xnu-4k-baseline.sh` form a cycle:

1. The archive-linking block is gated on `XNU_SOURCE_CORECRYPTO14`. With the
   crypto tree off, not even `XNU_EXTRA_LD_ARCHIVES` is applied.
2. With the crypto tree on, the script requires
   `build/corecrypto-2023/libcorecrypto_kernel_noasm-namespaced.a`. Producing
   that archive needs `scripts/build-corecrypto-14.py --namespace`, which
   reads the built kernel's undefined symbols.

Additionally, `build-arm-driver.py` and `build-native-legacy-display.py`
compile against the kernel's recorded command line
(`build/xnu/.../IOPlatformExpert.cpo.json`), and the legacy display builder
needs an original-class candidate Mach-O.

So the workflow runs:

1. `bootstrap_defconfig` → `unixnu build kernel` → compile records plus
   `out/uke/kernel/kernel.macho`; copy the prelink output and its
   `kernel.json` plan to
   `out/uke/tree/build/native-prelink/native-owner-mobile-graphics-candidate.{macho,json}`.
2. Clone the pinned `ONLYA/corecrypto-2023` (`11fba65…`) into `refer/` and run
   `build-corecrypto-14.py` plain and `--namespace`.
3. Seed the original-class runtime observation hash, build `ArmDTPlatform`,
   `ArmCpu`, the empty touch-firmware object and the legacy display archive.
   The hash is the sheng-recorded observation of the unchanged iPad7,1
   original class; uke pins the same kernelcache, and the audit accepts the
   recorded hash when the GDB dump is absent.
4. `uke_defconfig` → `unixnu build kernel`, which links the archives and
   prelinks against the iPad7,1 kernelcache.

`scripts/build-xnu-4k-baseline.sh` in this repository overrides the base
script. It appends a `native_legacy_display_probe` logging stub only when
`NATIVE_LEGACY_DISPLAY` is off (the bootstrap pass), and leaves the symbol to
the legacy display archive in the shipping build.

## Running it

Push this repository to GitHub, then either use the Actions tab or:

```sh
gh workflow run build-uke-kernel
```

Download the `uke-kernel` artifact. It contains:

- `kernel.macho` — the prelinked first-console kernel;
- `kernel.macho.sha256`;
- `mach.development.bcm2837.unstripped.noctf` — the native kernel before
  prelink;
- `logs/` — the build logs.

The workflow tracks `UniXNU/UniXNU` `main` (the current `origin/main` with
`UniXNUPkg` and `unixnu.toml`). Update the pin when the base repository moves.

## Runner and Xcode

Use `macos-latest` (macOS 26, Xcode 26.x). The piano port's
`KERNEL-BUILD.md` records why `macos-14` / Xcode 15.4 is too old:

```
clang: error: unknown argument: '-mach_msg2'
mig: fatal: no SubSystem declaration
```

The standard macOS runner (3-core M1, 7 GB RAM, 14 GB SSD) was enough for the
piano kernel: 9m12s from checkout to a 51.8 MB prelinked kernel. The uke
archive set adds the legacy display provider, so expect somewhat longer.

## After the download: payload and boot image on Linux

The payload and boot image do not need macOS. Copy `kernel.macho` to the
Linux host and run:

```sh
cd UniXNU
./unixnu defconfig uke_defconfig -D ../UniXNU-uke
./unixnu build payload -D ../UniXNU-uke \
    --kernel /path/to/kernel.macho \
    --unique-chip-id <hex>
./unixnu bootimg -D ../UniXNU-uke \
    --uefi ../uefi-uke \
    --dtb /path/to/vendor_boot.dtb
```

`board.toml` currently uses `size_mib = 128`. The prelinked
kernel is about 50 MB, but the full 192 MiB FAT volume makes the boot image
larger than the 96 MiB `boot_a` partition. For the first kernel-only payload,
set `size_mib = 64` or `80`; `fastboot boot` may accept a larger image, but
keeping it at or below 96 MiB also keeps a later `fastboot flash boot`
possible.

## Known gaps after this build

- **UFS/root.** `NativeUfs`, the vendor UEFI UFS/Clock/PMIC stack, the
  userdata image and the trust cache/Data seed are not part of this pass.
- **PMGR/AMFI.** `CONFIG_XNU_SHENG_PMGR` is off in `uke_defconfig`; the
  `sheng-pmgr=2` argument is present in `board.toml` but the wrapper is not
  linked. A panic at AMFI is expected; the next step is to enable the PMGR
  archive and relink.
- **Touch.** The touch firmware object is empty, so the NT36532 probe stops
  before the download stage. The protocol driver is present for the later
  firmware/pin adaptation.
- **GPU.** The Adreno 732 is not supported by the current A740 kit.
- **Userland.** It needs the userdata image (macOS `hdiutil`, or a Linux DMG
  conversion port), a matching `root_match_volume_uuid`, a trust cache bundle
  and a Data seed.
