// SPDX-License-Identifier: MIT
// Host tests for FtsProtocol.c. The fake bus implements the wire behaviour
// observed on the FT8719: one chip-select period per package, a status
// package, a command package with a length nibble, a data package, and a
// little-endian CRC-16/Kermit over read data.
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "FtsProtocol.h"

#define FAKE_MAX_DATA 4096

struct fake {
    uint8_t regs[256];
    uint8_t command[16];
    size_t command_len;
    uint8_t control;
    uint8_t data[FAKE_MAX_DATA];
    size_t data_len;
    const uint8_t *read_source;
    size_t read_source_len;
    uint8_t image[FTS_FW_MAX];
    size_t image_len;
    size_t ecc_len;
    uint8_t ecc_ok;
    int app_started;
    int corrupt_crc;
    int fail_next;
    unsigned transfers;
    unsigned resets;
    unsigned first_delays;
};

static uint16_t ref_ecc(const uint8_t *data, size_t len)
{
    uint16_t ecc = 0;
    size_t i;
    unsigned bit;

    for (i = 0; i + 1 < len; i += 2) {
        ecc ^= (uint16_t)(((uint16_t)data[i] << 8) | data[i + 1]);
        for (bit = 0; bit < 16; ++bit) {
            ecc = (uint16_t)((ecc >> 1) ^ ((ecc & 1) ? 0x8408 : 0));
        }
    }
    return ecc;
}

static void fake_fill_read(struct fake *f, uint8_t *dst, size_t len)
{
    size_t i;

    if (f->read_source) {
        size_t count = len < f->read_source_len ? len : f->read_source_len;
        memcpy(dst, f->read_source, count);
        if (count < len) {
            memset(dst + count, 0, len - count);
        }
        f->read_source = NULL;
        f->read_source_len = 0;
        return;
    }
    memset(dst, 0, len);
    switch (f->command[0]) {
    case 0x90:
        dst[0] = f->regs[0x90];
        if (len > 1) {
            dst[1] = f->regs[0x91];
        }
        break;
    case 0xCD: {
        const uint16_t ecc = ref_ecc(f->image, f->ecc_len);
        dst[0] = (uint8_t)(ecc >> 8);
        if (len > 1) {
            dst[1] = (uint8_t)ecc;
        }
        break;
    }
    case 0xCE:
        dst[0] = f->ecc_ok;
        break;
    default:
        for (i = 0; i < len; ++i) {
            dst[i] = f->regs[f->command[0]];
        }
        break;
    }
}

static void fake_handle_command(struct fake *f)
{
    switch (f->command[0]) {
    case 0x55:
        f->app_started = 0;
        break;
    case 0x08:
        f->app_started = 1;
        f->regs[0xA3] = 0x87;
        f->regs[0x9F] = 0x19;
        break;
    case 0xCC:
        f->ecc_len = ((size_t)f->command[4] << 16) |
                     ((size_t)f->command[5] << 8) | f->command[6];
        break;
    default:
        break;
    }
}

static void fake_handle_write(struct fake *f)
{
    switch (f->command[0]) {
    case 0xA5:
        if (f->data_len) {
            f->regs[0xA5] = f->data[0];
        }
        break;
    case 0xAE: {
        const size_t address = ((size_t)f->command[1] << 16) |
                               ((size_t)f->command[2] << 8) |
                               f->command[3];
        const size_t length = ((size_t)f->command[4] << 8) | f->command[5];
        const size_t count = length < f->data_len ? length : f->data_len;

        assert(address + count <= sizeof(f->image));
        memcpy(f->image + address, f->data, count);
        if (address + count > f->image_len) {
            f->image_len = address + count;
        }
        break;
    }
    default:
        break;
    }
}

