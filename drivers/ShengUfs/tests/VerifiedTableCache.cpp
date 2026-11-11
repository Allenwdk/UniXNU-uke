// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host checks of the verified-root table page cache against vr_verify_block on the whole table.
#include <atomic>
#include <cassert>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>
#include <openssl/sha.h>
#include "verified_root.h"
#include "../VerifiedTableCache.h"

static int sha (const void *data, size_t bytes, uint8_t out[32]) { SHA256 (static_cast<const unsigned char *> (data), bytes, out); return 1; }
static int noRsa (const uint8_t *, const uint8_t *, const uint8_t *) { return 0; }
static const vr_crypto kCrypto = { sha, noRsa };

// A disk holding one window's table object (whole 4096-byte blocks, the tail of the last one filled with unrelated bytes).
struct Disk {
  std::vector<uint8_t> blocks;
  std::atomic<unsigned> reads {0};
  std::atomic<int> failRead {-1};          // page whose reads fail
  unsigned delayUs = 0;
};

struct Ops {
  std::mutex &mutex;
  std::condition_variable &changed;
  Disk &disk;
  std::unique_lock<std::mutex> *held = nullptr;
  std::unique_lock<std::mutex> guard {mutex, std::defer_lock};
  Ops (std::mutex &m, std::condition_variable &c, Disk &d) : mutex(m), changed(c), disk(d) {}
  void lock () { guard.lock (); }
  void unlock () { guard.unlock (); }
  void sleep (void *) { changed.wait (guard); }
  void wakeup (void *) { changed.notify_all (); }
  bool readPage (const VrootTable &table, uint64_t page, void *dst) {
    assert (!guard.owns_lock ());           // never read under the cache lock
    if (disk.delayUs) std::this_thread::sleep_for (std::chrono::microseconds (disk.delayUs));
    if (int (page) == disk.failRead.load ()) return false;
    const uint64_t at = (table.firstLba + page) * 4096;
    assert (at + 4096 <= disk.blocks.size ());
    memcpy (dst, disk.blocks.data () + at, 4096);
    ++disk.reads;
    return true;
  }
  bool sha256 (const void *data, size_t bytes, uint8_t out[32]) { return sha (data, bytes, out) != 0; }
};

struct Window {
  std::vector<uint8_t> image, hashes, pageHashes;
  std::vector<uint32_t> where;
  vr_view whole {}, cached {};
  VrootTable table {};
  Window (uint64_t imageBytes, Disk &disk, uint64_t firstLba, unsigned seed) : image(imageBytes) {
    for (size_t i = 0; i < image.size (); ++i) image[i] = uint8_t (i * 31 + i / 4096 + seed);
    const uint64_t count = (imageBytes + 4095) / 4096;
    hashes.resize (count * 32);
    for (uint64_t b = 0; b < count; ++b) {
      const uint64_t n = imageBytes - b * 4096 < 4096 ? imageBytes - b * 4096 : 4096;
      sha (image.data () + b * 4096, n, hashes.data () + b * 32);
    }
    const uint64_t pages = (hashes.size () + 4095) / 4096;
    if (disk.blocks.size () < (firstLba + pages) * 4096) disk.blocks.resize ((firstLba + pages) * 4096, 0xee);
    memcpy (disk.blocks.data () + firstLba * 4096, hashes.data (), hashes.size ());
    pageHashes.resize (pages * 32);
    for (uint64_t p = 0; p < pages; ++p) {
      const uint64_t n = hashes.size () - p * 4096 < 4096 ? hashes.size () - p * 4096 : 4096;
      sha (hashes.data () + p * 4096, n, pageHashes.data () + p * 32);
    }
    where.assign (pages, 0);
    whole.hashes = hashes.data (); whole.image_bytes = imageBytes; whole.block_count = count; whole.generation = 1;
    cached = whole; cached.hashes = nullptr;
    table = { pageHashes.data (), where.data (), hashes.size (), pages, firstLba };
  }
  size_t length (uint64_t b) const { return image.size () - b * 4096 < 4096 ? image.size () - b * 4096 : 4096; }
};

