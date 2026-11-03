// SPDX-License-Identifier: MIT
// Seqlock publication of the latest contact set. See FtsHid.h.
#include "FtsHid.h"

volatile uint32_t fts_touch_seq;
volatile uint32_t fts_touch_count;
volatile uint32_t fts_touch_x_max;
volatile uint32_t fts_touch_y_max;
volatile int32_t fts_touch_xs[FTS_HID_MAX_POINTS];
volatile int32_t fts_touch_ys[FTS_HID_MAX_POINTS];
volatile uint32_t fts_touch_ids[FTS_HID_MAX_POINTS];
volatile uint32_t fts_touch_tips[FTS_HID_MAX_POINTS];
volatile uint32_t fts_touch_reports;

static fts_hid_sink g_sink;
static void *g_sink_context;

void fts_hid_set_sink(fts_hid_sink sink, void *context)
{
    g_sink_context = context;
    __atomic_store_n(&g_sink, sink, __ATOMIC_RELEASE);
}

void fts_hid_publish(const struct fts_core_report *report)
{
    fts_hid_sink sink;
    void *context;
    uint32_t seq;
    unsigned i;

    if (!report) {
        return;
    }
    seq = __atomic_load_n(&fts_touch_seq, __ATOMIC_RELAXED);
    __atomic_store_n(&fts_touch_seq, seq + 1, __ATOMIC_RELEASE);

    __atomic_store_n(&fts_touch_count, report->count, __ATOMIC_RELAXED);
    __atomic_store_n(&fts_touch_x_max, report->x_max, __ATOMIC_RELAXED);
    __atomic_store_n(&fts_touch_y_max, report->y_max, __ATOMIC_RELAXED);
    for (i = 0; i < report->count && i < FTS_HID_MAX_POINTS; ++i) {
        __atomic_store_n(&fts_touch_xs[i], report->points[i].x,
                         __ATOMIC_RELAXED);
        __atomic_store_n(&fts_touch_ys[i], report->points[i].y,
                         __ATOMIC_RELAXED);
        __atomic_store_n(&fts_touch_ids[i], report->points[i].id,
                         __ATOMIC_RELAXED);
        __atomic_store_n(&fts_touch_tips[i], report->points[i].active,
                         __ATOMIC_RELAXED);
    }

    __atomic_store_n(&fts_touch_seq, seq + 2, __ATOMIC_RELEASE);
    __atomic_fetch_add(&fts_touch_reports, 1, __ATOMIC_RELAXED);

    // Load the sink before its context: the release store in
    // fts_hid_set_sink publishes the context first, so an acquire load of a
    // new sink also sees the matching context.
    sink = __atomic_load_n(&g_sink, __ATOMIC_ACQUIRE);
    context = __atomic_load_n(&g_sink_context, __ATOMIC_RELAXED);
    if (sink) {
        sink(context);
    }
}

int fts_hid_read(struct fts_hid_state *state)
{
    unsigned attempt;

    if (!state) {
        return FTS_ERR_PARAM;
    }
    for (attempt = 0; attempt < 8; ++attempt) {
        uint32_t before, after, count;
        unsigned i;

        before = __atomic_load_n(&fts_touch_seq, __ATOMIC_ACQUIRE);
        if (before & 1u) {
            continue;
        }
        count = __atomic_load_n(&fts_touch_count, __ATOMIC_RELAXED);
        if (count > FTS_HID_MAX_POINTS) {
            count = FTS_HID_MAX_POINTS;
        }
        state->count = count;
        state->x_max = (uint16_t)__atomic_load_n(&fts_touch_x_max,
                                                 __ATOMIC_RELAXED);
        state->y_max = (uint16_t)__atomic_load_n(&fts_touch_y_max,
                                                 __ATOMIC_RELAXED);
        for (i = 0; i < count; ++i) {
            state->points[i].x = __atomic_load_n(&fts_touch_xs[i],
                                                 __ATOMIC_RELAXED);
            state->points[i].y = __atomic_load_n(&fts_touch_ys[i],
                                                 __ATOMIC_RELAXED);
            state->points[i].id = (uint8_t)__atomic_load_n(
                &fts_touch_ids[i], __ATOMIC_RELAXED);
            state->points[i].tip = (uint8_t)__atomic_load_n(
                &fts_touch_tips[i], __ATOMIC_RELAXED);
        }
        after = __atomic_load_n(&fts_touch_seq, __ATOMIC_ACQUIRE);
        if (before == after) {
            return FTS_OK;
        }
    }
    return FTS_ERR_STATE;
}
