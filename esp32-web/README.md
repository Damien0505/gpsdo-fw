# GPSDO web interface

Firmware for an ESP32-C3 Super Mini that puts a GPSDO running this repository's firmware on the LAN: live readings,
PPB trend, GPS details, and every setting of the front panel menus. The LCD and encoder keep working as before.

**Use it on a trusted network only.** The admin password guards against accidental changes. It travels unencrypted,
as everything does over plain HTTP.

## Wiring

| GPSDO (Bluepill) | | ESP32-C3 | Notes |
| --- | --- | --- | --- |
| PA2 (USART2 TX) | to | GPIO20 | 1 kΩ series resistor recommended |
| PA3 (USART2 RX) | from | GPIO21 | 1 kΩ series resistor recommended |
| GND | | GND | The only shared conductor |

Power the ESP32 from its own USB-C port, not from the GPSDO. Keep it and its antenna outside the metal case and away
from the GPS antenna lead. The link runs at 115200 baud: the GPSDO's `GPS > Comm` setting must stay at that default.

## Build and flash

With [PlatformIO](https://platformio.org/), from this directory:

```
pio run -t upload       # The firmware
pio run -t uploadfs     # The web interface
```

Keep the path of the project and of PlatformIO's files short on Windows: the compiler fails with
`CreateProcess: No such file or directory` on very long paths.

Later updates go over the network, at `/update`: `firmware.bin` for the firmware, `littlefs.bin` for the web interface.

## First start

1. Join the `GPSDO-Setup` WiFi network. A setup page should open by itself, otherwise go to `http://192.168.4.1/`.
2. Enter your WiFi details and choose an admin password.
3. After the restart, open `http://gpsdo.local/`. The user name for the password is `admin`.

If WiFi is missing for 60 s, `GPSDO-Setup` comes back while the ESP32 keeps trying. On this network, a device that
is already set up asks for the admin password before changing anything.

Holding the BOOT button for 5 s erases the settings, in case the password is lost.

The onboard LED is solid when everything works, blinks slowly without a link to the GPSDO, and quickly without WiFi.

Transmit power is limited to 8.5 dBm, for both the setup network and your WiFi, because some C3 Super Mini boards
are reported to be unreliable at full power. To change it, edit `WIFI_TX_POWER` under `build_flags`.

## HTTP API

| Method | Path | Password | Purpose |
| --- | --- | --- | --- |
| GET | `/api/status` | | Telemetry, GPS, link health, as JSON |
| GET | `/api/config` | | Current settings and firmware version |
| POST | `/api/config` | admin | `{"key": value, ...}`, with integer values as in [the protocol](../doc/PROTOCOL.md). Sent in order. |
| POST | `/api/action` | admin | `{"action": "savepwm"}` or `{"action": "forcesync"}` |
| GET | `/api/history?range=1h` or `24h` | | Trend, oldest first, in ppb x 100. `null` is a gap. In `24h`, rows are `[min, mean, max]` per minute. |
| GET | `/api/console` | | The last 200 lines of the serial link. `> ` marks what was sent to the GPSDO. |
| WS | `/ws` | | The status every second, and the settings when they change. Messages have a `type`. |
| GET | `/metrics` | | Prometheus |
| GET | `/gpsquery` | admin | Opened in a browser: reads the GPS module's settings, which takes about 15 s. Changes nothing. |
| GET | `/api/gpsquery` | | The results: `$GPTXT` answers, and raw payloads of its `CFG` messages |

POST requests need `Content-Type: application/json`, and answer once the GPSDO has replied, 4 s at most:

```
{"ok":false,"results":{"contrast":{"ok":true},"warmup_time":{"ok":false,"error":"RANGE"}}}
```

`error` is a reason from the protocol, or `TIMEOUT`, `LINK_DOWN` or `BUSY` (too many commands waiting).
A setting applied is saved by the GPSDO within 13 s, see [Saving](../doc/PROTOCOL.md#saving).

History is kept in RAM, and restarts when the ESP32 restarts.

A flash write on the GPSDO stops its CPU for about 20 ms, which puts one frequency reading out by multiples of 65536 Hz.
The GPSDO's control loop ignores those readings. Here, the last good frequency and PPS shift stay in place instead, up
to three readings in a row, and `glitches` in the status counts them.

## Working without the hardware

`src/core` holds the protocol logic, with no Arduino dependency, so that it runs on a PC.

```
# Tests. ARDUINOJSON: ArduinoJson's src directory, in .pio/libdeps after a "pio run"
test_host/run.sh

# The web interface in a browser: real core logic, simulated GPSDO. Log in with admin / admin
c++ -std=c++17 -Isrc/core -I$ARDUINOJSON test_host/core_cli.cpp src/core/*.cpp -o core_cli
python test_host/preview_server.py --cli ./core_cli

# A simulated GPSDO for a real ESP32, through a 3.3 V USB-serial adapter on GPIO20 and GPIO21
python ../tools/sim_gpsdo.py --port COM5 --bad-rate 0.01 --loss-every 120 --encoder-every 30
```

The preview replaces the Arduino web layer (`web.cpp`, `network.cpp`). What only exists on a real board, such as WiFi,
the captive portal, OTA and long runs, has to be tested there.

uPlot is included in `web/vendor`, under its MIT licence, so that nothing is loaded from the internet.
