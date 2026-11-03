// SPDX-License-Identifier: BSD-2-Clause-Patent
// Allocation-free live checkpoints. The XNU-side diagnostic hook supplies a
// verified high-VA framebuffer alias, including across the TTBR1 replacement.
#include <stdint.h>
#include <stddef.h>
#include "Font.h"

namespace {
constexpr unsigned kRows = 6;
constexpr unsigned kChars = 96;
static const uint64_t kFont[] = GLYPH_FONT;
volatile unsigned gStageCount;
char gStages[kRows][kChars];
uintptr_t gFrame;
unsigned gWidth, gHeight, gStride;

static void barrier() {
#if defined(__aarch64__) || defined(__arm64__)
    __asm__ volatile("dsb sy" ::: "memory");
#endif
}

static void draw_cell(uintptr_t frame, unsigned stride, unsigned x, unsigned y,
                      unsigned scale, unsigned char character) {
    if (character < 32 || character > 126) character = '?';
    const uint64_t glyph = kFont[character - 32];
    for (unsigned gy = 0; gy < 14; ++gy) {
        const unsigned bits = gy < 6 ? (unsigned)(glyph >> (32 + gy * 5)) :
            gy < 12 ? (unsigned)(glyph >> ((gy - 6) * 5)) : 0;
        for (unsigned gx = 0; gx < 6; ++gx) {
            const uint32_t color = gx < 5 && ((bits >> gx) & 1) ? 0xffffffffu : 0xff000000u;
            for (unsigned sy = 0; sy < scale; ++sy) {
                for (unsigned sx = 0; sx < scale; ++sx) {
                    const uintptr_t at = frame + (uintptr_t)(y + gy * scale + sy) * stride +
                        (uintptr_t)(x + gx * scale + sx) * 4;
                    *(volatile uint32_t *)at = color;
                }
            }
        }
    }
}

static void draw_row(unsigned slot) {
    const unsigned scale = gWidth >= 2160 && gHeight >= 1400 ? 2 : 1;
    const unsigned y = 416 + slot * 14 * scale;
    if (y + 14 * scale > gHeight) return;
    const unsigned columns = (gWidth - 8) / (6 * scale);
    const char *text = gStages[slot];
    bool ended = false;
    for (unsigned col = 0; col < kChars - 1 && col < columns; ++col) {
        if (!text[col]) ended = true;
        draw_cell(gFrame, gStride, 8 + col * 6 * scale, y, scale,
                  ended ? ' ' : (unsigned char)text[col]);
    }
}
}

extern "C" void uke_boot_stage(const char *text) {
    if (!text) return;
    const unsigned sequence = __atomic_load_n(&gStageCount, __ATOMIC_RELAXED);
    char *row = gStages[sequence % kRows];
    unsigned number = sequence + 1;
    for (unsigned digit = 0; digit < 6; ++digit) {
        row[5 - digit] = '0' + number % 10;
        number /= 10;
    }
    row[6] = ' ';
    unsigned i = 7;
    for (; i < kChars - 1 && text[i - 7]; ++i) {
        const unsigned char c = (unsigned char)text[i - 7];
        row[i] = c == '\n' || c == '\r' ? ' ' : c >= 32 && c <= 126 ? (char)c : '?';
    }
    row[i] = 0;
    __atomic_store_n(&gStageCount, sequence + 1, __ATOMIC_RELEASE);
    if (gFrame) {
        draw_row(sequence % kRows);
        barrier();
    }
}

extern "C" void uke_boot_stage_unbind(void) {
    gFrame = 0;
    barrier();
}

extern "C" void uke_boot_stage_bind(uintptr_t frame, unsigned width, unsigned height,
                                    unsigned stride, unsigned depth) {
    if (depth != 32 || width < 720 || width > 4096 || height < 560 || height > 4096 ||
        stride < width * 4 || stride > 65536 || frame < 0xffff000000000000ULL ||
        frame > UINTPTR_MAX - (uint64_t)height * stride) return;
    gFrame = frame;
    gWidth = width;
    gHeight = height;
    gStride = stride;
    const unsigned count = __atomic_load_n(&gStageCount, __ATOMIC_ACQUIRE);
    const unsigned first = count > kRows ? count - kRows : 0;
    for (unsigned row = first; row < count; ++row) draw_row(row % kRows);
    barrier();
}