static int fake_xfer(void *context, const uint8_t *tx, uint8_t *rx, size_t len,
                     unsigned first_delay_us)
{
    struct fake *f = context;

    ++f->transfers;
    if (first_delay_us == FTS_FIRST_BYTE_DELAY_US) {
        ++f->first_delays;
    }
    if (f->fail_next > 0) {
        --f->fail_next;
        return -1;
    }
    if (len < 1 || !tx) {
        return -1;
    }
    switch (tx[0]) {
    case FTS_PACKAGE_STATUS:
        if (len < 2) {
            return -1;
        }
        rx[1] = f->app_started ? FTS_STATUS_IDLE_FW : FTS_STATUS_IDLE_BOOT;
        return 0;
    case FTS_PACKAGE_COMMAND:
        if (len < 2) {
            return -1;
        }
        f->control = tx[1];
        f->command_len = f->control & 0x0F;
        if (2 + f->command_len > len) {
            return -1;
        }
        memset(f->command, 0, sizeof(f->command));
        memcpy(f->command, tx + 2, f->command_len);
        fake_handle_command(f);
        return 0;
    case FTS_PACKAGE_DATA: {
        if (len < 2) {
            return -1;
        }
        if (f->read_source || (f->control & 0x80)) {
            const size_t data_len = len - 3;
            uint16_t crc;

            if (len < 4 || data_len > FAKE_MAX_DATA) {
                return -1;
            }
            fake_fill_read(f, rx + 1, data_len);
            crc = fts_crc16_kermit(rx + 1, data_len);
            if (f->corrupt_crc) {
                crc ^= 0x0001;
            }
            rx[1 + data_len] = (uint8_t)crc;
            rx[2 + data_len] = (uint8_t)(crc >> 8);
            return 0;
        }
        f->data_len = len - 1;
        if (f->data_len > FAKE_MAX_DATA) {
            return -1;
        }
        memcpy(f->data, tx + 1, f->data_len);
        fake_handle_write(f);
        return 0;
    }
    default:
        return -1;
    }
}

static void fake_delay_us(void *context, unsigned us)
{
    (void)context;
    (void)us;
}

static void fake_delay_ms(void *context, unsigned ms)
{
    (void)context;
    (void)ms;
}

static void fake_reset(void *context, int asserted)
{
    struct fake *f = context;

    if (!asserted) {
        ++f->resets;
        f->app_started = 0;
        f->regs[0x90] = 0x87;
        f->regs[0x91] = 0xA9;
    }
}

static void fake_app(struct fake *f)
{
    memset(f, 0, sizeof(*f));
    f->app_started = 1;
    f->regs[0x90] = 0x87;
    f->regs[0x91] = 0xA9;
    f->regs[0xA3] = 0x87;
    f->regs[0x9F] = 0x19;
    f->regs[0xA6] = 0x42;
    f->control = FTS_CTRL_READ;
}

static void fake_boot(struct fake *f)
{
    memset(f, 0, sizeof(*f));
    f->app_started = 0;
    f->regs[0x90] = 0x87;
    f->regs[0x91] = 0xA9;
    f->regs[0xA3] = 0x00;
    f->regs[0x9F] = 0x00;
    f->control = FTS_CTRL_READ;
}

struct fixture {
    struct fake fake;
    struct fts_bus bus;
    struct fts_chip chip;
    uint8_t tx[FAKE_MAX_DATA + 16];
    uint8_t rx[FAKE_MAX_DATA + 16];
};

static void fixture_init(struct fixture *fixture, int app)
{
    if (app) {
        fake_app(&fixture->fake);
    } else {
        fake_boot(&fixture->fake);
    }
    fixture->bus = (struct fts_bus){&fixture->fake, fake_xfer, fake_delay_us,
                                    fake_delay_ms, fake_reset};
    assert(fts_chip_init(&fixture->chip, &fixture->bus, fixture->tx,
                         sizeof(fixture->tx), fixture->rx,
                         sizeof(fixture->rx)) == FTS_OK);
    fixture->chip.fw_running = app ? 1 : 0;
}

