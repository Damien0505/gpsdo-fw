# GPSDO serial protocol (version 1)

Monitoring and control of the GPSDO on the external link (USART2: PA2 TX, PA3 RX), mixed with the GPS
passthrough. The protocol code is in `src/comm.c`.

The link defaults to 115200 8N1, and is independent from the GPS module baudrate. It can be changed with
the `GPS > Comm` menu or the `comm_baudrate` setting, and takes effect at next boot.

Sentences are proprietary NMEA-style ones prefixed `$PGPSDO,`. Ordinary NMEA consumers ignore `$P...`
sentences, so a plain serial-to-USB adapter on the link still works as a GPS feed.

## Framing

- Format: `$PGPSDO,<TYPE>,<fields...>*<CS>\r\n`, where `CS` is the standard NMEA checksum: two hex digits, the
  XOR of every byte between `$` and `*`.
- Integers only, no floats. Fractional values are scaled, e.g. ppb x 100.
- Commands with a missing or wrong checksum are rejected with `ERR,CHECKSUM`.
- Maximum line length is 160 characters, including CRLF, which can exceed the 82 of the NMEA convention.
- Setting keys are lowercase snake_case and stable across versions. New `TLM` fields may only be appended.
- Sentences are never spliced into a GPS sentence, they go out between lines.
- If the GPS stops mid-line for 250 ms, the line is terminated with CRLF so that the GPSDO can still talk.

## From the GPSDO

| Type | When | Fields |
| --- | --- | --- |
| `VER` | At boot, and on `GET,VER` | protocol version (`1`), firmware version string |
| `TLM` | Once per second while enabled (default on) | Positional, see below. Dropped if it can't go out within 500 ms because the GPS stays mid-line. |
| `CFG` | On `GET,CFG`, and when a saved setting changes from any source | `key=value` pairs, split across several `CFG` lines (3 today). A report is complete when `fw_version` is received. |
| `ACK` | Reply to every `SET` / `CMD` / `GET` | See below |

### `ACK`

```
$PGPSDO,ACK,<SET|CMD|GET>,<key or command>,OK
$PGPSDO,ACK,<SET|CMD|GET>,<key or command>,ERR,<reason>
$PGPSDO,ACK,ERR,<reason>     (sentence too damaged to tell what it was)
```

| Reason | Meaning |
| --- | --- |
| `CHECKSUM` | Missing or wrong checksum |
| `KEY` | Unknown key or command. Read-only keys also give this on `SET`. |
| `RANGE` | Value not allowed |
| `FORMAT` | Malformed sentence: not an integer, missing or extra field, line too long, or line left unfinished for 1 s |
| `BUSY` | `trend_v_scale` / `trend_h_scale` while the matching auto-scale is on, as the encoder refuses too |

