#!/usr/bin/env bash
# Build the uke first-console kernel, payload and boot image on macOS.
#
# Run from the UniXNU-uke device repository:
#   ./scripts/build-uke-macos.sh
#
# Environment overrides:
#   UNIXNU_CORE           core UniXNU checkout (default ../UniXNU)
#   UNIXNU_UEFI           uefi-uke checkout (default ../uefi-uke)
#   UNIXNU_DTB            stock vendor_boot DTB (default build/uke/uke.dtb)
#   UNIXNU_UNIQUE_CHIP_ID 8-byte platform seed (default: random)
set -euo pipefail

export PATH="/usr/local/bin:/opt/homebrew/bin:$PATH"
if command -v brew >/dev/null 2>&1; then
  eval "$(brew shellenv)"
  export PATH="$(brew --prefix)/opt/python@3.12/bin:$(brew --prefix)/opt/llvm/bin:$(brew --prefix)/opt/lld/bin:$PATH"
fi

DEVICE=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
CORE=${UNIXNU_CORE:-$DEVICE/../UniXNU}
UEFI=${UNIXNU_UEFI:-$DEVICE/../uefi-uke}
DTB=${UNIXNU_DTB:-$DEVICE/build/uke/uke.dtb}
TREE=$CORE/out/uke/tree

say() { printf '\n== %s\n' "$*"; }
need() {
  command -v "$1" >/dev/null 2>&1 || {
    printf 'missing %s; install Xcode and the Homebrew tools from docs/MACOS-BUILD.md\n' "$1" >&2
    exit 1
  }
}

for tool in xcodebuild xcrun python3 make git clang llvm-objcopy llvm-nm fdtput fdtget mformat mcopy; do
  need "$tool"
done
python3 - <<'PY'
import sys
if sys.version_info < (3, 11):
    raise SystemExit('Python 3.11 or newer is required')
PY
[ -f "$CORE/unixnu" ] || { echo "not a UniXNU checkout: $CORE" >&2; exit 1; }
[ -f "$UEFI/Resources/Configs/uke.toml" ] || { echo "not a uefi-uke checkout: $UEFI" >&2; exit 1; }
[ -f "$DTB" ] || {
  echo "missing $DTB; copy the stock vendor_boot DTB to UniXNU-uke/build/uke/uke.dtb" >&2
  exit 1
}

say "bootstrap configuration"
cd "$CORE"
./unixnu defconfig bootstrap_defconfig -D "$DEVICE"
./unixnu fetch -D "$DEVICE" xnu libplatform libdispatch AvailabilityVersions
./unixnu ipsw fetch -D "$DEVICE"
./unixnu tree -D "$DEVICE"

say "pinned corecrypto source"
mkdir -p "$TREE/refer"
if [ ! -d "$TREE/refer/corecrypto-2023/.git" ]; then
  git clone https://github.com/ONLYA/corecrypto-2023 "$TREE/refer/corecrypto-2023"
fi
git -C "$TREE/refer/corecrypto-2023" checkout 11fba65b12d8830630e248dfa8344caf8b065f26

say "bootstrap kernel and prelinked candidate"
./unixnu build kernel -D "$DEVICE"
mkdir -p "$TREE/build/native-prelink"
cp "$CORE/out/uke/kernel/kernel.macho" \
   "$TREE/build/native-prelink/native-owner-mobile-graphics-candidate.macho"

say "device archives"
cd "$TREE"
python3 scripts/gen-touch-firmware.py
python3 scripts/build-corecrypto-14.py --namespace
python3 scripts/build-arm-driver.py ArmDTPlatform
python3 scripts/build-arm-driver.py ArmCpu
python3 scripts/build-native-legacy-display.py

say "first-console kernel"
cd "$CORE"
./unixnu defconfig uke_defconfig -D "$DEVICE"
./unixnu build kernel -D "$DEVICE"

say "payload and boot image"
CHIP=${UNIXNU_UNIQUE_CHIP_ID:-$(python3 -c 'import secrets; print(hex(secrets.randbits(63) | 1))')}
./unixnu build payload -D "$DEVICE" --unique-chip-id "$CHIP"
./unixnu bootimg -D "$DEVICE" --uefi "$UEFI" --dtb "$DTB"

say "done"
printf 'kernel   %s\npayload  %s\nboot     %s\n' \
  "$CORE/out/uke/kernel/kernel.macho" \
  "$CORE/out/uke/payload/payload.img" \
  "$CORE/out/uke/boot/boot.img"
