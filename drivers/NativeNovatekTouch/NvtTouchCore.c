// SPDX-License-Identifier: Apache-2.0
// See NvtTouchCore.h and NOTICE. Simplified finger-only reimplementation.
#include "NvtTouchCore.h"
#include <string.h>

static const int kRowMap[NVT_ROWS] = {
    26, 78, 130, 182, 234, 286, 338, 390, 442, 494,
    546, 598, 650, 702, 754, 806, 856, 906, 956, 1006,
    1056, 1106, 1156, 1206, 1256, 1306, 1356, 1406, 1456, 1506,
    1556, 1606, 1656, 1706, 1756, 1806, 1856, 1906, 1956, 2006,
};
static const int kColumnMap[NVT_COLUMNS] = {
    25, 75, 125, 175, 225, 275, 325, 375, 425, 476,
    528, 580, 632, 684, 736, 788, 840, 892, 944, 996,
    1048, 1099, 1149, 1199, 1249, 1299, 1349, 1399, 1449, 1499,
    1549, 1599, 1649, 1699, 1749, 1799, 1849, 1899, 1949, 2000,
    2052, 2104, 2156, 2208, 2260, 2312, 2364, 2416, 2468, 2520,
    2572, 2623, 2673, 2723, 2773, 2823, 2873, 2923, 2973, 3023,
};
#define AXIS_BASE 2700
#define AXIS_EDGE 600
#define AXIS_GAIN 4350
#define MAX_PEAKS 24
#define TRACK_RADIUS (NVT_SUPER_RESOLUTION*50*4)   // 2000 units: about 20% of the short axis per frame is far

int nvt_touch_row_center(int row) {return row>=0 && row<NVT_ROWS ? kRowMap[row] : 0;}
int nvt_touch_column_center(int column) {return column>=0 && column<NVT_COLUMNS ? kColumnMap[column] : 0;}

int nvt_touch_map_edge_axis(int value, int maximum, int low_edge_bias) {
    if (value < AXIS_EDGE) {
        int mapped = AXIS_GAIN * (AXIS_EDGE - value) / AXIS_BASE;
        if (mapped > AXIS_EDGE - 1) mapped = AXIS_EDGE - 1;
        return AXIS_EDGE - mapped - (low_edge_bias ? 1 : 0);
    }
    int edge_start = maximum - AXIS_EDGE;
    if (value > edge_start) {
        int mapped = AXIS_GAIN * (value - edge_start) / AXIS_BASE;
        if (mapped > AXIS_EDGE - 1) mapped = AXIS_EDGE - 1;
        return edge_start + mapped;
    }
    return value;
}

void nvt_touch_reset(struct nvt_touch_core *core) {
    memset(core, 0, sizeof(*core));
    core->next_tracking_id = 1;
}

static void sort_i16(int16_t *v, unsigned n) {
    for (unsigned i=1;i<n;++i) {int16_t x=v[i];unsigned j=i;while(j>0 && v[j-1]>x){v[j]=v[j-1];--j;}v[j]=x;}
}

static void form_reference(struct nvt_touch_core *core) {
    int16_t column[NVT_STARTUP_FRAMES];
    for (unsigned node=0;node<NVT_NODES;++node) {
        for (unsigned s=0;s<NVT_STARTUP_FRAMES;++s) column[s]=core->startup[s][node];
        sort_i16(column,NVT_STARTUP_FRAMES);
        core->reference[node]=(int16_t)(((int)column[NVT_STARTUP_FRAMES/2-1]+(int)column[NVT_STARTUP_FRAMES/2])/2);
    }
    core->have_reference=1;core->startup_count=0;
}

struct peak {int index, value;};
struct blob {long sum_w, sum_row, sum_col; int peak, area, label;};

static int delta_at(const int16_t *m, const int16_t *ref, int index) {return (int)m[index]-(int)ref[index];}