int main ()
{
  std::mutex mutex;
  std::condition_variable changed;
  Disk disk;
  // OS-like window: 3 table pages, the last one partial (300 blocks = 9600 table bytes); image ends in a partial block.
  Window a (300 * 4096 - 1000, disk, 2, 1);
  // A second window sharing the cache: one partial table page.
  Window b (40 * 4096, disk, 10, 7);
  std::vector<VrootCacheEntry> entries (2);
  std::vector<uint8_t> pages (entries.size () * 4096);
  VrootTableCache cache = { pages.data (), entries.data (), uint32_t (entries.size ()), 0, {} };
  Ops ops (mutex, changed, disk);

  // Same decision as vr_verify_block on the whole table, for every block, and the tail check of the partial last block.
  for (uint64_t blk = 0; blk < a.whole.block_count; ++blk) {
    const void *data = a.image.data () + blk * 4096;
    assert (vr_verify_block (&a.whole, blk, data, a.length (blk), &kCrypto) == VR_OK);
    assert (vrootVerifyBlock (cache, ops, a.table, a.cached, blk, data, a.length (blk), &kCrypto) == VR_OK);
    assert (vrootVerifyBlock (cache, ops, a.table, a.cached, blk, data, 4096 + (blk == a.whole.block_count - 1 ? 0 : 1),
                              &kCrypto) == VR_BLOCK);
  }
  assert (cache.stats.misses == 3 && cache.stats.reads == 3 && cache.stats.evictions == 1 && !cache.stats.failures);
  // Out of range: refused by both.
  assert (vrootVerifyBlock (cache, ops, a.table, a.cached, a.whole.block_count, a.image.data (), 4096, &kCrypto) == VR_BLOCK);
  // A tampered data block is refused exactly as now.
  {
    std::vector<uint8_t> bad (a.image.begin () + 200 * 4096, a.image.begin () + 201 * 4096);
    bad[17] ^= 1;
    assert (vr_verify_block (&a.whole, 200, bad.data (), 4096, &kCrypto) == VR_BLOCK);
    assert (vrootVerifyBlock (cache, ops, a.table, a.cached, 200, bad.data (), 4096, &kCrypto) == VR_BLOCK);
  }
  // LRU: pages 1 and 2 of window a are cached; touch 2, load b's page 0, so page 1 goes.
  {
    const uint64_t misses = cache.stats.misses;
    assert (vrootVerifyBlock (cache, ops, a.table, a.cached, 260, a.image.data () + 260 * 4096, 4096, &kCrypto) == VR_OK);
    assert (cache.stats.misses == misses);
    assert (vrootVerifyBlock (cache, ops, b.table, b.cached, 3, b.image.data () + 3 * 4096, 4096, &kCrypto) == VR_OK);
    assert (cache.stats.misses == misses + 1 && a.where[1] == 0 && a.where[2] != 0 && b.where[0] != 0);
  }
  // A tampered table page on disk: every block it covers is refused, nothing is cached, and each request reads it again.
  {
    uint8_t *page0 = disk.blocks.data () + a.table.firstLba * 4096;
    page0[40] ^= 0x80;                       // entry of block 1
    const uint64_t failures = cache.stats.failures, reads = disk.reads;
    for (int round = 0; round < 2; ++round) {
      assert (vrootVerifyBlock (cache, ops, a.table, a.cached, 1, a.image.data () + 4096, 4096, &kCrypto) == VR_HASH_TABLE);
      assert (vrootVerifyBlock (cache, ops, a.table, a.cached, 0, a.image.data (), 4096, &kCrypto) == VR_HASH_TABLE);
    }
    assert (cache.stats.failures == failures + 4 && disk.reads == reads + 4 && a.where[0] == 0);
    // A table page forged to match a forged block is refused too: the kept page hash, not the page, decides.
    std::vector<uint8_t> forged (4096, 0x42);
    sha (forged.data (), 4096, page0 + 32);
    assert (vrootVerifyBlock (cache, ops, a.table, a.cached, 1, forged.data (), 4096, &kCrypto) == VR_HASH_TABLE);
    page0[40] ^= 0x80;
    memcpy (page0 + 32, a.hashes.data () + 32, 32);
    assert (vrootVerifyBlock (cache, ops, a.table, a.cached, 1, a.image.data () + 4096, 4096, &kCrypto) == VR_OK);
    // Bytes after the table in its last block are not part of the table and do not matter.
    disk.blocks[(a.table.firstLba + 2) * 4096 + 4000] ^= 0xff;
    assert (vrootVerifyBlock (cache, ops, a.table, a.cached, 299, a.image.data () + 299 * 4096, a.length (299), &kCrypto) == VR_OK);
  }
  // A read error refuses the block and caches nothing.
  {
    VrootTableCache fresh = cache;
    for (auto &e : entries) e = {};
    for (auto &w : a.where) w = 0;
    for (auto &w : b.where) w = 0;
    fresh.stats = {};
    disk.failRead = 1;
    assert (vrootVerifyBlock (fresh, ops, a.table, a.cached, 130, a.image.data () + 130 * 4096, 4096, &kCrypto) == VR_HASH_TABLE);
    assert (fresh.stats.failures == 1 && fresh.stats.reads == 0 && a.where[1] == 0);
    disk.failRead = -1;
    assert (vrootVerifyBlock (fresh, ops, a.table, a.cached, 130, a.image.data () + 130 * 4096, 4096, &kCrypto) == VR_OK);
    cache = fresh;
  }
  // Drop forgets a table's pages and leaves the other's.
  vrootTableDrop (cache, ops, a.table);
  for (auto w : a.where) assert (!w);
  for (auto &e : entries) assert (e.table != &a.table);
  // Concurrency: more threads than entries, slow reads; every block verifies and fills wait instead of reading twice at once.
  {
    for (auto &e : entries) e = {};
    for (auto &w : a.where) w = 0;
    for (auto &w : b.where) w = 0;
    cache.stats = {};
    disk.delayUs = 200;
    std::atomic<unsigned> bad {0};
    std::vector<std::thread> threads;
    for (unsigned t = 0; t < 8; ++t) {
      threads.emplace_back ([&, t] {
        Ops mine (mutex, changed, disk);
        for (unsigned i = 0; i < 200; ++i) {
          Window &w = (i + t) % 3 ? a : b;
          const uint64_t blk = (i * 37 + t * 11) % w.whole.block_count;
          if (vrootVerifyBlock (cache, mine, w.table, w.cached, blk, w.image.data () + blk * 4096, w.length (blk), &kCrypto) != VR_OK)
            ++bad;
        }
      });
    }
    for (auto &t : threads) t.join ();
    assert (!bad && !cache.stats.failures);
    assert (cache.stats.hits + cache.stats.misses == 1600);
    for (auto &e : entries) assert (e.state != kVrootFilling);
    printf ("concurrent: hits=%llu misses=%llu reads=%llu evictions=%llu waits=%llu\n",
            (unsigned long long) cache.stats.hits, (unsigned long long) cache.stats.misses,
            (unsigned long long) cache.stats.reads, (unsigned long long) cache.stats.evictions,
            (unsigned long long) cache.stats.waits);
  }
  puts ("PASS: verified table cache");
  return 0;
}
