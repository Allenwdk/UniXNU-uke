/* SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 * Weak stand-ins for the source HID driver while it is not built in the CPU-rendering bring-up.
 * The display probe and the HID probe reference these; a weak definition keeps the archive linkable and
 * is replaced as soon as component:ShengHidInput is built (it needs the DriverKit HID headers, which
 * the Linux SDK here does not carry).
 */
#include <stdbool.h>
#include <stdint.h>

typedef struct IOService IOService;

__attribute__((weak)) volatile uint32_t native_hid_input_events[8];
__attribute__((weak)) volatile uint32_t native_hid_input_results[8];
__attribute__((weak)) volatile uint32_t native_hid_stage, native_hid_resource_found;
__attribute__((weak)) volatile int native_hid_load_status = -1;
__attribute__((weak)) volatile int native_hid_matching_load_status = -1;
__attribute__((weak)) volatile int native_hid_input_meta_status = -1;
__attribute__((weak)) volatile uint32_t native_hid_class_sizes[5];
__attribute__((weak)) volatile uint64_t native_hid_input_object;

__attribute__((weak)) void native_hid_static_init(void) {}
__attribute__((weak)) IOService *native_hid_input_allocate(void) { return 0; }
__attribute__((weak)) IOService *native_hid_touch_allocate(void) { return 0; }

/* HidInputProbe calls the source driver's static initializer through its asm label. */
void native_hid_static_init_label(void) __asm__("__GLOBAL__sub_I_NativeHidInput.cpp");
void native_hid_static_init_label(void) {}

/* The display bank's stage-1 window belongs to ShengUfs (source storage); the probe and the backend
 * read its state, and the direct path stays off while the window is absent. */
__attribute__((weak)) volatile uint32_t native_ufs_dpu_state;
__attribute__((weak)) volatile uint32_t native_ufs_dpu_smmu[24];
__attribute__((weak)) volatile uint32_t native_ufs_dpu_check[8];
__attribute__((weak)) volatile uint32_t native_ufs_dpu_atos[4];
__attribute__((weak)) volatile uint32_t native_ufs_dpu_fault_sid;
/* Strong: the callers reach these through authenticated function pointers (weak_import), and the linker keeps
 * an import entry for a weak_import reference even when a weak definition sits in a force-loaded archive.
 * The chained-fixup prelink refuses import entries, so the no-storage build defines them outright. */
uint32_t native_ufs_dpu_map(unsigned slot, const uint64_t *pages, unsigned count)
{
	(void)slot; (void)pages; (void)count;
	return 0;
}
uint32_t native_ufs_dpu_table(unsigned index) { (void)index; return 0; }
uint32_t native_ufs_dpu_l2(unsigned index) { (void)index; return 0; }
__attribute__((weak)) volatile uint32_t native_touch_edge_stats[4];