static void test_crc(void)
{
    static const uint8_t text[] = "123456789";

    // The vendor's variant uses the Kermit polynomial with a 0xFFFF initial
    // value, so the usual Kermit check value 0x2189 becomes 0x6F91.
    assert(fts_crc16_kermit(text, sizeof(text) - 1) == 0x6F91);
    assert(fts_crc16_kermit(NULL, 0) == 0xFFFF);
}

static void test_identify_and_registers(void)
{
    struct fixture fixture;
    uint16_t model = 0;
    uint8_t value = 0;

    fixture_init(&fixture, 1);
    assert(fts_identify(&fixture.chip, &model) == FTS_OK);
    assert(model == FTS_MODEL_FT8719);
    assert(fts_read_version(&fixture.chip, &value) == FTS_OK);
    assert(value == 0x42);
    assert(fts_write_reg(&fixture.chip, FTS_REG_POWER_MODE,
                         FTS_POWER_SLEEP) == FTS_OK);
    assert(fixture.fake.regs[FTS_REG_POWER_MODE] == FTS_POWER_SLEEP);
    assert(fts_read_reg(&fixture.chip, FTS_REG_POWER_MODE, &value) == FTS_OK);
    assert(value == FTS_POWER_SLEEP);
    assert(fixture.fake.first_delays == fixture.fake.transfers);
}

static void build_frame(uint8_t *frame, unsigned status, unsigned count,
                        unsigned id, unsigned event, unsigned x, unsigned y,
                        unsigned pressure, unsigned area)
{
    const size_t base = 2;

    memset(frame, 0, FTS_TOUCH_FRAME_LEN);
    frame[0] = (uint8_t)status;
    frame[1] = (uint8_t)count;
    frame[base + 0] = (uint8_t)((event << 6) | ((x >> 8) & 0x0F));
    frame[base + 1] = (uint8_t)x;
    frame[base + 2] = (uint8_t)((id << 4) | ((y >> 8) & 0x0F));
    frame[base + 3] = (uint8_t)y;
    frame[base + 4] = (uint8_t)pressure;
    frame[base + 5] = (uint8_t)(area << 4);
}

static void test_frame_decode(void)
{
    struct fixture fixture;
    uint8_t frame[FTS_TOUCH_FRAME_LEN];
    struct fts_point points[FTS_MAX_POINTS];
    int count;

    fixture_init(&fixture, 1);
    build_frame(frame, 0x91, 1, 3, 0, 0x123, 0x456, 0x30, 4);
    fixture.fake.read_source = frame;
    fixture.fake.read_source_len = sizeof(frame);
    assert(fts_read_report(&fixture.chip, frame, sizeof(frame)) ==
           FTS_REPORT_TOUCH);
    count = fts_frame_points(frame, sizeof(frame), points, FTS_MAX_POINTS);
    assert(count == 1);
    assert(points[0].id == 3);
    assert(points[0].event == 0);
    assert(points[0].x == 0x123);
    assert(points[0].y == 0x456);
    assert(points[0].pressure == 0x30);
    assert(points[0].area == 4);

    build_frame(frame, 0x91, 1, 3, 1, 0x124, 0x457, 0x10, 2);
    count = fts_frame_points(frame, sizeof(frame), points, FTS_MAX_POINTS);
    assert(count == 1);
    assert(points[0].event == 1);

    build_frame(frame, 0x00, 0, 0, 0, 0, 0, 0, 0);
    assert(fts_frame_points(frame, sizeof(frame), points, FTS_MAX_POINTS) ==
           FTS_ERR_STATE);
}

