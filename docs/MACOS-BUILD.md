# Building the uke kernel on macOS

The Linux host builds the UEFI firmware and boot image. The kernel, prelink and
userdata steps need macOS with Xcode and `compression_tool`/`hdiutil`.

This guide uses the **first-console** configuration:

- `configs/uke_defconfig` — 4 KiB XNU, ArmDTPlatform, ArmCpu, architectural
  timer, PAN, DT panic region and the GOP legacy display. No UFS, AES, VirtIO,
  RamBlock, PMGR, Adreno or owner-root stack.
- `configs/uke-full_defconfig` — the full sheng-derived configuration for later
  stages.
- `configs/bootstrap_defconfig` — a no-device-archive bootstrap pass that
  produces the XNU compile records and a prelinked candidate.

## 1. Copy the trees into the macOS VM

The three trees are needed side by side:

```
UniXNU/          core build driver
UniXNU-uke/      this device repository
uefi-uke/        Mu-Silicium fork (already built on Linux)
```

Keep the existing `out/` directory out of the copy if you transfer by tarball;
the driver will fetch and rebuild inside macOS. Also copy
`vendor_boot.dtb` (the stock vendor_boot base DTB dumped on Linux) to
`UniXNU-uke/build/uke/uke.dtb`.

On the Linux host:

```sh
tar -C ~ --exclude=UniXNU/out --exclude=uefi-uke/Build \
    -czf /tmp/uke-src.tgz UniXNU UniXNU-uke uefi-uke
cd /tmp && python3 -m http.server 8000
```

In macOS, download `http://<linux-host-ip>:8000/uke-src.tgz` and extract it to
`~/uke`, then copy `vendor_boot.dtb` into `~/uke/UniXNU-uke/build/uke/uke.dtb`.

## 2. Install Xcode and host tools

- Install Xcode 15.x or 16.x (macOS 13/14/15).
- `sudo xcode-select -s /Applications/Xcode.app/Contents/Developer`
- `sudo xcodebuild -license accept`
- Verify: `xcodebuild -version` and
  `xcrun --sdk iphoneos --show-sdk-path`.
- `brew install python@3.12 llvm lld dtc mtools cmake ninja unifdef`
- Put Homebrew's Python and LLVM on PATH:

```sh
export PATH="$(brew --prefix)/opt/python@3.12/bin:$(brew --prefix)/opt/llvm/bin:$(brew --prefix)/opt/lld/bin:$PATH"
python3 --version
```

## 3. Bootstrap pass (compile records + prelinked candidate)

From the core tree:

```sh
cd ~/uke/UniXNU
./unixnu defconfig bootstrap_defconfig -D ../UniXNU-uke
./unixnu fetch
./unixnu ipsw fetch
./unixnu tree
```

Fetch the pinned corecrypto source into the assembled tree:

```sh
cd out/uke/tree
git clone https://github.com/ONLYA/corecrypto-2023 refer/corecrypto-2023
git -C refer/corecrypto-2023 checkout 11fba65b12d8830630e248dfa8344caf8b065f26
```

Build the base kernel and the prelinked candidate:

```sh
cd ~/uke/UniXNU
./unixnu build kernel -D ../UniXNU-uke
mkdir -p out/uke/tree/build/native-prelink
cp out/uke/kernel/kernel.macho \
   out/uke/tree/build/native-prelink/native-owner-mobile-graphics-candidate.macho
cp out/uke/kernel/kernel.json \
   out/uke/tree/build/native-prelink/native-owner-mobile-graphics-candidate.json
cp out/uke/kernel/kernel.macho \
   out/uke/tree/build/native-prelink/native-owner-ufs-cpu-desktop-v9-hid-init-candidate.macho
cp out/uke/kernel/kernel.json \
   out/uke/tree/build/native-prelink/native-owner-ufs-cpu-desktop-v9-hid-init-candidate.json
```

The bootstrap kernel does not have to boot. Its outputs are the XNU compile
records under `out/uke/tree/build/xnu/` and the prelink candidate used by the
display ABI builder.

## 4. Build the device archives

All commands run in the assembled tree:

```sh
cd ~/uke/UniXNU/out/uke/tree
mkdir -p build/display-analysis
python3 - <<'PY'
import json
from pathlib import Path
p = Path('build/display-analysis/mobile-graphics-legacy-abi.json')
if not p.exists():
    p.write_text(json.dumps({'runtime_metadata_checkpoint_sha256': '3044c2beff552911605994c0dc7aa73182066cbb5aed0b396999ceeb5a96da2b'}) + '\n')
PY
python3 scripts/gen-touch-firmware.py        # empty firmware object is fine
python3 scripts/build-corecrypto-14.py --namespace
python3 scripts/build-arm-driver.py ArmDTPlatform
python3 scripts/build-arm-driver.py ArmCpu
python3 scripts/build-native-legacy-display.py
```

The display builder runs the mobile-graphics ABI audit and builds
`build/native-legacy-display/libNativeLegacyDisplay.late.a`. It needs the
candidate copied in step 3; the touch firmware object exists even when the
vendor firmware is absent, and the touch probe stops before the download stage.

## 5. Build the first-console kernel, payload and boot image

```sh
cd ~/uke/UniXNU
./unixnu defconfig uke_defconfig -D ../UniXNU-uke
./unixnu build kernel -D ../UniXNU-uke

CHIP=$(python3 -c 'import secrets; print(hex(secrets.randbits(63) | 1))')
./unixnu build payload -D ../UniXNU-uke --unique-chip-id "$CHIP"

./unixnu bootimg -D ../UniXNU-uke --uefi ../uefi-uke \
    --dtb ../UniXNU-uke/build/uke/uke.dtb
```

Outputs:

- `out/uke/kernel/kernel.macho`
- `out/uke/payload/payload.img` and `payload.json`
- `out/uke/boot/boot.img` and `boot.json`

The payload volume is 128 MiB and includes the kernel and the ADT. The boot
image is Android v4.

## 6. Copy back and test

Copy `out/uke/boot/boot.img` back to the Linux host and run:

```sh
fastboot boot /path/to/boot.img
```

Expected sequence on the panel:

1. UniXNU menu.
2. The `xnu` entry (from the payload).
3. `DARWINBOOT:` log lines from the loaders.
4. XNU entry and console/panic output.

The first-console configuration has no UFS, no PMGR and no userland stack. A
panic that names a storage/root provider is expected at this stage; the panic
screen is the useful result. If the kernel panics at AMFI with `No PMGR`, the
next step is to enable `CONFIG_XNU_SHENG_PMGR=y`, build the PMGR archive and
relink; that is the storage/userland phase.

## Troubleshooting

- `build-arm-driver.py` cannot find
  `build/xnu/.../IOPlatformExpert.cpo.json`: run the bootstrap pass first.
- `build-native-legacy-display.py` cannot find the candidate: copy the
  bootstrap `kernel.macho` to
  `out/uke/tree/build/native-prelink/native-owner-mobile-graphics-candidate.macho`.
- `build-corecrypto-14.py` complains about the source revision: the
  `refer/corecrypto-2023` checkout must be
  `11fba65b12d8830630e248dfa8344caf8b065f26`.
- The payload step rejects the kernel: check that `kernel.macho` really is the
  prelinked output, not the unstripped kernel.
