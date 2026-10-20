#!/usr/bin/env bash
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$ROOT"
export WORKSPACE=$ROOT
export PACKAGES_PATH=$ROOT:$ROOT/crabapple/Mu_Basecore
export EDK_TOOLS_PATH=$ROOT/crabapple/Mu_Basecore/BaseTools
export CONF_PATH=$ROOT/crabapple/Build/Conf
export CLANG_BIN=${CLANG_BIN:-$ROOT/.cache/toolchain/}
export PYTHON_COMMAND=python3
export PATH=$EDK_TOOLS_PATH/BinWrappers/PosixLike:$EDK_TOOLS_PATH/Source/C/bin:$PATH
build -p drivers/ShengPreflight/ShengPreflight.dsc -a AARCH64 -b DEBUG -t CLANGPDB -n "${JOBS:-4}"
