// SPDX-License-Identifier: MIT
// Host tests for FtsTouchCore.c.
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "FtsTouchCore.h"

static struct fts_point point(unsigned id, unsigned event, unsigned x,
                              unsigned y)
{
    struct fts_point p;

    p.id = (uint8_t)id;
    p.event = (uint8_t)event;
    p.x = (uint16_t)x;
    p.y = (uint16_t)y;
    p.pressure = 0x20;
    p.area = 0x03;
    return p;
}

static void test_lifecycle(void)
{
    struct fts_touch_core core;
    struct fts_core_config config = {1080, 2340, 0, 0, 0};
    struct fts_core_report report;
    struct fts_point one;

    assert(fts_core_init(&core, &config) == FTS_OK);
    assert(core.x_max == 1080);
    assert(core.y_max == 2340);

    one = point(0, 0, 100, 200);
    assert(fts_core_process(&core, &one, 1, &report) == 1);
    assert(report.count == 1);
    assert(report.down == 0x0001);
    assert(report.up == 0x0000);
    assert(report.active == 0x0001);
    assert(report.points[0].id == 0);
    assert(report.points[0].active == 1);
    assert(report.points[0].x == 100);
    assert(report.points[0].y == 200);

    one = point(0, 2, 110, 210);
    assert(fts_core_process(&core, &one, 1, &report) == 1);
    assert(report.down == 0);
    assert(report.up == 0);
    assert(report.active == 0x0001);
    assert(report.points[0].x == 110);
    assert(report.points[0].y == 210);

    one = point(0, 1, 120, 220);
    assert(fts_core_process(&core, &one, 1, &report) == 1);
    assert(report.down == 0);
    assert(report.up == 0x0001);
    assert(report.active == 0);
    assert(report.points[0].active == 0);
    assert(report.points[0].x == 120);
    assert(report.points[0].y == 220);

    assert(fts_core_process(&core, NULL, 0, &report) == 0);
    assert(report.count == 0);
    assert(report.down == 0);
    assert(report.up == 0);
    assert(report.active == 0);
}

static void test_missing_contact_releases(void)
{
    struct fts_touch_core core;
    struct fts_core_config config = {1080, 2340, 0, 0, 0};
    struct fts_core_report report;
    struct fts_point one = point(1, 0, 300, 400);

    assert(fts_core_init(&core, &config) == FTS_OK);
    assert(fts_core_process(&core, &one, 1, &report) == 1);
    assert(report.active == 0x0002);

    assert(fts_core_process(&core, NULL, 0, &report) == 1);
    assert(report.up == 0x0002);
    assert(report.active == 0);
    assert(report.points[0].active == 0);
    assert(report.points[0].x == 300);
    assert(report.points[0].y == 400);
    assert(core.released == 1);
}

static void test_two_contacts_and_order(void)
{
    struct fts_touch_core core;
    struct fts_core_config config = {1080, 2340, 0, 0, 0};
    struct fts_core_report report;
    struct fts_point points[2];

    assert(fts_core_init(&core, &config) == FTS_OK);
    points[0] = point(1, 0, 30, 40);
    points[1] = point(0, 0, 10, 20);
    assert(fts_core_process(&core, points, 2, &report) == 2);
    assert(report.down == 0x0003);
    assert(report.active == 0x0003);
    assert(report.points[0].id == 0);
    assert(report.points[1].id == 1);

    points[0] = point(1, 2, 31, 41);
    points[1] = point(0, 2, 11, 21);
    assert(fts_core_process(&core, points, 2, &report) == 2);
    assert(report.points[0].x == 11);
    assert(report.points[0].y == 21);
    assert(report.points[1].x == 31);
    assert(report.points[1].y == 41);
}

static void test_bad_records(void)
{
    struct fts_touch_core core;
    struct fts_core_config config = {1080, 2340, 0, 0, 0};
    struct fts_core_report report;
    struct fts_point points[2];

    assert(fts_core_init(&core, &config) == FTS_OK);

    points[0] = point(0, 0, 10, 20);
    points[1] = point(0, 0, 30, 40);
    assert(fts_core_process(&core, points, 2, &report) == 1);
    assert(core.duplicates == 1);
    assert(report.down == 0x0001);

    // Close the contact opened by the duplicate test before the next cases.
    assert(fts_core_process(&core, NULL, 0, &report) == 1);
    assert(report.up == 0x0001);

    points[0] = point(10, 0, 50, 60);
    assert(fts_core_process(&core, points, 1, &report) == 0);
    assert(core.out_of_range == 1);

    points[0] = point(2, 3, 70, 80);
    assert(fts_core_process(&core, points, 1, &report) == 0);
    assert(core.dropped == 1);

    points[0] = point(3, 1, 90, 100);
    assert(fts_core_process(&core, points, 1, &report) == 0);
    assert(core.dropped == 2);
}

static void test_mapping(void)
{
    struct fts_touch_core core;
    struct fts_core_config config = {1080, 2340, 0, 0, 0};
    uint16_t x, y;

    assert(fts_core_init(&core, &config) == FTS_OK);
    fts_core_map(&core, 100, 200, &x, &y);
    assert(x == 100 && y == 200);
    fts_core_map(&core, 1080, 2340, &x, &y);
    assert(x == 1079 && y == 2339);
    fts_core_map(&core, 5000, 9999, &x, &y);
    assert(x == 1079 && y == 2339);

    config.swap_xy = 1;
    config.invert_x = 1;
    config.invert_y = 1;
    assert(fts_core_init(&core, &config) == FTS_OK);
    assert(core.x_max == 2340);
    assert(core.y_max == 1080);
    fts_core_map(&core, 100, 200, &x, &y);
    assert(x == 2139);
    assert(y == 979);
    fts_core_map(&core, 0, 0, &x, &y);
    assert(x == 2339);
    assert(y == 1079);
}

static void test_reset_and_validation(void)
{
    struct fts_touch_core core;
    struct fts_core_config config = {1080, 2340, 0, 0, 0};
    struct fts_core_report report;
    struct fts_point one = point(4, 0, 10, 20);

    assert(fts_core_init(&core, &config) == FTS_OK);
    assert(fts_core_process(&core, &one, 1, &report) == 1);
    assert(core.active == 0x0010);
    fts_core_reset(&core);
    assert(core.active == 0);
    assert(core.frames == 0);
    assert(core.released == 0);
    assert(fts_core_process(&core, NULL, 0, &report) == 0);
    assert(report.count == 0);

    config.raw_y_max = 0;
    assert(fts_core_init(&core, &config) == FTS_ERR_PARAM);
    assert(fts_core_init(NULL, &config) == FTS_ERR_PARAM);
}

int main(void)
{
    test_lifecycle();
    test_missing_contact_releases();
    test_two_contacts_and_order();
    test_bad_records();
    test_mapping();
    test_reset_and_validation();
    puts("touch-core: ok");
    return 0;
}
