// SPDX-License-Identifier: MIT
// merlinx (Redmi Note 9 4G, MT6768) board facts for the FT8719.
//
// Every value names its source. The active touch node is the Novatek-named
// node because merlin.dts shares one bus node between the two panels:
//
//   &spi0 {
//       novatek@0 {
//           compatible = "novatek,NVT-ts-spi","focaltech,fts";
//           reg = <0>;
//           spi-max-frequency = <8000000>;
//           novatek,reset-gpio = <&pio 92 0x00>;
//           novatek,irq-gpio = <&pio 1 0x2001>;
//       };
//   };
//
// The separate focaltech_spi@1 node in the same file is commented out, so the
// FT8719 sits on SPI0 chip select 0. The panel geometry is in the &touch node:
// tpd-resolution = <1080 2340> and tpd-max-touch-num = <10>.
#ifndef FTS_BOARD_H
#define FTS_BOARD_H

#include "FtsTouchCore.h"

// merlin.dts &spi0: spi-max-frequency = <8000000>; the comment says this is
// the FT8719 limit. Node reg = <0> selects chip select 0.
#define FTS_BOARD_SPI_HZ 8000000u
#define FTS_BOARD_SPI_CS 0

// merlin.dts novatek,reset-gpio = <&pio 92 0x00>: GPIO 92, active low.
#define FTS_BOARD_RESET_GPIO 92
#define FTS_BOARD_RESET_ACTIVE_LOW 1

// merlin.dts novatek,irq-gpio = <&pio 1 0x2001>: GPIO 1, rising edge,
// active high (the MediaTek pinctrl flag bits).
#define FTS_BOARD_IRQ_GPIO 1
#define FTS_BOARD_IRQ_FLAGS 0x2001u

// merlin.dts &touch: tpd-resolution = <1080 2340>.
#define FTS_BOARD_X_MAX 1080
#define FTS_BOARD_Y_MAX 2340
// merlin.dts &touch: tpd-max-touch-num = <10>.
#define FTS_BOARD_MAX_POINTS 10

// The panel is mounted in its natural orientation on merlinx. Change these
// only with a photograph of the touch axes, not by guessing.
#define FTS_BOARD_SWAP_XY 0
#define FTS_BOARD_INVERT_X 0
#define FTS_BOARD_INVERT_Y 0

static inline struct fts_core_config fts_board_core_config(void)
{
    struct fts_core_config config;

    config.raw_x_max = FTS_BOARD_X_MAX;
    config.raw_y_max = FTS_BOARD_Y_MAX;
    config.swap_xy = FTS_BOARD_SWAP_XY;
    config.invert_x = FTS_BOARD_INVERT_X;
    config.invert_y = FTS_BOARD_INVERT_Y;
    return config;
}

#endif
