// SPDX-License-Identifier: MIT
// Contact lifecycle and panel-coordinate mapping for the FT8719 report.
//
// The controller already segments the panel: each report carries up to ten
// six-byte records, one per hardware contact id, with an event code of down,
// up or contact. This layer therefore does not process a matrix. It keeps one
// slot per hardware id, turns records into stable down/up/active masks, and
// applies the board's axis orientation once. Plain C, no allocation.
#ifndef FTS_TOUCH_CORE_H
#define FTS_TOUCH_CORE_H

#include <stdint.h>

#include "FtsProtocol.h"

#define FTS_CORE_SLOTS FTS_MAX_POINTS

// The raw maxima are the controller's coordinate range (the panel resolution
// plus one). swap_xy is applied before inversion, so x_max and y_max in the
// report are the bounds after swapping.
struct fts_core_config {
    uint16_t raw_x_max;
    uint16_t raw_y_max;
    uint8_t swap_xy;
    uint8_t invert_x;
    uint8_t invert_y;
};

struct fts_core_point {
    uint8_t id;     // hardware contact id, 0 .. FTS_CORE_SLOTS-1
    uint8_t active; // 1 while the tip switch is down, 0 on the lift report
    uint16_t x, y;  // panel coordinates after mapping
    uint8_t pressure, area;
};

struct fts_core_report {
    // Masks indexed by hardware id. down and up describe this report; active
    // is the set still down after it.
    uint16_t down, up, active;
    uint8_t count;
    struct fts_core_point points[FTS_CORE_SLOTS];
    uint16_t x_max, y_max;
};

struct fts_touch_core {
    struct fts_core_config cfg;
    struct {
        uint8_t active;
        uint16_t x, y;
        uint8_t pressure, area;
    } slots[FTS_CORE_SLOTS];
    uint16_t active;
    uint16_t x_max, y_max;
    // Diagnostics for a failed bring-up.
    unsigned frames;
    unsigned released;    // active contacts missing from a report
    unsigned duplicates;  // a second record for the same id in one report
    unsigned out_of_range;
    unsigned dropped;     // invalid event, or an up for an inactive id
};

// Both axes must be nonzero. Returns FTS_OK or FTS_ERR_PARAM.
int fts_core_init(struct fts_touch_core *core,
                  const struct fts_core_config *config);

// Clears contacts and counters but keeps the configuration.
void fts_core_reset(struct fts_touch_core *core);

// Maps one raw coordinate pair without touching a core.
void fts_core_map(const struct fts_touch_core *core, uint16_t raw_x,
                  uint16_t raw_y, uint16_t *x, uint16_t *y);

// Consumes one decoded report in place. points[] comes from
// fts_frame_points() and may be NULL when count is zero.
//
// A record without an up event is a contact update. An active slot that is
// absent from a complete report has lifted: the controller reports every
// active contact in every frame, so a missing slot must not stay stuck. The
// release is published at the slot's last position.
//
// Returns the number of entries written to report->points, or a negative
// error.
int fts_core_process(struct fts_touch_core *core,
                     const struct fts_point *points, int count,
                     struct fts_core_report *report);

#endif