int nvt_touch_process(struct nvt_touch_core *core, const int16_t *matrix, unsigned frame_type,
                      struct nvt_contact *out, int max) {
    if (frame_type != 2 && frame_type != 4) return 0;
    if (frame_type == 2 && !core->have_reference) {
        memcpy(core->reference, matrix, sizeof(core->reference));
        core->have_reference=1;core->startup_count=0;return -1;
    }
    if (!core->have_reference) {
        memcpy(core->startup[core->startup_count++], matrix, sizeof(core->reference));
        if (core->startup_count < NVT_STARTUP_FRAMES) return -1;
        form_reference(core);
        return -1;
    }
    int *delta=core->delta;
    for (int i=0;i<NVT_NODES;++i) delta[i]=delta_at(matrix,core->reference,i);

    // Steepest-ascent labelling: every node above the noise floor flows to the local maximum above it.
    int *label=core->label;
    struct peak peaks[MAX_PEAKS];int npeaks=0;
    for (int i=0;i<NVT_NODES;++i) label[i]=-1;
    for (int i=0;i<NVT_NODES;++i) {
        if (delta[i] < NVT_NOISE_THRESHOLD) continue;
        int cur=i;
        for (;;) {
            int best=cur,bv=delta[cur],r=cur/NVT_COLUMNS,c=cur%NVT_COLUMNS;
            for (int dr=-1;dr<=1;++dr) for (int dc=-1;dc<=1;++dc) {
                if (!dr && !dc) continue;
                int rr=r+dr,cc=c+dc;
                if (rr<0||rr>=NVT_ROWS||cc<0||cc>=NVT_COLUMNS) continue;
                int n=rr*NVT_COLUMNS+cc;
                if (delta[n] > bv || (delta[n]==bv && n>best && delta[n]>delta[cur])) {bv=delta[n];best=n;}
            }
            if (best==cur) break;
            cur=best;
        }
        // cur is a local maximum; register it as a peak the first time we see it.
        int found=-1;
        for (int p=0;p<npeaks;++p) if (peaks[p].index==cur) {found=p;break;}
        if (found<0) {
            if (npeaks==MAX_PEAKS) continue;
            peaks[npeaks].index=cur;peaks[npeaks].value=delta[cur];found=npeaks++;
        }
        label[i]=found;
    }
    struct blob blobs[MAX_PEAKS];
    for (int p=0;p<npeaks;++p) memset(&blobs[p],0,sizeof(blobs[p])),blobs[p].peak=peaks[p].value,blobs[p].label=p;
    for (int i=0;i<NVT_NODES;++i) {
        if (label[i]<0) continue;
        struct blob *b=&blobs[label[i]];
        int w=delta[i]-NVT_COORDINATE_REDUCE;if (w<0) w=0;
        b->sum_w+=w;b->sum_row+=(long)w*kRowMap[i/NVT_COLUMNS]*NVT_SUPER_RESOLUTION;
        b->sum_col+=(long)w*kColumnMap[i%NVT_COLUMNS]*NVT_SUPER_RESOLUTION;++b->area;
    }
    struct nvt_contact found[MAX_PEAKS];int nfound=0;
    for (int p=0;p<npeaks;++p) {
        struct blob *b=&blobs[p];
        if (!b->sum_w || b->area>=NVT_PALM_MIN_NODES) continue;   // large blobs: palm-sized, not reported
        // Starting a contact needs the down threshold, keeping one only the up threshold.
        int x=(int)((b->sum_col + b->sum_w/2) / b->sum_w);
        int y=(int)((b->sum_row + b->sum_w/2) / b->sum_w);
        if (x<0) x=0; if (x>NVT_LONG_AXIS_MAX-1) x=NVT_LONG_AXIS_MAX-1;
        if (y<0) y=0; if (y>NVT_SHORT_AXIS_MAX-1) y=NVT_SHORT_AXIS_MAX-1;
        x=nvt_touch_map_edge_axis(x,NVT_LONG_AXIS_MAX,0);
        y=nvt_touch_map_edge_axis(y,NVT_SHORT_AXIS_MAX,1);
        found[nfound].slot=-1;found[nfound].tracking_id=0;found[nfound].x=x;found[nfound].y=y;
        found[nfound].peak=b->peak;found[nfound].area=b->area;found[nfound].weight=(int)b->sum_w;++nfound;
    }
    // Track: greedy nearest assignment within TRACK_RADIUS, keeping contacts above the up threshold.
    int used_contact[MAX_PEAKS]={0};
    for (int s=0;s<NVT_SLOTS;++s) {
        struct nvt_slot *slot=&core->slots[s];
        if (!slot->active) continue;
        int best=-1;long bd=0;
        for (int c=0;c<nfound;++c) {
            if (used_contact[c] || found[c].peak<NVT_UP_THRESHOLD) continue;
            long dx=found[c].x-slot->x,dy=found[c].y-slot->y,d=dx*dx+dy*dy;
            if (d>(long)TRACK_RADIUS*TRACK_RADIUS) continue;
            if (best<0||d<bd){best=c;bd=d;}
        }
        if (best<0) {slot->active=0;continue;}
        used_contact[best]=1;found[best].slot=s;found[best].tracking_id=slot->tracking_id;
        slot->x=found[best].x;slot->y=found[best].y;++slot->age;
    }
    for (int c=0;c<nfound;++c) {
        if (used_contact[c] || found[c].peak<NVT_DOWN_THRESHOLD) continue;
        for (int s=0;s<NVT_SLOTS;++s) {
            if (core->slots[s].active) continue;
            core->slots[s].active=1;core->slots[s].tracking_id=core->next_tracking_id++;core->slots[s].age=1;
            core->slots[s].x=found[c].x;core->slots[s].y=found[c].y;
            found[c].slot=s;found[c].tracking_id=core->slots[s].tracking_id;used_contact[c]=1;break;
        }
    }
    int count=0;
    for (int c=0;c<nfound && count<max;++c) {
        if (found[c].slot<0 || core->slots[found[c].slot].age<2) continue;   // report from the second frame on
        out[count++]=found[c];
    }
    return count;
}
