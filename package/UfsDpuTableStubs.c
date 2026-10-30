/* SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 * Stage-1 table accessors of the display bank's window. ShengUfs's source API exposes the mapping call
 * (native_ufs_dpu_map) but not these two read-only diagnostics; the DpuProbe takes their addresses through
 * authenticated pointers, which the chained-fixup prelink needs defined, so they are always provided.
 */
#include <stdint.h>

uint32_t native_ufs_dpu_table(unsigned index) { (void)index; return 0; }
uint32_t native_ufs_dpu_l2(unsigned index) { (void)index; return 0; }
