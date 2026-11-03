// SPDX-License-Identifier: BSD-2-Clause-Patent
// Observe the existing source IOUserClient dispatch, without changing it or
// moving a signed vtable. Both callbacks run outside the client's IPC lock.
#include <IOKit/IOUserClient.h>
#include <IOKit/IOLib.h>
#include <pexpert/pexpert.h>
#include <sys/proc.h>
#include "LegacyDisplayOffsets27.h"

extern volatile uintptr_t native_legacy_object;
extern volatile uint32_t native_legacy_swap_validation_calls;
extern volatile uint32_t native_legacy_transfer_calls;
extern "C" unsigned native_legacy_vsync_report(char *, unsigned);

extern "C" void
native_legacy_display_sample27(const char *reason)
{
    const auto display = reinterpret_cast<const unsigned char *>(native_legacy_object);
    if (!display) return;
    const void *surface = nullptr, *current = nullptr, *head = nullptr, *vsync = nullptr;
    uint32_t width = 0, height = 0, requested = 0, powered = 0;
    memcpy(&surface, display + UKE_DISPLAY_DEFAULT_SURFACE, sizeof(surface));
    memcpy(&current, display + UKE_DISPLAY_CURRENT, sizeof(current));
    memcpy(&head, display + UKE_DISPLAY_HEAD, sizeof(head));
    memcpy(&vsync, display + UKE_DISPLAY_VSYNC_CLIENTS, sizeof(vsync));
    memcpy(&width, display + UKE_DISPLAY_WIDTH, sizeof(width));
    memcpy(&height, display + UKE_DISPLAY_HEIGHT, sizeof(height));
    memcpy(&requested, display + UKE_DISPLAY_REQUESTED, sizeof(requested));
    memcpy(&powered, display + UKE_DISPLAY_POWERED, sizeof(powered));
    // A concurrent sample is observational; pointers are never followed here.
    printf("UKE DISP state %s default=%p current=%p head=%p vsync=%p dims=%ux%u requested=%u powered=%u validate=%u transfer=%u\n",
        reason, surface, current, head, vsync, width, height,
        requested, powered, native_legacy_swap_validation_calls, native_legacy_transfer_calls);
    char text[160];
    native_legacy_vsync_report(text, sizeof(text));
    printf("UKE DISP %s\n", text);
}

namespace {
constexpr unsigned kClients = 16, kSelectors = 101, kBudget = 160;
struct ClientCounts {
    uintptr_t object;
    uint32_t calls[kSelectors][2];
};
ClientCounts clients[kClients];
uint32_t printed;
bool enabled()
{
    // Do not cache a pre-BSD failure to parse the boot argument.
    unsigned value = 0;
    return PE_parse_boot_argn("uke-display-trace", &value, sizeof(value)) && value == 1;
}
}

extern "C" void
native_legacy_method_trace(IOUserClient *client, uint32_t selector,
    const IOExternalMethodArguments *args, uint32_t result, unsigned phase)
{
    if (selector >= kSelectors || phase > 1 || !enabled() || !client ||
        !client->metaCast("IOMobileFramebufferUserClient")) return;
    ClientCounts *counts = nullptr;
    const auto object = reinterpret_cast<uintptr_t>(client);
    for (auto &row : clients) {
        auto at = __atomic_load_n(&row.object, __ATOMIC_RELAXED);
        if (!at) __atomic_compare_exchange_n(&row.object, &at, object, false,
            __ATOMIC_RELAXED, __ATOMIC_RELAXED);
        if (__atomic_load_n(&row.object, __ATOMIC_RELAXED) == object) {
            counts = &row;
            break;
        }
    }
    if (!counts) return;
    const unsigned count = __atomic_fetch_add(&counts->calls[selector][phase], 1u, __ATOMIC_RELAXED);
    // Keep each notification type's registration: selector72 starts with
    // types0/9, so two calls alone would hide the later vsync registration.
    // Other selectors retain two pairs and the same global call-line cap.
    const unsigned limit = selector == 72 ? 12 : 2;
    if (count >= limit || __atomic_fetch_add(&printed, 1u, __ATOMIC_RELAXED) >= kBudget) return;
    uint64_t input[3] = {}, output = 0;
    if (args && args->scalarInput) {
        unsigned n = args->scalarInputCount;
        if (n > 3) n = 3;
        memcpy(input, args->scalarInput, n * sizeof(input[0]));
    }
    if (phase && !result && args && args->scalarOutput && args->scalarOutputCount)
        output = args->scalarOutput[0];
    printf("UKE DISP call pid=%d client=%p sel=%u %s result=%x scalars=%u/%u struct=%u/%u desc=%u/%u in=%llx,%llx,%llx out=%llx\n",
        proc_selfpid(), client, selector, phase ? "return" : "enter", result,
        args ? args->scalarInputCount : 0, args ? args->scalarOutputCount : 0,
        args ? args->structureInputSize : 0, args ? args->structureOutputSize : 0,
        args && args->structureInputDescriptor ? 1u : 0u,
        args && args->structureOutputDescriptor ? 1u : 0u,
        input[0], input[1], input[2], output);
    if (phase && (selector == 3 || selector == 4 || selector == 5 || selector == 6 || selector == 72))
        native_legacy_display_sample27("method-return");
}
