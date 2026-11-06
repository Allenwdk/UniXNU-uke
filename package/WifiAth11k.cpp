// SPDX-License-Identifier: BSD-2-Clause-Patent
// Board callbacks for the ported ath11k AHB stack. The driver owns the QMI/WMI
// protocol; this file only supplies what XNU has no platform bus for: a
// persistent physical mapping, the QMI BAR base, a MAC, and the GLINK/WPSS
// start hooks. The ath11k path is opt-in (uke-wifi=3) until it is validated.
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IODeviceMemory.h>
#include <pexpert/pexpert.h>
#include <sys/errno.h>

namespace {
constexpr uint64_t kWlanBarBase = 0x61e00000ULL, kWlanBarBytes = 0x200000ULL;
struct Mapping {uint64_t base, bytes; IOMemoryMap *map; volatile uint8_t *va;};
Mapping gMappings[8];
unsigned gMappingCount;

void *mapPersistent(uint64_t pa, size_t bytes) {
    for (unsigned i = 0; i < gMappingCount; ++i) {
        const Mapping &m = gMappings[i];
        if (pa >= m.base && pa + bytes <= m.base + m.bytes) return const_cast<uint8_t *>(m.va) + (pa - m.base);
    }
    if (gMappingCount >= 8) return nullptr;
    const uint64_t base = pa & ~0xfffULL, offset = pa - base;
    const uint64_t span = (offset + bytes + 0xfff) & ~0xfffULL;
    IODeviceMemory *memory = IODeviceMemory::withRange(base, span);
    if (!memory) return nullptr;
    IOMemoryMap *map = memory->map(kIOMapAnywhere | kIOMapInhibitCache);
    memory->release();
    if (!map) return nullptr;
    Mapping &m = gMappings[gMappingCount++];
    m.base = base; m.bytes = span; m.map = map;
    m.va = reinterpret_cast<volatile uint8_t *>(map->getVirtualAddress());
    return const_cast<uint8_t *>(m.va) + offset;
}
}  // namespace

extern "C" void *nw_board_map_phys(uint64_t pa, size_t bytes) {return mapPersistent(pa, bytes);}
extern "C" void *nw_board_bar(void) {return mapPersistent(kWlanBarBase, kWlanBarBytes);}
extern "C" int nw_board_mac(unsigned char *address) {
    // Locally administered unicast address; the firmware reports a zero MAC.
    static const unsigned char mac[6] = {0x02, 0x00, 0x00, 0x75, 0x67, 0x50};
    if (!address) return -EINVAL;
    for (unsigned i = 0; i < 6; ++i) address[i] = mac[i];
    return 0;
}
// The ath11k QRTR path is not wired to the SMEM GLINK channel yet: the stack
// links and probes, but mode 3 still refuses to start the endpoint.
extern "C" int nw_board_glink_xmit(const void *packet, size_t length) {
    (void)packet; (void)length;
    return -ENODEV;
}
extern "C" int nw_board_wpss_start(void) {
    return -ENODEV;  // the existing WPSS worker owns the start sequence for now
}
extern "C" int nw_ahb_start(void);
extern "C" int uke_wifi_ath11k_start(void) {return nw_ahb_start();}
