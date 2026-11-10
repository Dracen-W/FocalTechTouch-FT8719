// SPDX-License-Identifier: MIT
// FocalTech FT8719 host protocol over an abstract SPI transport.
//
// The wire behaviour was derived by reading the Linux vendor driver
// drivers/input/touchscreen/mediatek/FT8719P (GPL-2.0). No code, table or
// comment from that driver is copied; see NOTICE. This header contains facts
// only: package bytes, register numbers, the CRC polynomial and the touch
// record layout. Board addresses, GPIOs and clocks belong to the board layer.
#ifndef FTS_PROTOCOL_H
#define FTS_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>

// The controller reports at most ten contacts, each in a six-byte record.
#define FTS_MAX_POINTS 10
#define FTS_MAX_ID 0x0A
#define FTS_TOUCH_RECORD_LEN 6
// Status byte, contact count, then ten records.
#define FTS_TOUCH_REPORT_LEN (2 + FTS_MAX_POINTS * FTS_TOUCH_RECORD_LEN)
// The vendor reads the touch report and the 28-byte gesture tail in one
// transfer, so the chip's CRC covers all 90 bytes. Only the first 62 are
// touch data.
#define FTS_TOUCH_FRAME_LEN 90

// One SPI chip-select period carries a package byte followed by its payload.
#define FTS_PACKAGE_STATUS 0x05
#define FTS_PACKAGE_COMMAND 0xC0
#define FTS_PACKAGE_DATA 0x3F
#define FTS_CTRL_WRITE 0x00
#define FTS_CTRL_READ (0x80 | 0x20) // read, data CRC enabled
// The vendor caps a command package at twelve bytes including the two-byte
// length field it appends once firmware is running.
#define FTS_COMMAND_MAX 12

#define FTS_BUSY_RETRIES 100
#define FTS_BUSY_DELAY_US 150
#define FTS_CS_HIGH_DELAY_US 150
#define FTS_FIRST_BYTE_DELAY_US 30
#define FTS_STATUS_MASK 0x81
#define FTS_STATUS_IDLE_BOOT 0x00
#define FTS_STATUS_IDLE_FW 0x01

// Registers used by this driver.
#define FTS_REG_WORKMODE 0x00
#define FTS_REG_CHIP_ID 0xA3
#define FTS_REG_CHIP_ID2 0x9F
#define FTS_REG_POWER_MODE 0xA5
#define FTS_REG_FW_VER 0xA6
#define FTS_REG_CHARGER_MODE 0x8B
#define FTS_REG_GLOVE_MODE 0xC0
#define FTS_REG_COVER_MODE 0xC1
#define FTS_REG_GESTURE 0xD0
#define FTS_POWER_ACTIVE 0x00
#define FTS_POWER_SLEEP 0x03

// Bootloader and application commands.
#define FTS_CMD_START 0x55
#define FTS_CMD_READ_ID 0x90
#define FTS_CMD_START_APP 0x08
#define FTS_ROMBOOT_SET_PRAM_ADDR 0xAD
#define FTS_ROMBOOT_WRITE 0xAE
#define FTS_ROMBOOT_ECC 0xCC
#define FTS_ROMBOOT_ECC_FINISH 0xCE
#define FTS_ROMBOOT_ECC_READ 0xCD
#define FTS_ROMBOOT_ECC_OK 0x00

#define FTS_PRAM_BASE 0x000000
#define FTS_APP_INFO_OFFSET 0x100
// Sized so 1 package byte + FTS_FW_CHUNK <= 1024 (MediaTek SPI single-packet
// hardware DMA limit MTK_SPI_PACKET_SIZE = 1024).
#define FTS_FW_CHUNK (1024 - 4)
#define FTS_FW_MAX (64 * 1024)
#define FTS_FW_MIN 0x120
#define FTS_FW_RETRIES 3
#define FTS_ECC_RETRIES 100

// Identification values. The application reports 0x8719; the bootloader may
// report the ROM, PB or BL alias for the same die.
#define FTS_MODEL_FT8719 0x8719
#define FTS_MODEL_FT8719_ROM 0x87A9
#define FTS_MODEL_FT8719_PB 0x87B9

