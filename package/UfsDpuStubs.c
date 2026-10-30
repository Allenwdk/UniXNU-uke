/* SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 * The display bank's stage-1 window mapping call, when the source storage (ShengUfs) is not built. The callers
 * reach it through an authenticated function pointer (weak_import), and the linker keeps an import entry for
 * such a reference even with a weak definition in a force-loaded archive; the chained-fixup prelink refuses
 * imports, so the no-storage build defines it outright. With ShengUfs linked, this file is not compiled.
 */
#include <stdint.h>

uint32_t native_ufs_dpu_map(unsigned slot, const uint64_t *pages, unsigned count)
{
	(void)slot; (void)pages; (void)count;
	return 0;
}