static void test_bad_crc_and_idle(void)
{
    struct fixture fixture;
    uint8_t frame[FTS_TOUCH_FRAME_LEN];

    fixture_init(&fixture, 1);
    build_frame(frame, 0x91, 1, 0, 0, 10, 20, 1, 1);
    fixture.fake.read_source = frame;
    fixture.fake.read_source_len = sizeof(frame);
    fixture.fake.corrupt_crc = 1;
    assert(fts_read_report(&fixture.chip, frame, sizeof(frame)) == FTS_ERR_CRC);
    fixture.fake.corrupt_crc = 0;

    build_frame(frame, 0x00, 0, 0, 0, 0, 0, 0, 0);
    fixture.fake.read_source = frame;
    fixture.fake.read_source_len = sizeof(frame);
    assert(fts_read_report(&fixture.chip, frame, sizeof(frame)) ==
           FTS_REPORT_IDLE);
}

static void test_recovery_markers(void)
{
    struct fixture fixture;
    uint8_t frame[FTS_TOUCH_FRAME_LEN];

    fixture_init(&fixture, 1);
    memset(frame, 0, sizeof(frame));
    frame[0] = 0x91;
    frame[1] = 0xEF;
    frame[2] = 0xEF;
    frame[3] = 0xEF;
    fixture.fake.read_source = frame;
    fixture.fake.read_source_len = sizeof(frame);
    assert(fts_read_report(&fixture.chip, frame, sizeof(frame)) ==
           FTS_REPORT_RECOVER);

    memset(frame, 0xFF, sizeof(frame));
    frame[0] = 0x9F;
    fixture.fake.read_source = frame;
    fixture.fake.read_source_len = sizeof(frame);
    assert(fts_read_report(&fixture.chip, frame, sizeof(frame)) ==
           FTS_REPORT_RECOVER);
}

static void test_firmware_check(void)
{
    uint8_t image[0x200];
    size_t length = 0;

    memset(image, 0x5A, sizeof(image));
    image[FTS_APP_INFO_OFFSET + 0] = 0x01;
    image[FTS_APP_INFO_OFFSET + 1] = 0x20;
    image[FTS_APP_INFO_OFFSET + 2] = 0xFE;
    image[FTS_APP_INFO_OFFSET + 3] = 0xDF;
    assert(fts_firmware_check(image, sizeof(image), &length) == FTS_OK);
    assert(length == 0x120);
    image[FTS_APP_INFO_OFFSET + 3] = 0x00;
    assert(fts_firmware_check(image, sizeof(image), &length) ==
           FTS_ERR_PARAM);
}

static void test_firmware_download(void)
{
    struct fixture fixture;
    uint8_t image[0x200];
    size_t i;

    memset(image, 0, sizeof(image));
    for (i = 0; i < 0x120; ++i) {
        image[i] = (uint8_t)(i * 7u + 3u);
    }
    image[FTS_APP_INFO_OFFSET + 0] = 0x01;
    image[FTS_APP_INFO_OFFSET + 1] = 0x20;
    image[FTS_APP_INFO_OFFSET + 2] = 0xFE;
    image[FTS_APP_INFO_OFFSET + 3] = 0xDF;

    fixture_init(&fixture, 0);
    fixture.fake.ecc_ok = FTS_ROMBOOT_ECC_OK;
    assert(fts_firmware_download(&fixture.chip, image, sizeof(image)) ==
           FTS_OK);
    assert(fixture.fake.app_started == 1);
    assert(fixture.fake.resets > 0);
    assert(fixture.fake.image_len == 0x120);
    assert(memcmp(fixture.fake.image, image, 0x120) == 0);
    assert(fixture.chip.fw_running == 1);
}

static void test_boot_identify(void)
{
    struct fixture fixture;
    uint16_t model = 0;

    fixture_init(&fixture, 0);
    assert(fts_boot_identify(&fixture.chip, &model) == FTS_OK);
    assert(model == FTS_MODEL_FT8719_ROM);
}

int main(void)
{
    test_crc();
    test_identify_and_registers();
    test_frame_decode();
    test_bad_crc_and_idle();
    test_recovery_markers();
    test_firmware_check();
    test_firmware_download();
    test_boot_identify();
    puts("fts-protocol: ok");
    return 0;
}
