# Building the uke kernel in CI

The kernel step needs macOS with Xcode: it drives xnu's makefiles with
`xcodebuild` and the iPhoneOS SDK, and generates Mach stubs with Apple's MIG.
GitHub's macOS runners have both. `.github/workflows/build-kernel.yml` is the
CI version of `docs/MACOS-BUILD.md`.

## What it builds

Two jobs:

**`kernel` — first-console** (`configs/uke_defconfig`):

- 4 KiB XNU with the source corecrypto provider;
- `ArmDTPlatform`, `ArmCpu`, the architectural timer, PAN, DT panic region and
  the trust-cache/CS-hash runtime;
- the GOP legacy display provider (`NativeLegacyDisplay`).

**`full` — stage-6** (`configs/uke-full_defconfig`):

- everything above, plus `SOURCE_PTHREAD`, `NativeUfs`, `NativeAES`,
  `SOURCE_MOCKFS`, `NATIVE_CS_HASH_CACHE`, `SHENG_PMGR`,
  `ZONE_PGZ_ALIGNMENT_PROBE` and `NATIVE_LEGACY_DISPLAY`.

The full job runs the board archive list in `board.toml`:

1. `build-corecrypto-14.py` and `--namespace`;
2. `build-arm-driver.py` for `ArmDTPlatform`, `ArmCpu` and `NativeAES`;
3. `build-native-aes-bindings.py` (recorded original AES addresses);
4. `prepare-storage-headers.py` (public `IOStorageFamily` headers);
5. `build-arm-driver.py NativeUfs --unverified`;
6. `build-ufs-original-bindings.py` (original `IOBlockStorageDevice`
   vtable/metaclass/constructor/destructor from the pinned prelink candidate);
7. `build-mockfs-fixture.py`;
8. `build-sheng-platform-power.py`;
9. `build-libpthread-kernel.py`;
10. `build-native-legacy-display.py`.

The hardware-qualification gates stay off: the verified-root policy bundle,
the live storage ABI snapshot, VirtIO/RamBlock, owner-root and boot progress.
`configs/uke-full-plus_defconfig` keeps those flags for the later stage.

`configs/bootstrap_defconfig` is the no-device-archive bootstrap pass used by
both jobs.

## Why the workflow has a bootstrap pass

Archive builders and mode-specific objects read the compile records that an
XNU pass leaves behind, and the legacy display builder and the UFS binder need
a prelinked original-class candidate. The full kernel also links archives, so
its first pass cannot be the final one.

The driver runs the board archive list only when the selected configuration
sets `CONFIG_XNU_BUILD_ARCHIVES=y`. `bootstrap_defconfig` does not, so the
bootstrap pass can link and prelink without running builders that need the
candidate it is about to produce. Both jobs run:

1. `bootstrap_defconfig` → `unixnu build kernel` → compile records plus
   `out/uke/kernel/kernel.macho` and `kernel.json`; copy the prelink output to
   `out/uke/tree/build/native-prelink/native-owner-mobile-graphics-candidate.{macho,json}`
   and the HID alias
   `native-owner-ufs-cpu-desktop-v9-hid-init-candidate.{macho,json}`.
2. Seed the display ABI observation hash (the first-console job also builds
   its four archives explicitly).
3. Select `uke_defconfig` or `uke-full_defconfig` → `unixnu build kernel`.
   The driver fetches `corecrypto-2023`, `libpthread` and `IOStorageFamily`
   as pinned feeds, runs the board archives for the full configuration, links
   the kernel and prelinks it against the iPad7,1 kernelcache.

`scripts/prepare-xnu-source.py` overrides the base header restore: it uses the
pinned xnu git tags instead of raw.githubusercontent.com, which GitHub Actions
rate-limits with HTTP 403. `scripts/build-xnu-4k-baseline.sh` in this repository
overrides the base script. It appends a `native_legacy_display_probe` logging
stub only when `NATIVE_LEGACY_DISPLAY` is off (the bootstrap pass), and leaves
the symbol to the legacy display archive in the shipping build.

## Caching

Both jobs restore:

- `UniXNU/out` — fetched feeds, IPSW members, the assembled tree, XNU objects
  and the previously built archives;
- `UniXNU-uke/.cache` — staged firmware, the libpthread work clone and the
  `xnu-work` patch clone;
- `~/Library/Caches/Homebrew` — the bottle download cache.

The key includes the `Allenwdk/UniXNU` `uke` commit and the hashes of
`board.toml`, `feeds.toml`, `configs/**` and `scripts/**`; `restore-keys`
still reuse the previous tree when the key changes. A cold run is about
15–17 minutes; a restored tree usually finishes in 8–10 minutes. Bump
`CACHE_SCHEMA` in the workflow when the object layout changes in a way the key
cannot see.

## Running it

Push this repository to GitHub, then either use the Actions tab or:

```sh
gh workflow run build-uke-kernel
```

Artifacts:

- `uke-kernel` — first-console `kernel.macho` + `kernel.macho.sha256` + logs;
- `uke-kernel-full` — full `kernel.macho` + `kernel.macho.sha256` + logs +
  the AES/storage/display reports.

The workflow checks out `Allenwdk/UniXNU` `uke`, which tracks upstream
`UniXNU/UniXNU` `main` plus the uke driver changes. Update the fork when the
base repository moves.

## Runner and Xcode

Use `macos-latest` (macOS 26, Xcode 26.x). The piano port's
`KERNEL-BUILD.md` records why `macos-14` / Xcode 15.4 is too old:

```
clang: error: unknown argument: '-mach_msg2'
mig: fatal: no SubSystem declaration
```

## After the download: payload and boot image on Linux

The payload and boot image do not need macOS. Copy `kernel.macho` to the
Linux host and run:

```sh
cd UniXNU
./unixnu defconfig uke-full_defconfig -D ../UniXNU-uke
./unixnu build payload -D ../UniXNU-uke \
    --kernel /path/to/kernel.macho \
    --unique-chip-id <hex>
./unixnu bootimg -D ../UniXNU-uke \
    --uefi ../uefi-uke \
    --dtb /path/to/vendor_boot.dtb
```

For the full kernel add `native-ufs-root=1` to `[payload] kernel_args` (keep
`sheng-pmgr=2`) and rebuild the payload. The prelinked kernel is about 52 MB;
`board.toml` uses `size_mib = 80` so the image stays under the 96 MiB `boot_a`
partition.

## Known gaps after this build

- **UFS/root runtime.** The full kernel links the UFS block class and the
  AES accelerator, but the device has not been booted with them. The UFS
  reads are unverified (no policy check); the verified path needs the signed
  OS/SystemOS bundles and the live storage ABI snapshot.
- **UFS power.** The device override uses the uke GCC windows
  (`0x177000`/`0x19e000`); PHY, reset and PMGR behaviour is still unqualified.
- **Adreno 732.** The driver still carries the A740 constants. The board
  values are in `[gpu]` and `docs/GPU-PORT.md`; the vendor firmware is needed.
- **Touch.** The touch firmware object is empty, so the NT36532 probe stops
  before the download stage.
- **Userland.** It needs the userdata image (macOS `hdiutil`, or a Linux DMG
  conversion port), a matching `root_match_volume_uuid`, a trust cache bundle
  and a Data seed.
