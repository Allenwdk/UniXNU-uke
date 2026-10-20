// SPDX-License-Identifier: Apache-2.0
// Finger-touch processing for the NT36532E THP matrix: baseline, delta, peaks, centroid and slot tracking.
// Constants and axis mapping follow xiaomi-sheng-thp (Apache-2.0); see NOTICE. Plain C, no allocation.
#ifndef NVT_TOUCH_CORE_H
#define NVT_TOUCH_CORE_H
#include <stdint.h>
#include <stddef.h>

#define NVT_ROWS 40
#define NVT_COLUMNS 60
#define NVT_NODES (NVT_ROWS*NVT_COLUMNS)
#define NVT_SUPER_RESOLUTION 10
#define NVT_SLOTS 12
#define NVT_STARTUP_FRAMES 72
// Output axes (short axis follows rows, long axis follows columns), before any display orientation.
#define NVT_LONG_AXIS_MAX 30480
#define NVT_SHORT_AXIS_MAX 20320

#define NVT_NOISE_THRESHOLD 100
#define NVT_PEAK_THRESHOLD 220
#define NVT_DOWN_THRESHOLD 300
#define NVT_UP_THRESHOLD 220
#define NVT_COORDINATE_REDUCE 25
#define NVT_PALM_MIN_NODES 60

struct nvt_contact {
    int slot, tracking_id;
    int x, y;        // x along the long axis, y along the short axis, 0 .. max-1
    int peak, area, weight;
};

struct nvt_slot {
    int active, tracking_id, age, x, y;
};

struct nvt_touch_core {
    int16_t reference[NVT_NODES];
    int have_reference;
    unsigned startup_count;
    int16_t startup[NVT_STARTUP_FRAMES][NVT_NODES];   // collected until the median reference is formed
    struct nvt_slot slots[NVT_SLOTS];
    int next_tracking_id;
    // Scratch for nvt_touch_process: 19 KB, kept here because kernel threads have 16 KiB stacks (a stack probe in the
    // function prologue faulted on hardware when these were locals).
    int delta[NVT_NODES];
    int label[NVT_NODES];
};

void nvt_touch_reset(struct nvt_touch_core *core);
// Feed one touch matrix (row-major, 40x60 int16). Returns the number of visible contacts written to out[0..max)
// (-1 while the startup baseline is still being collected). frame_type 2 = baseline frame.
int nvt_touch_process(struct nvt_touch_core *core, const int16_t *matrix, unsigned frame_type,
                      struct nvt_contact *out, int max);
// Edge compensation applied to a raw centroid coordinate.
int nvt_touch_map_edge_axis(int value, int maximum, int low_edge_bias);
// Row/column centre tables (before super resolution).
int nvt_touch_row_center(int row);
int nvt_touch_column_center(int column);
#endif
