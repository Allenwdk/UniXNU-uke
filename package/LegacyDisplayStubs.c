/* SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 * Weak fallbacks for the sheng legacy display probes when the first-console
 * configuration disables the BootProgress layer and the Adreno GPU probe.
 * The full sheng configuration links the real strong definitions; a weak
 * definition here is then discarded by the linker.
 *
 * boot_progress_prepare_scanout returns 1 in the real layer when boot
 * progress is disabled, which is the "no diagnostic writer" answer the
 * display probe expects.
 */
#include <stdint.h>

__attribute__((weak)) int
boot_progress_pulse_anchor (uintptr_t base, uint64_t bytes, unsigned width,
                            unsigned height, unsigned row_bytes)
{
  (void)base;
  (void)bytes;
  (void)width;
  (void)height;
  (void)row_bytes;
  return 0;
}

__attribute__((weak)) int
boot_progress_prepare_scanout (void)
{
  return 1;
}

__attribute__((weak)) void
boot_progress_cancel_scanout (void)
{
}

__attribute__((weak)) void
boot_progress_stage (const char *text)
{
  (void)text;
}

__attribute__((weak)) char boot_progress_ext[8][48];
__attribute__((weak)) char boot_progress_ext_wide[2][112];

__attribute__((weak)) void
sheng_gpu_power_probe (void)
{
}
