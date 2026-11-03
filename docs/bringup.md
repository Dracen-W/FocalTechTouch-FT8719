# FT8719 bring-up and hardware validation

[English](bringup.md) | [简体中文](bringup.zh-CN.md)

This document is the operational counterpart to `README.md`. It describes what
to check before touching a real merlinx, how to run the staged probe, how to
read the diagnostic page, and what still needs to be proven on hardware.

The short version: the protocol, touch core and HID snapshot are host-tested;
the XNU probe is syntax-checked; **nothing in this repository has been run on a
real merlinx yet**. Treat the first boot as an experiment.

## What is already verified

| Item | How it was checked | Result |
| --- | --- | --- |
| Package framing, CRC, register access | `Test/fts-protocol.c` with a fake FT8719 bus | pass |
| Boot identification, app identification | fake bus | pass |
| Firmware length check, PRAM write, ECC, start | fake bus | pass |
| Report decoding, recovery markers | fake bus | pass |
| Contact lifecycle, masks, release, mapping | `Test/touch-core.c` | pass |
| Seqlock snapshot | `Test/fts-hid.c` | pass |
| XNU API use in `FtsProbe.cpp` | `clang++ -fsyntax-only` against the macOS `Kernel.framework` headers | no errors or warnings |
| Real SPI engine, GPIOs, IRQ, panel | **not run** | pending |

Run the host suite with:

```sh
./Test/run.sh
```

Expected output:

```
fts-protocol: ok
touch-core: ok
fts-hid: ok
```

## Before the first boot

1. **Make sure the board really has an FT8719 on the bus.** The merlinx
   devicetree reuses a Novatek-named SPI node and lists `focaltech,fts` as a
   compatible string. Do not assume the node name selects the controller.
2. **Make sure the board's SPI engine works on its own.** Run the board's
   loopback or read-only transaction test first. This repository does not
   contain a MediaTek SPI driver.
3. **Confirm reset polarity and the IRQ line.** `FtsBoard.h` says reset is GPIO
   92 active low and IRQ is GPIO 1 with flags `0x2001`. Re-check these against
   the devicetree for the exact board revision.
4. **Decide whether firmware is allowed.** The repository contains no image.
   If the board does not provide `fts_board_firmware`, stage 2 cannot recover a
   controller that is stuck in its bootloader.
5. **Keep the probe off by default.** Do not add `fts-touch=` to the default
   boot arguments until stage 1 has passed.

## Boot arguments

| Argument | Effect |
| --- | --- |
| `fts-touch=1` | Reset, identify the controller, stop. Read-only. |
| `fts-touch=2` | Also download firmware when the model is not `0x8719`, then read the version. |
| `fts-touch=3` | Also run the frame loop and publish touches. |
| `fts-touch-poll=1` | Poll instead of using `fts_board_irq_wait`. |
| `fts-touch-swap=1` | Override the board axis swap. |
| `fts-touch-invert-x=1` | Override X inversion. |
| `fts-touch-invert-y=1` | Override Y inversion. |

The probe is started by the board calling `fts_touch_probe(board)` once the bus,
reset line and interrupt are usable. The call starts a kernel thread and
returns immediately.

## Stage 1: identify only

Use this first:

```
fts-touch=1
```

Expected diagnostic page:

```
FTS stage 1/1 step 1 err 0 id8719 ver00
FTS fw1 tx0 crc0 poll0 last0 rc0
FTS frames0 reads0 idle0 rec0 bad0 c0
FTS last -1,-1 rel0 dup0 range0 drop0
```

What the fields mean:

- `stage <done>/<requested>`: stage progress. `stage 1/1` means stage 1
  completed.
- `step <n>`: the step that was running or failed. `step 1` is bus/chip setup
  and identification.
- `err <n>`: first error, `0` on success. Negative values are errno-style.
- `id<hhhh>`: identified model. `8719` is the application firmware. `87a9` and
  `87b9` are ROM/PB aliases that mean "the bootloader is running".
- `ver<hh>`: firmware version, read at stage 2.
- `fw<n>`: framing. `1` means commands carry the two-byte length field.
- `tx/crc/poll/last/rc`: transfer errors, CRC errors, last idle-poll count,
  last protocol error, last report classification.

If stage 1 returns `id87a9` or `id87b9`, the controller is alive but its
application firmware is not running. Do not continue to stage 3; go to stage 2
only if a board-supplied image is intentional.

### Stage 1 failure hints

| Symptom | Likely cause |
| --- | --- |
| `err -5` at `step 1`, `tx` grows | SPI transfer fails: clock, pinmux, CS, or the board callback returns an error |
| `err -110` | Status polls never reach idle or identification never answers: wrong SPI mode, wrong reset polarity, or the controller is held in reset |
| `err -19` (`FTS_ERR_CHIP`) | The bus works but the id is not an FT8719 alias: wrong panel node, wrong chip select, or a different controller |
| `err -74` with `crc` growing | Reads return data but the CRC does not match: wrong read length field, wrong first-byte delay, or signal integrity |
| `err -22` | A buffer or argument is wrong; check `kBusBytes` and the board transfer length |

## Stage 2: firmware and version

Only run this with a board-supplied image and a known-good image for the panel:

```
fts-touch=2
```

The probe downloads only when the identified model is not `0x8719`. The
sequence is the vendor's RAM download path for this part: enter the bootloader,
write the PRAM image in `4K-4` chunks with `0xAE` address/length commands, ask
the bootloader to compute ECC, compare it with the host ECC, then start the
application with `0x08`.

