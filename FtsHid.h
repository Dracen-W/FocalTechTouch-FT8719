// SPDX-License-Identifier: MIT
// Seqlock snapshot between the touch thread and the HID digitizer source.
//
// The reader is a separate XNU component (the HID input driver). It reads the
// sequence before and after copying; an odd value or a change means the copy
// was torn and must be retried. The publisher is the single touch thread.
//
// Coordinates are panel pixels, not the Sheng 30480x20320 units: the maxima
// travel with the snapshot so a reader can normalise for its report descriptor
// without hard-coded panel constants.
#ifndef FTS_HID_H
#define FTS_HID_H

#include <stdint.h>

#include "FtsTouchCore.h"

#define FTS_HID_MAX_POINTS FTS_CORE_SLOTS

struct fts_hid_point {
    int32_t x, y;
    uint8_t id;
    uint8_t tip; // 1 down, 0 lift
};

struct fts_hid_state {
    uint32_t count;
    uint16_t x_max, y_max;
    struct fts_hid_point points[FTS_HID_MAX_POINTS];
};

// Called by the touch thread after the seqlock has closed. The board's HID
// source registers one sink; passing NULL unregisters it.
typedef void (*fts_hid_sink)(void *context);
void fts_hid_set_sink(fts_hid_sink sink, void *context);

// Publishes one core report. Every entry in report->points is copied,
// including lift entries, so the reader can clear the matching contact.
void fts_hid_publish(const struct fts_core_report *report);

// Copies one consistent snapshot. Returns FTS_OK, or FTS_ERR_STATE if the
// writer kept the sequence changing for the retry budget.
int fts_hid_read(struct fts_hid_state *state);

// Snapshot globals. A reader may access these directly with its own seqlock
// loop instead of calling fts_hid_read.
extern volatile uint32_t fts_touch_seq;
extern volatile uint32_t fts_touch_count;
extern volatile uint32_t fts_touch_x_max;
extern volatile uint32_t fts_touch_y_max;
extern volatile int32_t fts_touch_xs[FTS_HID_MAX_POINTS];
extern volatile int32_t fts_touch_ys[FTS_HID_MAX_POINTS];
extern volatile uint32_t fts_touch_ids[FTS_HID_MAX_POINTS];
extern volatile uint32_t fts_touch_tips[FTS_HID_MAX_POINTS];
extern volatile uint32_t fts_touch_reports;

#endif
