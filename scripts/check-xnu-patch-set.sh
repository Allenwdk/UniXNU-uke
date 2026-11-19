#!/usr/bin/env bash
# Apply the selected build profile's ordered patches to a clean XNU checkout.
set -euo pipefail
device=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
core=$(cd "${UNIXNU_CORE:-$device/../UniXNU}" && pwd)
PYTHONPATH="$core/scripts" python3 - "$device" <<'PY'
from pathlib import Path
import os
import sys
import tempfile

from unixnu import feeds, patches
from unixnu.cli import parser
from unixnu.config import Context
from unixnu.util import Error, git, run

with tempfile.TemporaryDirectory(prefix='uke-patch-check-') as directory:
    args = parser().parse_args(['build', 'kernel', '-D', sys.argv[1], '--out', directory])
    ctx = Context(args)
    modules = feeds.resolve(ctx)
    xnu = modules[('refer', 'xnu')]
    revision = feeds.pinned_revision(ctx, xnu, feeds.lock_read(ctx))
    if not revision:
        raise Error('the selected XNU source must be pinned')
    if os.environ.get('XNU_SOURCE'):
        source = Path(os.environ['XNU_SOURCE']).resolve()
    else:
        feeds.fetch(ctx, modules, only={'xnu'})
        source = xnu.path
    work = Path(directory) / 'xnu'
    run(['git', 'clone', '--quiet', '--shared', '--no-checkout', source, work])
    git(work, 'checkout', '--quiet', '--detach', revision)
    selected = patches.plan(ctx, modules)
    for patch in selected:
        git(work, 'apply', str(patch))
        print(f'PASS: {patch.name}', flush=True)
    print(f'PASS: {len(selected)} patches apply cleanly to {revision}')
PY
