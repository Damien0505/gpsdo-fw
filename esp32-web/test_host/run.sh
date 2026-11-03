#!/bin/bash
# Builds the ESP32 core logic for the PC and runs its tests.
#   ARDUINOJSON  directory containing ArduinoJson.h: .pio/libdeps/esp32c3/ArduinoJson/src after a "pio run"
#   CXX          any C++17 compiler
#   OUT          where to put the test program
#   PYTHON       to run the simulator
set -e
cd "$(dirname "$0")"
ARDUINOJSON="${ARDUINOJSON:-../.pio/libdeps/esp32c3/ArduinoJson/src}"
OUT="${OUT:-./test_core.exe}"
PYTHON="${PYTHON:-python}"
CORE="../src/core/gpsdo.cpp ../src/core/gps_query.cpp ../src/core/nmea.cpp ../src/core/history.cpp ../src/core/reports.cpp"
${CXX:-g++} -std=c++17 -g -Wall -I../src/core -I"$ARDUINOJSON" test_core.cpp $CORE -o "$OUT"
TRANSCRIPT="$OUT.clean.txt"
CORRUPTED="$OUT.bad.txt"
$PYTHON ../../tools/sim_gpsdo.py --dump 300 > "$TRANSCRIPT"
$PYTHON ../../tools/sim_gpsdo.py --dump 300 --bad-rate 0.02 > "$CORRUPTED"
"$OUT" "$TRANSCRIPT" "$CORRUPTED"
rm -f "$TRANSCRIPT" "$CORRUPTED"
