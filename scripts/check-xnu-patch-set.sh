#!/usr/bin/env bash
# Apply the exact XNU patch series to a clean source tree. The kernel build
# uses the same order (audit first, then the fork set, then the device set);
# this check catches an order or context conflict on a Linux runner in about a
# minute instead of after a full macOS kernel build.
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
FORK_PATCHES=${UNIXNU_FORK_PATCHES:-$ROOT/../UniXNU/patches/xnu}
DEVICE_PATCHES=$ROOT/patches/xnu
KIT_PATCHES=$ROOT/kits/NativePrelink/patches
if [ ! -d "$KIT_PATCHES" ]; then
  KIT_PATCHES=${UNIXNU_KIT_PATCHES:-$ROOT/../UniXNU/kits/NativePrelink/patches}
fi
XNU_TAG=${XNU_TAG:-xnu-10063.141.1}
XNU_COMMIT=${XNU_COMMIT:-}
if [ -z "$XNU_COMMIT" ] && [ -f "$ROOT/unixnu.lock" ]; then
  XNU_COMMIT=$(python3 - "$ROOT/unixnu.lock" <<'PY'
import sys
import tomllib
print(tomllib.load(open(sys.argv[1], 'rb'))['source']['xnu']['commit'])
PY
)
fi
XNU_COMMIT=${XNU_COMMIT:-d8b80295118ef25ac3a784134bcf95cd8e88109f}

if [ ! -d "$FORK_PATCHES" ]; then
  echo "fork patch directory not found: $FORK_PATCHES" >&2
  exit 1
fi
if [ ! -d "$DEVICE_PATCHES" ]; then
  echo "device patch directory not found: $DEVICE_PATCHES" >&2
  exit 1
fi

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
if [ -n "${XNU_SOURCE:-}" ]; then
  git clone -q --shared "$XNU_SOURCE" "$WORK/xnu"
else
  git clone -q --depth 1 --branch "$XNU_TAG" https://github.com/apple-oss-distributions/xnu.git "$WORK/xnu"
fi
ACTUAL=$(git -C "$WORK/xnu" rev-parse HEAD)
if [ "$ACTUAL" != "$XNU_COMMIT" ]; then
  echo "unexpected XNU revision: $ACTUAL (expected $XNU_COMMIT)" >&2
  exit 1
fi

apply() {
  local patch=$1
  local name
  local error
  name=$(basename "$patch")
  if ! error=$(git -C "$WORK/xnu" apply --check "$patch" 2>&1); then
    echo "FAIL: $name" >&2
    printf '%s\n' "$error" | sed 's/^/  /' >&2
    return 1
  fi
  git -C "$WORK/xnu" apply "$patch"
  echo "PASS: $name"
}

AUDIT=$FORK_PATCHES/offline-owner-root-binding-audit.patch
if [ -f "$AUDIT" ]; then
  apply "$AUDIT"
fi
for directory in "$FORK_PATCHES" "$DEVICE_PATCHES" "$KIT_PATCHES"; do
  [ -d "$directory" ] || continue
  for patch in $(find "$directory/" -maxdepth 1 -name '*.patch' -print 2>/dev/null | sort); do
    if [ "$patch" = "$AUDIT" ]; then
      continue
    fi
    apply "$patch"
  done
done
echo "PASS: the patch set applies cleanly to $XNU_COMMIT"
