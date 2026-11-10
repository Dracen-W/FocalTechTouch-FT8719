// SPDX-License-Identifier: MIT
// Staged, default-off bring-up of the FT8719 on merlinx.
//
// Boot arguments:
//   fts-touch=<1..3>  1 identify, 2 firmware if needed + version, 3 frame loop
//   fts-touch-poll=1  poll instead of waiting on the board interrupt
//   fts-touch-swap=1  override the board axis swap
//   fts-touch-invert-x=1, fts-touch-invert-y=1  override the board inversion
//
// The board repository owns the SPI engine, GPIOs and clocks. It supplies the
// weak symbols below and calls fts_touch_probe(board_context) once the bus is
// usable. This repository deliberately contains no MediaTek SPI driver and no
// vendor firmware image.
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <kern/thread.h>
#include <pexpert/pexpert.h>
#include <libkern/libkern.h>
#include <string.h>

extern "C" {
#include "FtsBoard.h"
#include "FtsHid.h"
#include "FtsProtocol.h"
#include "FtsTouchCore.h"

// Board transport. One call is one chip-select period: send tx[0], wait
// first_delay_us, then send tx[1..len). rx may be NULL for a write. The board
// serialises access to its bus and owns the buffers passed in.
int fts_board_spi_transfer(void *context, const uint8_t *tx, uint8_t *rx,
                           size_t len, unsigned first_delay_us)
    __attribute__((weak_import));
void fts_board_delay_us(void *context, unsigned us)
    __attribute__((weak_import));
void fts_board_delay_ms(void *context, unsigned ms)
    __attribute__((weak_import));
// asserted=1 drives the controller reset line low, 0 releases it.
void fts_board_reset(void *context, int asserted)
    __attribute__((weak_import));
// Return 0 when the controller has a frame ready, FTS_ERR_TIMEOUT when the
// timeout expires, and another negative value on a board error.
int fts_board_irq_wait(void *context, unsigned timeout_ms)
    __attribute__((weak_import));

// Optional, board-owned firmware. The image is never part of this repository.
extern const uint8_t fts_board_firmware[] __attribute__((weak_import));
extern const uint32_t fts_board_firmware_size __attribute__((weak_import));
}

// Diagnostic page. The board's boot log may read these at any time.
extern "C" {
char fts_touch_rows[4][120];
volatile uint32_t fts_touch_stage_req, fts_touch_stage_done;
volatile uint32_t fts_touch_step;
volatile uint32_t fts_touch_frames, fts_touch_reads, fts_touch_idle;
volatile uint32_t fts_touch_recover, fts_touch_bad, fts_touch_contacts;
volatile uint32_t fts_touch_model, fts_touch_version;
volatile int fts_touch_error;
volatile int fts_touch_last_x = -1, fts_touch_last_y = -1;
}

