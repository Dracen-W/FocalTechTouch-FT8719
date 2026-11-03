# UniXNU FT8719 FocalTech Touch

[English](README.md) | [简体中文](README.zh-CN.md)

An MIT-licensed, board-agnostic host driver for the FocalTech FT8719 touch
controller, written for XNU and wired for the merlinx board (Redmi Note 9 4G,
MediaTek MT6768).

The repository has three separable layers:

| Layer | Files | Depends on |
| --- | --- | --- |
| Wire protocol | `FtsProtocol.h`, `FtsProtocol.c` | C99 only, no XNU |
| Touch core | `FtsTouchCore.h`, `FtsTouchCore.c` | C99 only, no XNU |
| HID snapshot | `FtsHid.h`, `FtsHid.c` | C99 only, no XNU |
| Board facts | `FtsBoard.h` | merlinx devicetree values |
| XNU probe | `FtsProbe.cpp` | XNU kernel headers |

The protocol and core layers are host-tested with a fake bus. The XNU probe is
deliberately thin: it does not contain a MediaTek SPI engine, GPIO driver or
clock driver. The board repository supplies those through weak symbols.

The panel firmware that the host has to load is in `firmware/`; it is
proprietary and not covered by the MIT license. See `NOTICE` for provenance.

## Status

- Protocol, touch core and HID snapshot: covered by host tests.
- XNU probe: syntax-checked against the macOS `Kernel.framework` headers, but
  not yet linked into a real XNU build. It must be built inside the XNU source
  tree or the board's kernel-extension build.
- Hardware: not validated in this repository. Bring-up is staged and off by
  default.

## Documentation

- [`docs/bringup.md`](docs/bringup.md) - safety rules, staged probe, diagnostic
  page, failure triage and the real-hardware validation checklist.
- [`docs/bringup.zh-CN.md`](docs/bringup.zh-CN.md) - 中文版本。

## Protocol summary

The FT8719 speaks a package protocol over SPI. One chip-select period carries
one package:

| Package | First byte | Payload |
| --- | --- | --- |
| Status | `0x05` | one status byte |
| Command | `0xC0` | control byte, then command bytes |
| Data | `0x3F` | payload, and a two-byte CRC on reads |

The control byte is `0x00` for a write and `0xA0` for a read with data CRC.
Its low nibble is the command length. Once the application firmware is running,
reads and writes append a two-byte big-endian data length to the command. The
controller needs a 30 µs gap after the first byte of every transfer, and
`0x05` status polling must observe idle before each package.

Read data is protected by CRC-16/Kermit with reflected polynomial `0x8408` and
initial value `0xFFFF` (check value `0x6F91` for `123456789`).

A touch report is a status byte, a contact count, and up to ten six-byte
records. The controller is read as 90 bytes because the vendor reads the
28-byte gesture tail in the same transfer; the CRC covers all 90 bytes. Only
the first 62 bytes are touch data.

## Bus contract

`FtsProtocol` does not know how SPI is implemented. It calls:

```c
struct fts_bus {
    void *ctx;
    int  (*xfer)(void *ctx, const uint8_t *tx, uint8_t *rx,
                 size_t len, unsigned first_delay_us);
    void (*delay_us)(void *ctx, unsigned us);
    void (*delay_ms)(void *ctx, unsigned ms);
    void (*reset)(void *ctx, int asserted);
};
```

`xfer` is one chip-select period: send `tx[0]`, wait `first_delay_us`, then
send `tx[1..len)`. `rx` may be `NULL` for a write. The board owns the buffers
and serialises access to the chip object. The protocol layer calls `xfer`
synchronously and never from an interrupt.

The `CD_PACKAGE_BUFLEN` prefix in the vendor driver is a host-buffer artifact;
only `tx[0]` and the payload after the prefix reach the wire, so this contract
is wire-equivalent.

## Board integration

The board repository provides these weak symbols:

```c
int  fts_board_spi_transfer(void *context, const uint8_t *tx, uint8_t *rx,
                            size_t len, unsigned first_delay_us);
void fts_board_delay_us(void *context, unsigned us);
void fts_board_delay_ms(void *context, unsigned ms);
void fts_board_reset(void *context, int asserted);   // 1 = reset low
int  fts_board_irq_wait(void *context, unsigned timeout_ms);
```

`fts_board_irq_wait` returns `0` when a frame is ready, `FTS_ERR_TIMEOUT` when
the timeout expires, and another negative value on a board error. It is
optional; without it, the probe polls.

The board also provides the firmware image:

```c
extern const uint8_t  fts_board_firmware[];
extern const uint32_t fts_board_firmware_size;
```

