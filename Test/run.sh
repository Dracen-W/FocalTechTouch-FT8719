#!/bin/sh
# SPDX-License-Identifier: MIT
# Host tests for the pure layers. No XNU headers are needed.
set -eu
cd "$(dirname "$0")/.."

CC="${CC:-cc}"
CFLAGS="${CFLAGS:--std=c99 -Wall -Wextra -Werror -pedantic -I.}"
mkdir -p build

$CC $CFLAGS FtsProtocol.c FtsTouchCore.c Test/fts-protocol.c -o build/fts-protocol
$CC $CFLAGS FtsProtocol.c FtsTouchCore.c Test/touch-core.c -o build/touch-core
$CC $CFLAGS FtsHid.c Test/fts-hid.c -o build/fts-hid

./build/fts-protocol
./build/touch-core
./build/fts-hid
