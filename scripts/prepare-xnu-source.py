#!/usr/bin/env python3
"""Restore the DriverKit and TrustCache headers newer xnu omits, without the API.

This overrides UniXNU's `scripts/prepare-xnu-source.py`. The original asks
api.github.com for a directory listing and then downloads every header from
raw.githubusercontent.com. Both are unreliable from CI and from the tablet:

  - on the tablet raw.githubusercontent.com resets every connection;
  - on GitHub Actions the unauthenticated API returns HTTP 403 "rate limit
    exceeded", because the runner's address is shared.

The header set is derivable from the pinned source itself, so this version uses
git only: it fetches the two older tags named by feeds.toml into the xnu feed
checkout, lists the files with `git ls-tree`, and writes each one out with
`git show`. Same inputs, same outputs, no network beyond github.com.

The destination paths and the tag defaults are the ones the original uses, so
nothing else in the build has to change.
"""
from pathlib import Path
import hashlib
import json
import os
import subprocess
import sys

# feeds.toml sets these for the driver; keep the same defaults.
DRIVERKIT_TAG = os.environ.get('UNIXNU_DRIVERKIT_TAG', 'xnu-10063.121.3')
TRUSTCACHE_TAG = os.environ.get('UNIXNU_TRUSTCACHE_TAG', 'xnu-8796.101.5')

# (tag, directory inside the repository, destination inside .cache/xnu-work)
RESTORES = [
    (DRIVERKIT_TAG, 'iokit/DriverKit', '.cache/xnu-work/iokit/DriverKit'),
    (DRIVERKIT_TAG, 'iokit/DriverKit/crypto', '.cache/xnu-work/iokit/DriverKit/crypto'),
    (TRUSTCACHE_TAG, 'EXTERNAL_HEADERS/TrustCache', '.cache/xnu-work/EXTERNAL_HEADERS/TrustCache'),
]
MANIFEST = Path('.cache/sources/xnu-header-restore.json')


def find_feed():
    """The xnu feed checkout: out/feeds/xnu-<hash>, two levels above the tree."""
    if not Path('.cache/xnu-work').is_dir():
        sys.exit('Create the local XNU work source before preparing headers')
    for candidate in ('../../feeds', 'out/feeds', 'feeds'):
        feeds = sorted(Path(candidate).glob('xnu-*'))
        if feeds:
            return feeds[0]
    sys.exit('no xnu feed checkout found')


def git(repo, *arguments, check=True):
    result = subprocess.run(['git', '-C', str(repo), *arguments], capture_output=True)
    if check and result.returncode != 0:
        sys.exit(f'git {" ".join(arguments)} failed: {result.stderr.decode()[:300]}')
    return result


def ensure_tag(repo, tag):
    if git(repo, 'rev-parse', '--verify', '-q', f'{tag}^{{commit}}', check=False).returncode == 0:
        return
    print(f'  fetching tag {tag}')
    git(repo, 'fetch', '-q', '--depth', '1', 'origin', 'tag', tag)


def list_headers(repo, tag, directory):
    names = git(repo, 'ls-tree', '-r', '--name-only', tag, directory).stdout.decode().splitlines()
    return [name for name in names if name.endswith('.h')]


def main():
    repo = find_feed()
    print(f'prepare-xnu-source: feed {repo}')
    restored = []
    for tag, directory, destination in RESTORES:
        ensure_tag(repo, tag)
        headers = list_headers(repo, tag, directory)
        target_dir = Path(destination)
        written = 0
        for path in headers:
            name = path[len(directory) + 1:]
            target = target_dir / name
            if target.exists():
                continue
            data = git(repo, 'show', f'{tag}:{path}').stdout
            if not data:
                sys.exit(f'{tag}:{path} is empty')
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(data)
            written += 1
            restored.append({
                'path': str(target),
                'source': f'{tag}:{path}',
                'sha256': hashlib.sha256(data).hexdigest(),
            })
        print(f'  {directory} @ {tag}: {len(headers)} headers, {written} written')

    MANIFEST.parent.mkdir(parents=True, exist_ok=True)
    MANIFEST.write_text(json.dumps({'headers': restored}, indent=2) + '\n')
    print(f'prepare-xnu-source: {len(restored)} headers restored, manifest {MANIFEST}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