`firmware/focaltech_ts_fw_xinli.bin` is the merlinx image. It is not linked
automatically: the board converts it to a C array at build time and defines both
symbols. A build without it can still identify the controller, but this part
runs from program RAM, so a cold boot without the download leaves the
application stopped. See [`firmware/README.md`](firmware/README.md).

Entry points:

```c
int fts_touch_probe(void *board);   // call once the bus is usable
int fts_touch_suspend(void);        // call from a thread, stage 3 only
int fts_touch_resume(void);
```

`fts_touch_probe` starts a kernel thread and returns immediately. It does
nothing unless the `fts-touch` boot argument is present, so adding the driver
does not change boot behaviour.

### Diagnostic page

The board's boot log can read:

```c
char fts_touch_rows[4][120];
volatile uint32_t fts_touch_stage_req, fts_touch_stage_done;
volatile uint32_t fts_touch_step, fts_touch_frames, fts_touch_reads;
volatile uint32_t fts_touch_idle, fts_touch_recover, fts_touch_bad;
volatile uint32_t fts_touch_contacts, fts_touch_model, fts_touch_version;
volatile int fts_touch_error;
volatile int fts_touch_last_x, fts_touch_last_y;
```

## Boot arguments

| Argument | Meaning |
| --- | --- |
| `fts-touch=1` | Reset, identify the controller, stop |
| `fts-touch=2` | Also load `firmware/` into the controller and read the version |
| `fts-touch=3` | Also start the frame loop and publish touches |
| `fts-touch-poll=1` | Poll instead of using `fts_board_irq_wait` |
| `fts-touch-swap=1` | Override the board axis swap |
| `fts-touch-invert-x=1` | Override X inversion |
| `fts-touch-invert-y=1` | Override Y inversion |

Stage 1 is the safe first boot. It performs a read-only identification. Stage 2
writes the application image into the controller's program RAM. On this part the
download is not an upgrade: the controller does not boot from on-chip flash, so
stage 2 is what makes the panel usable after a reset.

## HID output

`FtsHid.c` publishes the latest contact set through a seqlock:

```c
extern volatile uint32_t fts_touch_seq;
extern volatile uint32_t fts_touch_count;
extern volatile uint32_t fts_touch_x_max;
extern volatile uint32_t fts_touch_y_max;
extern volatile int32_t  fts_touch_xs[FTS_HID_MAX_POINTS];
extern volatile int32_t  fts_touch_ys[FTS_HID_MAX_POINTS];
extern volatile uint32_t fts_touch_ids[FTS_HID_MAX_POINTS];
extern volatile uint32_t fts_touch_tips[FTS_HID_MAX_POINTS];
```

Coordinates are panel pixels. The maxima travel with the snapshot so the HID
report descriptor can normalise without hard-coded panel constants. A reader
may use `fts_hid_read()` or its own seqlock loop. A sink can be registered with
`fts_hid_set_sink()`.

## merlinx facts

From the merlinx devicetree:

- SPI0, chip select 0, 8 MHz (`spi-max-frequency = <8000000>`).
- Reset on GPIO 92, active low.
- Interrupt on GPIO 1, flags `0x2001`.
- Panel `1080 x 2340`, 10 contacts.
- No axis swap or inversion by default.

The devicetree node is Novatek-named because merlin shares one bus node between
panels; the same node lists `focaltech,fts`. See `FtsBoard.h` for the exact
source comments.

## Firmware

| Field | Value |
| --- | --- |
| File | `firmware/focaltech_ts_fw_xinli.bin` |
| Size | 131072 bytes, two 64 KiB application slots |
| SHA-256 | `05296053fd79eb961da2ecfc3b0ff88b93a7fc431f71065d8584b449bcb7a30a` |
| Source | `/vendor/firmware/focaltech_ts_fw_xinli.bin` on the device |

The driver uses slot A only: it validates the length pair at `0x100` and writes
`0xF124` bytes to PRAM, which is exactly what the vendor driver does. Slot B is
part of the shipped image and is kept for byte-for-byte comparison.

This binary is proprietary and is not covered by `LICENSE`. See
[`firmware/README.md`](firmware/README.md) for the format, the checksum and the
build-time glue that turns it into `fts_board_firmware[]`.

## Build and test

The pure layers need only a C99 compiler:

```sh
./Test/run.sh
```

This builds `FtsProtocol.c`, `FtsTouchCore.c`, `FtsHid.c` and the tests in
`Test/`, then runs:

```
fts-protocol: ok
touch-core: ok
fts-hid: ok
```

`FtsProbe.cpp` is compiled as part of the board's XNU build, with the repository
root on the include path. It expects `FtsBoard.h` to describe the board; copy or
override that header for a different device.

## License

MIT. See `LICENSE`. Every source file carries
`SPDX-License-Identifier: MIT`. The vendor driver was read for wire behaviour
only; see `NOTICE` for the GPL boundary and the firmware policy.