Expected result after a successful download:

```
FTS stage 2/2 step 2 err 0 id8719 ver42
```

Cautions:

- A failed download can leave the controller in its bootloader. Keep the
  firmware image and the board recovery path available.
- Do not use an image from a different panel or a different FT8719 vendor.
- The repository deliberately does not embed, fetch or generate firmware.
- The driver uses the conservative slow packet size. It does not run the
  vendor's fast-download negotiation.

## Stage 3: frame loop

Only after stages 1 and 2 pass:

```
fts-touch=3
```

Expected behaviour:

- `FTS frames<n>` increases while the panel is touched.
- `reads` counts touch reports, `idle` counts empty polls, `rec` counts recovery
  events, `bad` counts protocol errors.
- `c<n>` is the number of entries in the last published report, including lift
  entries.
- `last x,y` follows the first reported contact.
- `rel/dup/range/drop` are the touch core's diagnostics. They should stay at
  zero during normal use. `rel` grows when a contact disappears from a complete
  report; `dup` on a repeated hardware id; `range` on an id above 9; `drop` on
  an invalid event or an up for an inactive id.

If the interrupt path is suspect, compare with `fts-touch-poll=1`. Polling is
slower and uses more CPU, but it isolates the IRQ wiring from the SPI path.

## Hardware validation checklist

Record the result of each item. Do not mark the driver validated until all of
them pass on a real merlinx.

- [ ] Stage 1 identifies `id8719`.
- [ ] Stage 1 leaves `tx`, `crc`, `bad` at zero after several minutes.
- [ ] Stage 2 reads a plausible firmware version (`ver` is not `0x00`/`0xFF`).
- [ ] Stage 3 reports a single finger at the expected panel position.
- [ ] A second finger produces a second contact with a distinct hardware id.
- [ ] Ten fingers are reported (or the panel's real maximum) without lost ids.
- [ ] Lift reports arrive; no finger stays stuck after release.
- [ ] Dragging produces continuous coordinates with no axis swap or inversion.
- [ ] The coordinate reported at the top-left and bottom-right corners matches
      the panel orientation; adjust `FtsBoard.h` only from a photograph or a
      recorded corner test.
- [ ] `rel`, `dup`, `range`, `drop` stay at zero during normal use.
- [ ] `suspend` releases all contacts and `resume` restores touch without a
      reboot.
- [ ] A long idle period (30+ minutes) produces no recovery storm.
- [ ] A deliberate bus fault or reset recovers through the frame loop.
- [ ] The diagnostic page remains readable and does not stall the touch thread.

## Reading failures

`fts_touch_error` keeps the first error. `fts_touch_step` keeps the step that
failed. The steps are:

| Step | Meaning |
| --- | --- |
| 0 | Session allocation |
| 1 | Buffer allocation, bus setup, identification |
| 2 | Firmware download, version read, core configuration |
| 3 | Frame loop start |

Error values:

| Value | Name | Meaning |
| --- | --- | --- |
| `0` | `FTS_OK` | success |
| `-5` | `FTS_ERR_BUS` | the board transfer callback failed |
| `-19` | `FTS_ERR_CHIP` | the id is not an FT8719 alias |
| `-22` | `FTS_ERR_PARAM` | invalid argument or buffer size |
| `-71` | `FTS_ERR_STATE` | a report or state is inconsistent |
| `-74` | `FTS_ERR_CRC` | read CRC mismatch |
| `-110` | `FTS_ERR_TIMEOUT` | status or identification timeout |
| `-2` | no firmware | stage 2 needed an image but the board supplied none |

## Suspend and resume

`fts_touch_suspend()` and `fts_touch_resume()` are valid only after stage 3 has
started. Before that they are no-ops. The suspend path sets a flag, waits for
the frame thread to release all contacts and acknowledge, then writes
`0xA5=0x03`. Resume first waits for the controller to answer identification
reads again; only if that fails does it write `0xA5=0x00`, and only then does it
reset. This mirrors the vendor's resume order rather than guessing.

Do not call these from interrupt context. Do not call suspend while a firmware
download is in progress.

## Known limitations

- The probe is a bring-up and validation driver, not a finished HID driver. The
  board's HID source reads the seqlock snapshot and owns the report descriptor.
- Only the FT8719 is supported. The ROM/PB aliases are accepted for
  identification, not as a substitute for application firmware.
- The driver always uses the conservative firmware packet size. Fast download
  is not negotiated.
- There is no ESD workqueue and no gesture, glove, cover, charger or proximity
  handling. Those vendor features are out of scope.
- `FtsProbe.cpp` has not been linked into a real XNU build. The syntax check
  used the macOS `Kernel.framework` headers; a board XNU tree may still expose a
  different API surface.

## Rollback

Remove the `fts-touch=` boot argument. Without it, `fts_touch_probe()` returns
without starting a thread and touches the hardware not at all. If a build is
already installed, the probe is still inert by default.

## Reporting a result

When recording a hardware run, include:

- board revision and panel;
- XNU commit and board SPI driver commit;
- the exact boot arguments;
- the four diagnostic rows at stage 1, stage 2 and after ten minutes of stage 3;
- whether the run used IRQ or polling;
- a photograph or corner-test coordinates for orientation;
- any kernel log lines from the board transport.

Do not report "works" from the host tests alone.
