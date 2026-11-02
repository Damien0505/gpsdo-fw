#!/bin/bash
# Builds the firmware protocol code for the PC against a fake HAL and runs its tests.
# Usage: CC="gcc" tools/host_test/run.sh     (any C11 compiler with -I and -lm)
set -e
cd "$(dirname "$0")"
STUBS=stubs
for h in main usart tim LCD ee stm32f1xx_hal stm32f1xx_hal_gpio stm32f1xx_hal_uart stm32f1xx_hal_rcc; do
    echo '#include "hal_stub.h"' > "$STUBS/$h.h"
done
OUT="${OUT:-./test_comm.exe}"
${CC:-gcc} -std=gnu11 -g -Wall -Wno-format -Wno-unused-function -Wno-unused-variable -Wno-unused-but-set-variable \
    -I"$STUBS" -I../../src \
    test_comm.c ../../src/comm.c ../../src/gps.c ../../src/menu.c ../../src/int.c ../../src/frequency.c \
    -lm -o "$OUT"
"$OUT"
