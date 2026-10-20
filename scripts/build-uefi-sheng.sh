#!/usr/bin/env bash
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$ROOT"
python3 scripts/check-sheng.py
python3 - <<'PY'
import json
from pathlib import Path
if json.loads(Path('build/sheng/check.json').read_text())['missing_vendor_inputs']:
    raise SystemExit('Import verified sheng firmware inputs before building')
PY
export WORKSPACE=$ROOT/uefi-sheng
export EDK_TOOLS_PATH=$ROOT/crabapple/Mu_Basecore/BaseTools
export CONF_PATH=$ROOT/.cache/uefi-sheng-conf
export CLANG_BIN=${CLANG_BIN:-$ROOT/.cache/toolchain/}
export PYTHON_COMMAND=python3
export PACKAGES_PATH=$ROOT/uefi-sheng:$ROOT/uefi-sheng/Platforms/Xiaomi:$ROOT/crabapple:$ROOT/drivers/DarwinBoot:$ROOT/crabapple/Mu_Basecore:$ROOT/uefi-sheng/Common/Mu:$ROOT/uefi-sheng/Common/Mu_OEM_Sample:$ROOT/uefi-sheng/Silicon/Qualcomm:$ROOT/uefi-sheng/Silicon/Silicium:$ROOT/uefi-sheng/Silicium-ACPI:$ROOT/uefi-sheng/Silicium-ACPI/Platforms/Xiaomi:$ROOT/uefi-sheng/Silicium-ACPI/Silicon/Qualcomm
export PATH=$EDK_TOOLS_PATH/BinWrappers/PosixLike:$EDK_TOOLS_PATH/Source/C/bin:$PATH
mkdir -p "$CONF_PATH"
for NAME in target tools_def build_rule; do
  if [ ! -f "$CONF_PATH/$NAME.txt" ]; then
    cp "$EDK_TOOLS_PATH/Conf/$NAME.template" "$CONF_PATH/$NAME.txt"
  fi
done
# Board patches for the shared Mu base checkout. They are applied only for the
# build and always reverted, so the checkout stays clean (as upstream does).
MU_BASE=$ROOT/crabapple/Mu_Basecore
APPLIED_PATCHES=()
revert_patches() {
  for PATCH in "${APPLIED_PATCHES[@]:-}"; do
    [ -n "$PATCH" ] && git -C "$MU_BASE" apply --reverse "$PATCH"
  done
  return 0
}
trap revert_patches EXIT
for PATCH in "$ROOT/uefi-sheng/Resources/MuPatches/Gic-Spurious.patch"; do
  git -C "$MU_BASE" apply --check "$PATCH"
  git -C "$MU_BASE" apply "$PATCH"
  APPLIED_PATCHES+=("$PATCH")
done
cd "$WORKSPACE"
build -p Platforms/Xiaomi/shengPkg/sheng.dsc -a AARCH64 -b "${TARGET:-DEBUG}" \
  -t CLANGPDB -n "${JOBS:-4}" -D FD_BASE=0xA7100000 -D FD_SIZE=0x300000 \
  -D FD_BLOCKS=768 -D ENABLE_SECUREBOOT=1 -D DEVICE_MODEL=sheng \
  -D MEMORY_PROTECTION=TRUE -D SHIP_MODE=FALSE -D BUILDID_STRING=sheng-research