`ACK,...,OK` on a `SET` means applied to the running GPSDO, not yet saved: see [Saving](#saving).

### `TLM` fields

| # | Field | Source | Unit |
| --- | --- | --- | --- |
| 1 | uptime | `device_uptime` | s |
| 2 | gps_lock | `gps_lock_status` | 0/1 |
| 3 | ppb_lock | `ppb_lock_status` | 0/1 |
| 4 | sats | `num_sats` | count |
| 5 | ppb_mean | `frequency_get_ppb()` | ppb x 100. 65535 means no sample yet (`?` on the LCD). |
| 6 | ppb_inst | as PPB > Inst screen | ppb x 100 |
| 7 | freq | `ppb_frequency` | Hz (~70,000,000) |
| 8 | error | `ppb_error` | Hz |
| 9 | correction | `ppb_correction` | PWM steps |
| 10 | pwm | `TIM1->CCR2` | raw |
| 11 | millis | `ppb_millis` | ms |
| 12 | pps_shift | `pps_error` | clock cycles |
| 13 | pps_shift_ms | `pps_millis` | 0.1 us: divide by 10000 for the ms shown on the LCD |
| 14 | pps_sync_count | `pps_sync_count` | count |
| 15 | adjusting | `frequency_adjustment_allowed()` | 0/1 (0 during warm-up) |

Differences with what the LCD displays: `correction` is a number during warm-up where the LCD shows `Warm-up`
(see `adjusting`), and `pps_shift` keeps its sign below -9999999, where the LCD drops it for lack of room.

The sample after a save to flash has wrong `freq` and `pps_shift` values, see [Saving](#saving).

GPS time, position, altitude, HDOP and satellites come from the normal NMEA sentences.

## To the GPSDO

| Sentence | Effect |
| --- | --- |
| `$PGPSDO,GET,VER` | Reply with `VER` |
| `$PGPSDO,GET,CFG` | Reply with `CFG` lines |
| `$PGPSDO,SET,<key>,<value>` | Validate, apply, save if changed, reply `ACK`. `CFG` follows if a saved value changed. |
| `$PGPSDO,CMD,SAVEPWM` | Same as the PWM screen press: save the current PWM. Nothing is written if it is unchanged. |
| `$PGPSDO,CMD,FORCESYNC` | Same as PPS > Force Sync. Only acts when `pps_sync_on` is 1. |
| `$PGPSDO,CMD,TLM,<0/1>` | Turn telemetry off or on (not saved) |

Send one command at a time and wait for its `ACK`. Commands are not read while a reply is waiting.

## Settings

| Key | Menu | Values | Notes |
| --- | --- | --- | --- |
| `contrast` | Contrast | 0-100 | |
| `pwm_auto_save` | PPB | 0/1 | |
| `pps_auto_sync` | PPB | 0/1 | |
| `ppb_lock_threshold` | PPB | 0-1000 | ppb x 100 |
| `ocxo_model` | PPB | 0 Isotemp, 1 OX256B, 2 Unknown | Also resets `warmup_time` to the model's default, when it changes |
| `warmup_time` | PPB | 0-1000 | s |
| `correction_algo` | PPB | 0 Dankar, 1 Fredzo, 2 Eric H | Also resets `correction_factor` to the algorithm's default, when it changes |
| `correction_factor` | PPB | Dankar, Fredzo: 1-60. Eric H: 10-600 | Limits of the current algorithm |
| `pps_sync_on` | PPS | 0/1 | |
| `pps_sync_delay` | PPS | 0-4294967294 | s |
| `pps_sync_threshold` | PPS | 0-4294967294 | clock cycles |
| `trend_auto_v` / `trend_auto_h` | Trend | 0/1 | |
| `trend_v_scale` | Trend | 0-4294967294 | ppb x 100. Rounded as by the encoder. `CFG` gives the result. `BUSY` if `trend_auto_v` is on. |
| `trend_h_scale` | Trend | 0-4294967294 | Rounded down to a power of 2, up to 64. `BUSY` if `trend_auto_h` is on. |
| `gps_time_offset` | GPS | -14 to 14 | h |
| `date_format` | GPS | 0-4 | 0 dd/mm/yy, 1 mm/dd/yy, 2 yy/mm/dd, 3 dd.mm.yy, 4 yy-mm-dd |
| `gps_baudrate` | GPS | 9600, 19200, 38400, 57600, 115200, 230400, 460800, 921600 | Up to 115200 with an ATGM336H. Does not affect the link. |
| `boot_menu` | (last screen) | 0 Main, 1 Date, 2 Date/time, 3 Trend | |
| `comm_baudrate` | GPS > Comm | Same list as `gps_baudrate` | Takes effect at next boot. **The other end must follow:** the ESP32 firmware expects 115200. Recover with the `GPS > Comm` menu. |

Read-only keys, reported in `CFG` only: `gps_model` (0 ATGM336H, 1 NEO-6M, 2 NEO-M9N, 3 unknown), `fw_version`.

A setting that is already at the requested value replies `OK`, and neither writes to flash nor sends `CFG`.

## Saving

Saving erases a flash page: the CPU stalls for 20-40 ms, and the flash is rated for about 10,000 erases.

So remote changes apply at once but are saved separately: at most once every 10 s (a burst of changes gives one
erase), and if possible when the GPS is quiet and the next PPS output pulse is more than 100 ms away. Waiting for
such a moment takes 3 s at most. **Allow 13 s after the last change before removing power.** Encoder changes are
still saved immediately.

Each save, wherever it comes from, has effects on the frequency measurement because TIM1 overflow interrupts are
lost during the stall:

- One bad sample: `frequency_get_error()` filters it out and counts it as 0 error. No correction is applied.
- `pps_shift` is wrong for that second. Resync waits for `pps_sync_delay` consecutive bad seconds, so this
  causes no resync unless `pps_sync_delay` is 0.
- The PPS output pulse is late if its interrupt falls in the stall, which the timing above is there to avoid.

This is from reading the code. It should be confirmed on the bench by watching `TLM` around a `SET`.

## Passthrough

Anything else received on the link goes to the GPS, including binary UBX traffic. A sentence starting with
`$PGPSDO,` at the start of a line is never forwarded.

The link and the GPS have independent baudrates, which has a few consequences:

- **Link faster than the GPS** (the default: 115200 / 9600): about 300 bytes can be accepted in one burst. More
  is lost when there is no pause for the GPS to catch up. Normal configuration commands are far below that.
- **Link slower than the GPS**: the GPS keeps working normally, but excess bytes are lost on the link if NMEA
  output exceeds what it can carry.
- **Heavy traffic such as u-center or firmware upload**: set `GPS > Comm` to the same value as the GPS baudrate
  and reboot, so that bytes are not faster on one side.
- **u-center baudrate changes**: changing the module baudrate from u-center no longer changes the link. u-center
  will lose the module, as the GPSDO keeps using its configured GPS baudrate. Change it from the GPS menu.
- A `$PGPSDO` command is only seen at the start of a line. After binary traffic it may be missed until the next
  newline, or 1 s without traffic. Don't send commands during a UBX session.

## Examples

```
$PGPSDO,VER,1,v0.1.16-web1*5F
$PGPSDO,TLM,1234,1,1,9,3,-12,70000000,0,-1,38212,0,12,0,4,1*7D
$PGPSDO,GET,CFG*0B
$PGPSDO,SET,pps_sync_delay,30*73
$PGPSDO,ACK,SET,pps_sync_delay,OK*11
$PGPSDO,ACK,SET,contrast,ERR,RANGE*20
$PGPSDO,CMD,SAVEPWM*1E
$PGPSDO,ACK,CMD,SAVEPWM,OK*53
$PGPSDO,CMD,FORCESYNC*0F
```

## Testing

`tools/host_test/run.sh` runs the real firmware sources against a fake HAL on a PC, with any C compiler. This
checks logic, not real UART, DMA or flash timing.
