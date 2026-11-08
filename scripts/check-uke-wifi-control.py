#!/usr/bin/env python3
"""Host check for the native control socket callbacks (WifiAth11k.cpp).

The board ports sheng's controlConnect/controlDisconnect verbatim. Both run on
XNU's socket threads, so the failure paths matter more than the happy path: a
failing IOMalloc, a busy session slot, and the disconnect XNU delivers after a
failed connect (with no client context). This check slices those two production
functions out of the board source, substitutes the kernel allocator/lock and the
module's attach/detach, and asserts the ownership and lock balance.

Run: python3 scripts/check-uke-wifi-control.py [--report PATH]
"""
import argparse
import hashlib
import json
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'package/WifiAth11k.cpp'
MODULE = ROOT.parent / 'UniXNU/drivers/Ath12kWifi'
BEGIN = 'struct ControlClient {uint64_t session; uint32_t unit; unsigned rxShown, dcShown;};'
END = 'int controlReadMbuf(void *source'

HARNESS = r'''
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cstddef>
#include <cerrno>
#include "nw_control.h"

typedef void *kern_ctl_ref;
struct sockaddr_ctl { uint32_t sc_unit; };
struct IOLock { int depth; };
typedef int errno_t;

// ---- kernel allocator and lock, with accounting ----
static int gAllocated, gFreed, gLockDepth, gDetaches, gAttachResult;
static uint64_t gAttachSession;
static bool gAllocateFails;
static void *IOMalloc(size_t n) { if (gAllocateFails) return nullptr; ++gAllocated; return std::malloc(n); }
static void IOFree(void *p, size_t) { if (p) ++gFreed; std::free(p); }
static void IOLockLock(IOLock *l) { ++l->depth; ++gLockDepth; }
static void IOLockUnlock(IOLock *l) { --l->depth; --gLockDepth; }

// ---- the module's session API ----
static int nw_wifi_control_attach(uint64_t *session) {
    if (gAttachResult) return gAttachResult;
    *session = gAttachSession;
    return NW_CONTROL_OK;
}
static void nw_wifi_control_detach(uint64_t session) { (void)session; ++gDetaches; }

// ---- production callbacks, sliced verbatim from package/WifiAth11k.cpp ----
// __SLICED__
// ---- end of sliced production code ----

static int gFails;
static void check(const char *what, bool ok) {
    if (!ok) { gFails++; std::printf("FAIL %s\n", what); }
}

int main(void) {
    // controlStart() owns this lock in production, and it precedes every connect.
    gControlLock = new IOLock();

    // 1. Failed allocation: no session, no free, no detach.
    gAllocateFails = true;
    void *information = (void *)1;
    check("allocation failure returns ENOMEM", controlConnect(nullptr, nullptr, &information) == ENOMEM);
    gAllocateFails = false;
    check("allocation failure leaves no client", gControlClient == nullptr && gAllocated == 0 && gFreed == 0);
    check("allocation failure detaches nothing", gDetaches == 0);

    // 2. Busy session slot: the allocation is released and attach is not undone.
    gAttachResult = NW_CONTROL_BUSY;
    information = nullptr;
    sockaddr_ctl busy{7};
    check("busy attach maps to EWOULDBLOCK", controlConnect(nullptr, &busy, &information) == EWOULDBLOCK);
    check("busy attach releases the allocation", gAllocated == 1 && gFreed == 1 && information == nullptr);
    check("busy attach detaches nothing", gDetaches == 0);

    // 3. Successful connect: owner published with the session and unit.
    gAttachResult = NW_CONTROL_OK;
    gAttachSession = 42;
    sockaddr_ctl good{7};
    check("connect succeeds", controlConnect(nullptr, &good, &information) == 0);
    check("connect publishes the owner", information != nullptr && information == (void *)gControlClient);
    check("connect keeps the session", gControlClient->session == 42 && gControlClient->unit == 7);
    check("connect holds no lock", gLockDepth == 0);

    // 4. Disconnect after a failed connect (XNU calls it with no context).
    check("context-free disconnect returns 0", controlDisconnect(nullptr, 7, nullptr) == 0);
    check("context-free disconnect touches nothing", gDetaches == 0 && gFreed == 1 && gControlClient != nullptr);

    // 5. Real disconnect: one detach, owner cleared, allocation returned, lock balanced.
    check("disconnect returns 0", controlDisconnect(nullptr, 7, information) == 0);
    check("disconnect detaches exactly once", gDetaches == 1);
    check("disconnect clears the owner", gControlClient == nullptr);
    check("disconnect balances allocations", gAllocated == gFreed);
    check("disconnect leaves no lock held", gLockDepth == 0);

    if (gFails) { std::printf("%d control lifetime checks failed\n", gFails); return 1; }
    std::printf("PASS: failed allocation and busy session release their resources, a successful "
                "connect publishes session and unit, and disconnect detaches exactly once with "
                "balanced allocations and locks\n");
    return 0;
}
'''


def slice_source() -> str:
    text = SOURCE.read_text()
    start = text.index(BEGIN)
    end = text.index(END, start)
    return text[start:end]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument('--report')
    args = parser.parse_args()
    if not (MODULE / 'include/nw_control.h').is_file():
        print('module header not found: ' + str(MODULE / 'include/nw_control.h'))
        return 1
    sliced = slice_source()
    with tempfile.TemporaryDirectory() as tmp:
        harness = Path(tmp) / 'control.cpp'
        binary = Path(tmp) / 'control'
        harness.write_text(HARNESS.replace('// __SLICED__', sliced))
        build = subprocess.run(['clang++', '-std=c++17', '-O1', '-Wall', '-Wextra', '-Werror',
                                '-Wno-unused-parameter', '-Wno-format',
                                '-fsanitize=address,undefined', '-I', str(MODULE / 'include'),
                                str(harness), '-o', str(binary)],
                               text=True, capture_output=True)
        if build.returncode:
            print(build.stderr)
            return 1
        run = subprocess.run([str(binary)], text=True, capture_output=True)
        sys.stdout.write(run.stdout)
        sys.stderr.write(run.stderr)
        report = {
            'passed': run.returncode == 0,
            'source': str(SOURCE.relative_to(ROOT)),
            'source_sha256': hashlib.sha256(SOURCE.read_bytes()).hexdigest(),
            'xnu_allocator_and_socket_calls_simulated': True,
            'hardware_verified': False,
        }
        if args.report:
            Path(args.report).write_text(json.dumps(report, indent=1) + '\n')
        return run.returncode


if __name__ == '__main__':
    sys.exit(main())
