/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "AdrenoFirmware.h"
#include "AdrenoFirmwareInput.h"
#include <pexpert/pexpert.h>
#include <libkern/libkern.h>
#include <libkern/crypto/sha2.h>
#include <kern/kalloc.h>

#if DEVELOPMENT || DEBUG
extern const uint8_t native_adreno_firmware_image[];
extern const uint64_t native_adreno_firmware_image_size;
const uint8_t * const native_adreno_firmware_pointer = native_adreno_firmware_image;
volatile int native_adreno_firmware_status = -1;
volatile uint32_t native_adreno_firmware_records;
volatile uint32_t native_adreno_firmware_payload_bytes;
volatile uint32_t native_adreno_firmware_refusal_mask;
volatile uint32_t native_adreno_firmware_output_preserved;
uint8_t native_adreno_firmware_sha256[32];

__attribute__((noinline)) void
native_adreno_firmware_checkpoint(void)
{
  __asm__ volatile("" ::: "memory");
}

static bool image_matches(const uint8_t *image, size_t bytes, uint8_t digest[32])
{
  SHA256_CTX hash;
  SHA256_Init(&hash);
  SHA256_Update(&hash, image, bytes);
  SHA256_Final(digest, &hash);
  return bytes == ADRENO_FIRMWARE_INPUT_BYTES && !memcmp(digest, expected_gmu_sha256, 32);
}

void native_adreno_firmware_probe(void)
{
  int enabled = 0;
  if (!PE_parse_boot_argn("offline-adreno-firmware", &enabled, sizeof(enabled)) || !enabled) return;
  const size_t bytes = native_adreno_firmware_image_size;
  native_adreno_firmware_status = 1;
  if (!image_matches(native_adreno_firmware_image, bytes, native_adreno_firmware_sha256)) goto done;
  const AdrenoFirmwareRegion regions[] = {
    {0, 0x4000, 1}, {0x4000, 0xffc000, 2}, {0x10004000, 0x4000, 3}
  };
  AdrenoFirmwareBlock blocks[32], before[32];
  size_t count = 0;
  native_adreno_firmware_status = 2;
  if (AdrenoFirmwarePlanGmu(native_adreno_firmware_image, bytes, regions, 3, blocks, 32, &count) != AdrenoFirmwareSuccess || count != 15) goto done;
  native_adreno_firmware_records = (uint32_t)count;
  for (size_t i = 0; i < count; ++i) native_adreno_firmware_payload_bytes += blocks[i].bytes;
  if (native_adreno_firmware_payload_bytes != 66604) goto done;
  memset(blocks, 0xa5, sizeof(blocks));
  memcpy(before, blocks, sizeof(blocks));
  const AdrenoFirmwareResult expected[] = {AdrenoFirmwareTruncated, AdrenoFirmwareNoSpace, AdrenoFirmwareUnmapped};
  native_adreno_firmware_output_preserved = 1;
  for (unsigned i = 0; i < 3; ++i) {
    count = 123;
    AdrenoFirmwareResult result = AdrenoFirmwarePlanGmu(native_adreno_firmware_image, i == 0 ? bytes - 1 : bytes,
        regions, i == 2 ? 2 : 3, blocks, i == 1 ? 14 : 32, &count);
    if (result == expected[i] && count == 0) native_adreno_firmware_refusal_mask |= 1U << i;
    if (memcmp(before, blocks, sizeof(blocks))) native_adreno_firmware_output_preserved = 0;
  }
  uint8_t *copy = kalloc_data(bytes, Z_WAITOK);
  if (copy) {
    uint8_t digest[32];
    memcpy(copy, native_adreno_firmware_image, bytes);
    copy[bytes - 1] ^= 1;
    if (!image_matches(copy, bytes, digest)) native_adreno_firmware_refusal_mask |= 8;
    kfree_data(copy, bytes);
  }
  native_adreno_firmware_status = native_adreno_firmware_refusal_mask == 15 && native_adreno_firmware_output_preserved ? 0 : 3;
done:
  native_adreno_firmware_checkpoint();
}
#endif