namespace {

// Enough for up to a 4096-byte firmware chunk plus its package byte and CRC tail.
constexpr size_t kBusBytes = (FTS_FW_CHUNK > 4096 ? FTS_FW_CHUNK : 4096) + 16;

struct Session {
    void *board;
    struct fts_bus bus;
    struct fts_chip chip;
    struct fts_touch_core core;
    uint8_t *tx;
    uint8_t *rx;
    uint8_t *frame;
    struct fts_point points[FTS_MAX_POINTS];
    uint16_t model;
    uint8_t version;
    uint32_t reads, idle, recover, bad;
    uint32_t consecutive_errors;
    int last_rc;
};

Session *gSession;
volatile int gSuspended;
volatile int gSuspendedPublished;
volatile int gSuspendedAck;
unsigned gStage;
int gPoll;

int busXfer(void *context, const uint8_t *tx, uint8_t *rx, size_t len,
            unsigned first_delay_us)
{
    Session *session = static_cast<Session *>(context);

    if (!&fts_board_spi_transfer) {
        return FTS_ERR_BUS;
    }
    return fts_board_spi_transfer(session->board, tx, rx, len, first_delay_us);
}

void busDelayUs(void *context, unsigned us)
{
    Session *session = static_cast<Session *>(context);

    if (&fts_board_delay_us) {
        fts_board_delay_us(session->board, us);
    } else {
        IODelay(us);
    }
}

void busDelayMs(void *context, unsigned ms)
{
    Session *session = static_cast<Session *>(context);

    if (&fts_board_delay_ms) {
        fts_board_delay_ms(session->board, ms);
    } else {
        IOSleep(ms);
    }
}

void busReset(void *context, int asserted)
{
    Session *session = static_cast<Session *>(context);

    if (&fts_board_reset) {
        fts_board_reset(session->board, asserted);
    }
}

void fail(unsigned step, int code)
{
    if (fts_touch_error != 0) {
        return;
    }
    fts_touch_step = step;
    fts_touch_error = (code == 0) ? -1 : code;
}

void rows(const Session &session)
{
    const struct fts_chip &chip = session.chip;

    snprintf(fts_touch_rows[0], sizeof(fts_touch_rows[0]),
             "FTS stage %u/%u step %u err %d id%04x ver%02x",
             fts_touch_stage_done, fts_touch_stage_req, fts_touch_step,
             fts_touch_error, (unsigned)session.model,
             (unsigned)session.version);
    snprintf(fts_touch_rows[1], sizeof(fts_touch_rows[1]),
             "FTS fw%d tx%u crc%u poll%u last%d rc%d",
             chip.fw_running, chip.transfer_errors, chip.crc_errors,
             chip.idle_polls, chip.last_error, session.last_rc);
    snprintf(fts_touch_rows[2], sizeof(fts_touch_rows[2]),
             "FTS frames%u reads%u idle%u rec%u bad%u c%u",
             session.reads + session.idle + session.recover + session.bad,
             session.reads, session.idle, session.recover, session.bad,
             fts_touch_contacts);
    snprintf(fts_touch_rows[3], sizeof(fts_touch_rows[3]),
             "FTS last %d,%d rel%u dup%u range%u drop%u",
             fts_touch_last_x, fts_touch_last_y, session.core.released,
             session.core.duplicates, session.core.out_of_range,
             session.core.dropped);
}

void mark(const Session &session, unsigned step)
{
    fts_touch_step = step;
    rows(session);
}

void publish(const struct fts_core_report &report)
{
    fts_hid_publish(&report);
    fts_touch_contacts = report.count;
    if (report.count) {
        fts_touch_last_x = report.points[0].x;
        fts_touch_last_y = report.points[0].y;
    }
}

// Releases every contact through the normal core path, so the HID source sees
// one lift report for each finger that was down.
void releaseAll(Session &session)
{
    struct fts_core_report report;

    if (!session.core.cfg.raw_x_max) {
        return;
    }
    if (fts_core_process(&session.core, nullptr, 0, &report) >= 0 &&
        (report.count || report.up)) {
        publish(report);
    }
}

bool haveFirmware(void)
{
    return &fts_board_firmware && &fts_board_firmware_size &&
           fts_board_firmware_size != 0;
}

int recoverChip(Session &session)
{
    uint16_t model = 0;
    int result;

    // The application may have lost its state, or the controller may have
    // fallen back to its bootloader. Try the register wake first.
    session.chip.fw_running = 1;
    result = fts_wake(&session.chip);
    if (result == FTS_OK &&
        fts_identify(&session.chip, &model) == FTS_OK &&
        model == FTS_MODEL_FT8719) {
        session.model = model;
        return FTS_OK;
    }
    // Re-download only when the board supplies an image. A board without one
    // can still recover through the reset path in fts_wake().
    if (haveFirmware()) {
        result = fts_firmware_download(&session.chip, fts_board_firmware,
                                       fts_board_firmware_size);
        if (result == FTS_OK) {
            session.model = FTS_MODEL_FT8719;
            return FTS_OK;
        }
    }
    return result ? result : FTS_ERR_STATE;
}

void frameLoop(Session &session)
{
    unsigned lastRows = 0;

    for (;;) {
        struct fts_core_report report;
        int kind;
        int count;

        if (__atomic_load_n(&gSuspended, __ATOMIC_ACQUIRE)) {
            if (!__atomic_load_n(&gSuspendedPublished, __ATOMIC_RELAXED)) {
                releaseAll(session);
                __atomic_store_n(&gSuspendedPublished, 1, __ATOMIC_RELEASE);
                __atomic_store_n(&gSuspendedAck, 1, __ATOMIC_RELEASE);
            }
            IOSleep(20);
            continue;
        }
        __atomic_store_n(&gSuspendedPublished, 0, __ATOMIC_RELAXED);

        if (!gPoll && &fts_board_irq_wait) {
            int wait = fts_board_irq_wait(session.board, 100);

            if (wait == FTS_ERR_TIMEOUT) {
                continue;
            }
            if (wait < 0) {
                ++session.bad;
                fts_touch_bad = session.bad;
                IOSleep(10);
                continue;
            }
        } else {
            IOSleep(10);
        }

        kind = fts_read_report(&session.chip, session.frame,
                               FTS_TOUCH_FRAME_LEN);
        session.last_rc = kind;

        if (kind == FTS_REPORT_RECOVER) {
            ++session.recover;
            fts_touch_recover = session.recover;
            session.consecutive_errors = 0;
            releaseAll(session);
            if (recoverChip(session) != FTS_OK) {
                ++session.bad;
                fts_touch_bad = session.bad;
                IOSleep(20);
            }
            continue;
        }
        if (kind < 0) {
            ++session.bad;
            fts_touch_bad = session.bad;
            // A single CRC or bus error is normal under load. Sixteen in a row
            // mean the controller state is gone.
            if (++session.consecutive_errors >= 16) {
                session.consecutive_errors = 0;
                if (recoverChip(session) != FTS_OK) {
                    IOSleep(20);
                }
            }
            continue;
        }
        session.consecutive_errors = 0;
        if (kind == FTS_REPORT_IDLE) {
            const int had = session.core.active != 0;

            ++session.idle;
            fts_touch_idle = session.idle;
            if (fts_core_process(&session.core, nullptr, 0, &report) >= 0 &&
                (had || report.count)) {
                publish(report);
            }
            continue;
        }

        ++session.reads;
        fts_touch_reads = session.reads;
        count = fts_frame_points(session.frame, FTS_TOUCH_FRAME_LEN,
                                 session.points, FTS_MAX_POINTS);
        if (count < 0) {
            ++session.bad;
            fts_touch_bad = session.bad;
            continue;
        }
        if (fts_core_process(&session.core, session.points, count,
                             &report) >= 0) {
            publish(report);
        }
        fts_touch_frames = fts_touch_frames + 1;
        if (session.reads - lastRows >= 64) {
            lastRows = session.reads;
            rows(session);
        }
    }
}

void touchThread(void *parameter, wait_result_t)
{
    Session *session;

    thread_set_thread_name(current_thread(), "fts-touch");
    session = static_cast<Session *>(IOMalloc(sizeof(Session)));
    if (!session) {
        fail(0, -12);
        return;
    }
    bzero(session, sizeof(*session));
    session->board = parameter;
    __atomic_store_n(&gSession, session, __ATOMIC_RELEASE);

    do {
        session->tx = static_cast<uint8_t *>(IOMalloc(kBusBytes));
        session->rx = static_cast<uint8_t *>(IOMalloc(kBusBytes));
        session->frame = static_cast<uint8_t *>(IOMalloc(FTS_TOUCH_FRAME_LEN));
        if (!session->tx || !session->rx || !session->frame) {
            fail(1, -12);
            break;
        }
        if (!&fts_board_spi_transfer) {
            fail(1, FTS_ERR_BUS);
            break;
        }
        session->bus = (struct fts_bus){session, busXfer, busDelayUs,
                                        busDelayMs, busReset};
        if (fts_chip_init(&session->chip, &session->bus, session->tx,
                          kBusBytes, session->rx, kBusBytes) != FTS_OK) {
            fail(1, FTS_ERR_PARAM);
            break;
        }

        mark(*session, 1);
        {
            int result = FTS_ERR_CHIP;
            unsigned attempt;

            // The controller boots into its application if already loaded, or
            // sits in its ROM bootloader on cold boot. Retry up to three times
            // so a transient power or clock settling glitch does not abort
            // bring-up permanently.
            for (attempt = 0; attempt < 3; ++attempt) {
                session->chip.fw_running = 1;
                result = fts_identify(&session->chip, &session->model);
                if (result != FTS_OK) {
                    result = fts_boot_identify(&session->chip, &session->model);
                }
                if (result == FTS_OK) {
                    break;
                }
                IOSleep(40);
            }
            if (result != FTS_OK) {
                fail(1, result);
                break;
            }
        }
        fts_touch_model = session->model;
        fts_touch_stage_done = 1;
        rows(*session);
        if (gStage < 2) {
            break;
        }

        {
            int result;

            if (!haveFirmware()) {
                fail(2, -2);
                break;
            }
            // The ROM and the application report the same id on this die, so
            // the model cannot tell whether an application is already running.
            // The vendor driver loads the image on every boot for the same
            // reason; a controller left in its ROM never reports touches.
            mark(*session, 2);
            result = fts_firmware_download(&session->chip, fts_board_firmware,
                                           fts_board_firmware_size);
            if (result != FTS_OK) {
                fail(2, result);
                break;
            }
            result = fts_identify(&session->chip, &session->model);
            if (result != FTS_OK || session->model != FTS_MODEL_FT8719) {
                fail(2, result ? result : FTS_ERR_CHIP);
                break;
            }
            fts_touch_model = session->model;
        }

        {
            int result = fts_read_version(&session->chip, &session->version);

            if (result != FTS_OK) {
                fail(2, result);
                break;
            }
            fts_touch_version = session->version;
        }
        {
            struct fts_core_config config = fts_board_core_config();
            unsigned swap = config.swap_xy;
            unsigned invertX = config.invert_x;
            unsigned invertY = config.invert_y;

            PE_parse_boot_argn("fts-touch-swap", &swap, sizeof(swap));
            PE_parse_boot_argn("fts-touch-invert-x", &invertX,
                               sizeof(invertX));
            PE_parse_boot_argn("fts-touch-invert-y", &invertY,
                               sizeof(invertY));
            config.swap_xy = swap ? 1 : 0;
            config.invert_x = invertX ? 1 : 0;
            config.invert_y = invertY ? 1 : 0;
            if (fts_core_init(&session->core, &config) != FTS_OK) {
                fail(2, FTS_ERR_PARAM);
                break;
            }
        }
        fts_touch_stage_done = 2;
        rows(*session);
        if (gStage < 3) {
            break;
        }

        fts_touch_stage_done = 3;
        mark(*session, 3);
        frameLoop(*session);
    } while (0);

    releaseAll(*session);
    rows(*session);
    __atomic_store_n(&gSession, nullptr, __ATOMIC_RELEASE);
    if (session->frame) {
        IOFree(session->frame, FTS_TOUCH_FRAME_LEN);
    }
    if (session->rx) {
        IOFree(session->rx, kBusBytes);
    }
    if (session->tx) {
        IOFree(session->tx, kBusBytes);
    }
    IOFree(session, sizeof(Session));
}

} // namespace

