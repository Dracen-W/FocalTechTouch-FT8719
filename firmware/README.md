# Firmware

[English](README.md) | [简体中文](README.zh-CN.md)

`focaltech_ts_fw_xinli.bin` is the FT8719 application image for the merlinx
panel.

| Field | Value |
| --- | --- |
| Size | 131072 bytes (128 KiB) |
| SHA-256 | `05296053fd79eb961da2ecfc3b0ff88b93a7fc431f71065d8584b449bcb7a30a` |
| Layout | two 64 KiB application slots |
| Slot A | header at `0x100`, code length `0xF124` |
| Slot B | header at `0x10100`, code length `0xE504` |

The file came from the device itself, at
`/vendor/firmware/focaltech_ts_fw_xinli.bin`. That is the path the vendor kernel
driver requests for this panel.

## Why it is needed

The FT8719 on merlinx does not run an application from on-chip flash. The host
must write the application into the controller's program RAM after every reset,
verify it with the ROM ECC command, and start it. That is the only upgrade path
the vendor driver has for this part, and it runs on every boot.

Without the image the controller stays in its ROM. Note that this part reports
`0x8719` in both ROM and application mode, so an identification read alone does
not prove that the application is running.

## Format

Each 64 KiB slot starts with the standard FocalTech application header at
offset `0x100`:

| Offset | Meaning |
| --- | --- |
| `0x100` | code length, big endian |
| `0x102` | bitwise complement of the code length |
| `0x108` | constant-block length |
| `0x10A` | bitwise complement of the constant-block length |

Both slots carry a length pair that sums to `0xFFFF`. The driver reads slot A,
validates the pair, writes exactly `code_len` bytes to PRAM from offset `0`, and
ignores slot B. That matches the vendor driver, which also writes `code_len`
bytes from the beginning of the file and never touches the second slot.

Slot B is kept because it is part of the image the device ships; removing it
would make the file harder to compare against a known-good copy.

## How the board consumes it

This repository does not link the blob into anything. The probe takes it through
two weak symbols that the board repository defines:

```c
extern const uint8_t  fts_board_firmware[];
extern const uint32_t fts_board_firmware_size;
```

Generate them from this file at board build time, for example:

```sh
xxd -i -n fts_board_firmware firmware/focaltech_ts_fw_xinli.bin \
    > build/fts_firmware_blob.c
```

and define `fts_board_firmware_size` as `sizeof(fts_board_firmware)` in the same
translation unit. Keep the generated `.c` in the board build directory, not in
this repository.

## Licensing

This image is proprietary FocalTech/ODM firmware. It is **not** covered by the
MIT license in `LICENSE`, and this project grants no right to redistribute it.
It is present here only so that a merlinx owner has the exact image that matches
both the panel and the driver. Remove it if you redistribute the sources
publicly without permission; the driver works without it as long as the board
supplies an equivalent image.