// Results. Negative values are errno-compatible for diagnostics only.
#define FTS_OK 0
#define FTS_ERR_BUS (-5)
#define FTS_ERR_PARAM (-22)
#define FTS_ERR_CRC (-74)
#define FTS_ERR_TIMEOUT (-110)
#define FTS_ERR_CHIP (-19)
#define FTS_ERR_STATE (-71)

// fts_read_report classifications.
#define FTS_REPORT_TOUCH 0
#define FTS_REPORT_IDLE 1
#define FTS_REPORT_RECOVER 2

// Transport. One call is one chip-select period: the bus sends tx[0], waits
// first_delay_us, then sends tx[1..len). rx may be NULL for a write. The caller
// owns the buffers and serialises all access to the chip object.
struct fts_bus {
    void *ctx;
    int (*xfer)(void *ctx, const uint8_t *tx, uint8_t *rx, size_t len,
                unsigned first_delay_us);
    void (*delay_us)(void *ctx, unsigned us);
    void (*delay_ms)(void *ctx, unsigned ms);
    // Optional. asserted=1 drives the controller reset line low, 0 releases it.
    void (*reset)(void *ctx, int asserted);
};

struct fts_chip {
    const struct fts_bus *bus;
    uint8_t *tx;
    size_t tx_size;
    uint8_t *rx;
    size_t rx_size;
    // The vendor enables the two-byte length field in commands only after it
    // has downloaded and started firmware in this boot. Keep the same meaning.
    int fw_running;
    uint8_t last_status;
    // Diagnostics for a failed bring-up.
    unsigned transfer_errors, crc_errors, idle_polls;
    int last_error;
};

struct fts_point {
    uint8_t id;
    uint8_t event; // 0 down, 1 up, 2 contact
    uint16_t x, y;
    uint8_t pressure, area;
};

int fts_chip_init(struct fts_chip *chip, const struct fts_bus *bus,
                  uint8_t *tx, size_t tx_size, uint8_t *rx, size_t rx_size);

// CRC-16/Kermit (reflected polynomial 0x8408, initial value 0xFFFF).
uint16_t fts_crc16_kermit(const uint8_t *data, size_t len);

// Generic transfers. cmd may be NULL for a data-only read (touch report).
int fts_write(struct fts_chip *chip, const uint8_t *cmd, size_t cmdlen,
              const uint8_t *data, size_t datalen);
int fts_read(struct fts_chip *chip, const uint8_t *cmd, size_t cmdlen,
             uint8_t *data, size_t datalen);
int fts_write_reg(struct fts_chip *chip, uint8_t addr, uint8_t value);
int fts_read_reg(struct fts_chip *chip, uint8_t addr, uint8_t *value);

// Identification and power.
int fts_read_app_id(struct fts_chip *chip, uint16_t *model);
int fts_identify(struct fts_chip *chip, uint16_t *model);
int fts_boot_identify(struct fts_chip *chip, uint16_t *model);
int fts_enter_boot(struct fts_chip *chip);
int fts_wait_ready(struct fts_chip *chip, unsigned timeout_ms);
int fts_read_version(struct fts_chip *chip, uint8_t *version);
int fts_set_power_mode(struct fts_chip *chip, uint8_t mode);
int fts_sleep(struct fts_chip *chip);
int fts_wake(struct fts_chip *chip);

// Read one report and classify it. Returns FTS_REPORT_* or a negative error.
// The frame buffer receives FTS_TOUCH_FRAME_LEN bytes on a successful bus read,
// including on a CRC error, so the caller can inspect recovery markers.
int fts_read_report(struct fts_chip *chip, uint8_t *frame, size_t len);

// Decode the touch records. Returns the number of points, or a negative error.
int fts_frame_points(const uint8_t *frame, size_t len,
                     struct fts_point *points, int max);

// Optional firmware download. The image is supplied by the device repository;
// this repository contains no vendor firmware.
int fts_firmware_check(const uint8_t *image, size_t size, size_t *code_len);
uint16_t fts_firmware_ecc(const uint8_t *data, size_t len);
int fts_firmware_download(struct fts_chip *chip, const uint8_t *image,
                          size_t size);

#endif