// Called by the board once its SPI bus, reset line and interrupt are usable.
// Returns 0 when the probe was started or was not requested. The probe is off
// unless the fts-touch boot argument is present.
extern "C" int fts_touch_probe(void *board)
{
    unsigned stage = 0;
    unsigned poll = 0;
    thread_t thread = nullptr;

    if (!PE_parse_boot_argn("fts-touch", &stage, sizeof(stage)) || stage < 1 ||
        fts_touch_stage_req) {
        return 0;
    }
    if (stage > 3) {
        stage = 3;
    }
    PE_parse_boot_argn("fts-touch-poll", &poll, sizeof(poll));
    gPoll = poll != 0;
    gStage = stage;
    fts_touch_stage_req = stage;
    if (kernel_thread_start(touchThread, board, &thread) == KERN_SUCCESS) {
        thread_deallocate(thread);
        return 0;
    }
    fts_touch_error = -12;
    return -12;
}

// The board calls these from a thread, not from an interrupt. They are valid
// once the frame loop has started (stage 3). Before that they are no-ops.
extern "C" int fts_touch_suspend(void)
{
    Session *session;
    unsigned waited;
    int acked = 0;

    if (__atomic_load_n(&fts_touch_stage_done, __ATOMIC_ACQUIRE) < 3) {
        return FTS_OK;
    }
    if (!__atomic_load_n(&gSuspended, __ATOMIC_ACQUIRE)) {
        __atomic_store_n(&gSuspendedAck, 0, __ATOMIC_RELAXED);
        __atomic_store_n(&gSuspended, 1, __ATOMIC_RELEASE);
    }
    // Wait until the frame loop has released the contacts and stopped
    // transferring. The interrupt wait is bounded at 100 ms, so half a second
    // covers the worst case with room to spare.
    for (waited = 0; waited < 500; ++waited) {
        if (__atomic_load_n(&gSuspendedAck, __ATOMIC_ACQUIRE)) {
            acked = 1;
            break;
        }
        IOSleep(1);
    }
    session = __atomic_load_n(&gSession, __ATOMIC_ACQUIRE);
    if (!session) {
        return FTS_OK;
    }
    if (!acked) {
        // Do not race a transfer that is still in flight.
        return FTS_ERR_TIMEOUT;
    }
    return fts_sleep(&session->chip);
}

extern "C" int fts_touch_resume(void)
{
    Session *session;
    int result = FTS_OK;

    if (__atomic_load_n(&fts_touch_stage_done, __ATOMIC_ACQUIRE) < 3) {
        return FTS_OK;
    }
    // Wake before the frame loop is allowed to run again.
    session = __atomic_load_n(&gSession, __ATOMIC_ACQUIRE);
    if (session) {
        result = fts_wake(&session->chip);
    }
    __atomic_store_n(&gSuspendedPublished, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&gSuspendedAck, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&gSuspended, 0, __ATOMIC_RELEASE);
    return result;
}
