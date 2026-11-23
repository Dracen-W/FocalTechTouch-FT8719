// SPDX-License-Identifier: MIT
// merlinx (Redmi Note 9 4G, MT6768) board facts for the FT8719.
// All board defaults may be overridden via CONFIG_XNU_DW_FOCALTECH_* at build time.
#ifndef FTS_BOARD_H
#define FTS_BOARD_H

#include "FtsTouchCore.h"

#ifdef CONFIG_XNU_DW_FOCALTECH_SPI_HZ
#define FTS_BOARD_SPI_HZ ((unsigned)(CONFIG_XNU_DW_FOCALTECH_SPI_HZ))
#else
#define FTS_BOARD_SPI_HZ 8000000u
#endif

#ifdef CONFIG_XNU_DW_FOCALTECH_SPI_CS
#define FTS_BOARD_SPI_CS (CONFIG_XNU_DW_FOCALTECH_SPI_CS)
#else
#define FTS_BOARD_SPI_CS 0
#endif

#ifdef CONFIG_XNU_DW_FOCALTECH_RESET_GPIO
#define FTS_BOARD_RESET_GPIO (CONFIG_XNU_DW_FOCALTECH_RESET_GPIO)
#else
#define FTS_BOARD_RESET_GPIO 92
#endif
#define FTS_BOARD_RESET_ACTIVE_LOW 1

#ifdef CONFIG_XNU_DW_FOCALTECH_IRQ_GPIO
#define FTS_BOARD_IRQ_GPIO (CONFIG_XNU_DW_FOCALTECH_IRQ_GPIO)
#else
#define FTS_BOARD_IRQ_GPIO 1
#endif
#define FTS_BOARD_IRQ_FLAGS 0x2001u

#ifdef CONFIG_XNU_DW_FOCALTECH_X_MAX
#define FTS_BOARD_X_MAX (CONFIG_XNU_DW_FOCALTECH_X_MAX)
#else
#define FTS_BOARD_X_MAX 1080
#endif

#ifdef CONFIG_XNU_DW_FOCALTECH_Y_MAX
#define FTS_BOARD_Y_MAX (CONFIG_XNU_DW_FOCALTECH_Y_MAX)
#else
#define FTS_BOARD_Y_MAX 2340
#endif

#ifdef CONFIG_XNU_DW_FOCALTECH_MAX_POINTS
#define FTS_BOARD_MAX_POINTS (CONFIG_XNU_DW_FOCALTECH_MAX_POINTS)
#else
#define FTS_BOARD_MAX_POINTS 10
#endif

#ifdef CONFIG_XNU_DW_FOCALTECH_SWAP_XY
#define FTS_BOARD_SWAP_XY (CONFIG_XNU_DW_FOCALTECH_SWAP_XY)
#else
#define FTS_BOARD_SWAP_XY 0
#endif

#ifdef CONFIG_XNU_DW_FOCALTECH_INVERT_X
#define FTS_BOARD_INVERT_X (CONFIG_XNU_DW_FOCALTECH_INVERT_X)
#else
#define FTS_BOARD_INVERT_X 0
#endif

#ifdef CONFIG_XNU_DW_FOCALTECH_INVERT_Y
#define FTS_BOARD_INVERT_Y (CONFIG_XNU_DW_FOCALTECH_INVERT_Y)
#else
#define FTS_BOARD_INVERT_Y 0
#endif

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
