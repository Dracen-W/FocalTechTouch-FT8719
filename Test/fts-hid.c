// SPDX-License-Identifier: MIT
// Host tests for the seqlock snapshot in FtsHid.c.
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "FtsHid.h"

static unsigned sink_calls;

static void sink(void *context)
{
    assert(context == (void *)0x1234);
    ++sink_calls;
}

int main(void)
{
    struct fts_core_report report;
    struct fts_hid_state state;

    fts_hid_set_sink(sink, (void *)0x1234);

    memset(&report, 0, sizeof(report));
    report.count = 2;
    report.x_max = 1080;
    report.y_max = 2340;
    report.points[0].id = 3;
    report.points[0].active = 1;
    report.points[0].x = 100;
    report.points[0].y = 200;
    report.points[1].id = 5;
    report.points[1].active = 0;
    report.points[1].x = 300;
    report.points[1].y = 400;
    fts_hid_publish(&report);
    assert(sink_calls == 1);
    assert((fts_touch_seq & 1u) == 0);
    assert(fts_touch_reports == 1);

    assert(fts_hid_read(&state) == FTS_OK);
    assert(state.count == 2);
    assert(state.x_max == 1080);
    assert(state.y_max == 2340);
    assert(state.points[0].id == 3);
    assert(state.points[0].tip == 1);
    assert(state.points[0].x == 100);
    assert(state.points[0].y == 200);
    assert(state.points[1].id == 5);
    assert(state.points[1].tip == 0);
    assert(state.points[1].x == 300);
    assert(state.points[1].y == 400);

    memset(&report, 0, sizeof(report));
    report.x_max = 1080;
    report.y_max = 2340;
    fts_hid_publish(&report);
    assert(sink_calls == 2);
    assert(fts_hid_read(&state) == FTS_OK);
    assert(state.count == 0);
    assert(state.x_max == 1080);
    assert(state.y_max == 2340);

    // An odd sequence means the writer is inside an update.
    fts_touch_seq = 1;
    assert(fts_hid_read(&state) == FTS_ERR_STATE);
    fts_touch_seq = 0;
    assert(fts_hid_read(&state) == FTS_OK);

    fts_hid_publish(NULL);
    assert(sink_calls == 2);
    assert(fts_hid_read(NULL) == FTS_ERR_PARAM);

    fts_hid_set_sink(NULL, NULL);
    fts_hid_publish(&report);
    assert(sink_calls == 2);

    puts("fts-hid: ok");
    return 0;
}
