// SPDX-License-Identifier: MIT
// Contact lifecycle for the FT8719 report. See FtsTouchCore.h.
#include "FtsTouchCore.h"

#include <string.h>

static uint16_t clamp_axis(uint16_t value, uint16_t maximum)
{
    if (!maximum) {
        return 0;
    }
    return value < maximum ? value : (uint16_t)(maximum - 1);
}

int fts_core_init(struct fts_touch_core *core,
                  const struct fts_core_config *config)
{
    if (!core || !config || !config->raw_x_max || !config->raw_y_max) {
        return FTS_ERR_PARAM;
    }
    memset(core, 0, sizeof(*core));
    core->cfg = *config;
    core->x_max = config->swap_xy ? config->raw_y_max : config->raw_x_max;
    core->y_max = config->swap_xy ? config->raw_x_max : config->raw_y_max;
    return FTS_OK;
}

void fts_core_reset(struct fts_touch_core *core)
{
    if (!core) {
        return;
    }
    memset(core->slots, 0, sizeof(core->slots));
    core->active = 0;
    core->frames = 0;
    core->released = 0;
    core->duplicates = 0;
    core->out_of_range = 0;
    core->dropped = 0;
}

void fts_core_map(const struct fts_touch_core *core, uint16_t raw_x,
                  uint16_t raw_y, uint16_t *x, uint16_t *y)
{
    uint16_t rx, ry, mx, my;

    if (!core) {
        if (x) {
            *x = 0;
        }
        if (y) {
            *y = 0;
        }
        return;
    }
    rx = clamp_axis(raw_x, core->cfg.raw_x_max);
    ry = clamp_axis(raw_y, core->cfg.raw_y_max);
    if (core->cfg.swap_xy) {
        mx = ry;
        my = rx;
    } else {
        mx = rx;
        my = ry;
    }
    if (core->cfg.invert_x && core->x_max) {
        mx = (uint16_t)(core->x_max - 1 - mx);
    }
    if (core->cfg.invert_y && core->y_max) {
        my = (uint16_t)(core->y_max - 1 - my);
    }
    if (x) {
        *x = mx;
    }
    if (y) {
        *y = my;
    }
}

int fts_core_process(struct fts_touch_core *core,
                     const struct fts_point *points, int count,
                     struct fts_core_report *report)
{
    struct fts_core_point out[FTS_CORE_SLOTS];
    uint8_t have[FTS_CORE_SLOTS];
    uint16_t seen = 0;
    unsigned i;
    unsigned id;

    if (!core || !report || count < 0 || (count > 0 && !points)) {
        return FTS_ERR_PARAM;
    }
    memset(report, 0, sizeof(*report));
    memset(out, 0, sizeof(out));
    memset(have, 0, sizeof(have));
    report->x_max = core->x_max;
    report->y_max = core->y_max;

    for (i = 0; i < (unsigned)count; ++i) {
        const struct fts_point *point = &points[i];
        uint16_t bit;
        uint16_t x, y;

        if (point->id >= FTS_CORE_SLOTS) {
            ++core->out_of_range;
            continue;
        }
        bit = (uint16_t)(1u << point->id);
        if (seen & bit) {
            // A hardware id names one contact. Keep the first record.
            ++core->duplicates;
            continue;
        }
        seen |= bit;
        fts_core_map(core, point->x, point->y, &x, &y);

        if (point->event == 1) { // FTS_TOUCH_UP
            if (!core->slots[point->id].active) {
                ++core->dropped;
                continue;
            }
            core->slots[point->id].active = 0;
            core->active &= (uint16_t)~bit;
            report->up |= bit;
            out[point->id].id = point->id;
            out[point->id].active = 0;
            out[point->id].x = x;
            out[point->id].y = y;
            out[point->id].pressure = point->pressure;
            out[point->id].area = point->area;
            have[point->id] = 1;
            continue;
        }
        if (point->event != 0 && point->event != 2) {
            ++core->dropped;
            continue;
        }
        // The controller may omit the first down record after a bus error.
        // A contact record for an inactive id starts the slot rather than
        // leaving the finger invisible until the next physical touch.
        if (!core->slots[point->id].active) {
            core->slots[point->id].active = 1;
            core->active |= bit;
            report->down |= bit;
        }
        core->slots[point->id].x = x;
        core->slots[point->id].y = y;
        core->slots[point->id].pressure = point->pressure;
        core->slots[point->id].area = point->area;
        out[point->id].id = point->id;
        out[point->id].active = 1;
        out[point->id].x = x;
        out[point->id].y = y;
        out[point->id].pressure = point->pressure;
        out[point->id].area = point->area;
        have[point->id] = 1;
    }

    // Release contacts that the report did not mention. A complete FT8719
    // report lists every active contact, so this is the lift of a contact
    // whose up record was lost, and prevents a stuck finger.
    for (id = 0; id < FTS_CORE_SLOTS; ++id) {
        const uint16_t bit = (uint16_t)(1u << id);
        if (!(core->active & bit) || (seen & bit)) {
            continue;
        }
        core->slots[id].active = 0;
        core->active &= (uint16_t)~bit;
        report->up |= bit;
        ++core->released;
        out[id].id = (uint8_t)id;
        out[id].active = 0;
        out[id].x = core->slots[id].x;
        out[id].y = core->slots[id].y;
        out[id].pressure = core->slots[id].pressure;
        out[id].area = core->slots[id].area;
        have[id] = 1;
    }

    report->active = core->active;
    for (id = 0; id < FTS_CORE_SLOTS; ++id) {
        if (have[id]) {
            report->points[report->count++] = out[id];
        }
    }
    ++core->frames;
    return report->count;
}
