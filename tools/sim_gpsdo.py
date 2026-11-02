#!/usr/bin/env python3
"""GPSDO simulator: develop and test the ESP32 firmware without the hardware (doc/PROTOCOL.md).

Connect a 3.3 V USB-serial adapter to the ESP32 (adapter TX to GPIO20, adapter RX to GPIO21, GND) and run:

    python tools/sim_gpsdo.py --port COM5

It sends NMEA and TLM, answers GET/SET/CMD with the limits of the real firmware, and can misbehave:

    --bad-rate 0.02                 corrupt 2% of the sentences
    --loss-every 120 --loss-for 10  go silent for 10 s every 2 minutes
    --encoder-every 30              change a setting every 30 s, as if from the encoder
    --nmea capture.txt              replay recorded NMEA instead of the generated one

Without a serial port, --dump SECONDS prints what would be sent, for tests.
"""
import argparse
import random
import sys
import time

BAUDRATES = [9600, 19200, 38400, 57600, 115200, 230400, 460800, 921600]
VALUE_MAX = 0xFFFFFFFE
FW_VERSION = "v0.1.16-web1"
OCXO_WARMUP = {0: 70, 1: 100, 2: 70}
ALGO_FACTOR_DEFAULT = {0: 10, 1: 10, 2: 300}
ALGO_FACTOR_LIMITS = {0: (1, 60), 1: (1, 60), 2: (10, 600)}

# In the order of the real CFG report
DEFAULTS = {
    "contrast": 80, "pwm_auto_save": 1, "pps_auto_sync": 1, "ppb_lock_threshold": 50, "ocxo_model": 0,
    "warmup_time": 70, "correction_algo": 2, "correction_factor": 300, "pps_sync_on": 1, "pps_sync_delay": 10,
    "pps_sync_threshold": 30000, "trend_auto_v": 1, "trend_auto_h": 1, "trend_v_scale": 70, "trend_h_scale": 1,
    "gps_time_offset": 0, "date_format": 0, "gps_baudrate": 9600, "boot_menu": 0, "comm_baudrate": 115200,
    "gps_model": 0,
}
RANGES = {
    "contrast": (0, 100), "pwm_auto_save": (0, 1), "pps_auto_sync": (0, 1), "ppb_lock_threshold": (0, 1000),
    "ocxo_model": (0, 2), "warmup_time": (0, 1000), "correction_algo": (0, 2), "pps_sync_on": (0, 1),
    "pps_sync_delay": (0, VALUE_MAX), "pps_sync_threshold": (0, VALUE_MAX), "trend_auto_v": (0, 1),
    "trend_auto_h": (0, 1), "trend_v_scale": (0, VALUE_MAX), "trend_h_scale": (0, VALUE_MAX),
    "gps_time_offset": (-14, 14), "date_format": (0, 4), "boot_menu": (0, 3),
}
MAX_LINE = 160


def checksum(body):
    value = 0
    for ch in body.encode():
        value ^= ch
    return value


def sentence(body):
    return "$%s*%02X\r\n" % (body, checksum(body))


def round_v_scale(scale):
    if scale < 70:
        return 70
    if scale > 2000:
        return round(scale / 1000) * 1000
    if scale > 200:
        return round(scale / 100) * 100
    return round(scale / 10) * 10


def round_h_scale(scale):
    if scale > 64:
        return 64
    if scale < 1:
        return 1
    return 1 << (scale.bit_length() - 1)


class SimGpsdo:
    """Everything except the wire: feed() takes received bytes, both return bytes to send."""

    def __init__(self, seed=1, nmea_lines=None):
        self.random = random.Random(seed)
        self.settings = dict(DEFAULTS)
        self.tlm_enabled = True
        self.uptime = 0
        self.pwm = 38212
        self.saved_pwm = 38000
        self.sync_count = 0
        self.ppb_error = 400.0      # Settles over time
        self.wander = 0.0
        self.samples = 0
        self.rx = bytearray()
        self.forwarded = bytearray()  # What a real GPSDO would pass to the GPS
        self.nmea_lines = nmea_lines
        self.nmea_position = 0
        self.writes = 0

    # -- from the GPSDO
    def boot(self):
        return self.version().encode()

    def version(self):
        return sentence("PGPSDO,VER,1,%s" % FW_VERSION)

    def cfg(self):
        pairs = ["%s=%s" % kv for kv in self.settings.items()] + ["fw_version=%s" % FW_VERSION]
        lines, current = [], "PGPSDO,CFG"
        for pair in pairs:
            if len(current) + 1 + len(pair) + 5 > MAX_LINE:
                lines.append(sentence(current))
                current = "PGPSDO,CFG"
            current += "," + pair
        lines.append(sentence(current))
        return "".join(lines)

    def telemetry(self):
        adjusting = self.uptime >= self.settings["warmup_time"]
        if adjusting:
            self.samples = min(self.samples + 1, 128)
            self.ppb_error *= 0.985
            # An oscillator wanders a little, even when locked
            self.wander = 0.995 * self.wander + self.random.gauss(0, 0.012)
        error = self.ppb_error * 0.07 + self.random.gauss(0, 0.6)     # Hz at 70 MHz
        error_hz = round(error)
        mean = round((self.ppb_error + self.wander) * 100) if self.samples else 65535
        inst = int(error_hz * 1e9 * 100 / 70000000)
        correction = -round(error / 3) if adjusting else 0
        self.pwm = max(0, min(65535, self.pwm + correction))
        locked = self.samples == 128 and abs(mean) <= self.settings["ppb_lock_threshold"]
        shift = round(self.random.gauss(12, 6))
        fields = [
            self.uptime, 1, int(locked), 9, mean, inst, 70000000 + error_hz, error_hz, correction, self.pwm,
            round(self.random.gauss(0, 1)), shift, int(shift / 7), self.sync_count, int(adjusting),
        ]
        return sentence("PGPSDO,TLM," + ",".join(str(f) for f in fields))

    def nmea(self):
        if self.nmea_lines:
            burst = []
            while True:
                line = self.nmea_lines[self.nmea_position]
                self.nmea_position = (self.nmea_position + 1) % len(self.nmea_lines)
                burst.append(line + "\r\n")
                if self.nmea_position == 0 or "GGA" in self.nmea_lines[self.nmea_position][:7]:
                    return "".join(burst)
        t = time.gmtime(1700000000 + self.uptime)
        utc = time.strftime("%H%M%S", t) + ".00"
        date = time.strftime("%d%m%y", t)
        lines = [
            "GNGGA,%s,3351.65000,S,15112.58000,E,1,09,1.1,58.3,M,22.4,M,," % utc,
            "GNRMC,%s,A,3351.65000,S,15112.58000,E,0.01,0.00,%s,,,A" % (utc, date),
            "GPGSA,A,3,04,09,16,27,,,,,,,,,1.9,1.1,1.5",
            "BDGSA,A,3,06,11,19,22,35,,,,,,,,1.9,1.1,1.5",
            "GPGSV,2,1,06,04,62,123,%02d,09,45,233,%02d,16,33,054,%02d,27,71,310,%02d" % self.snr(4),
            "GPGSV,2,2,06,08,12,178,,30,05,288,",
            "BDGSV,2,1,05,06,55,040,%02d,11,40,201,%02d,19,66,290,%02d,22,28,110,%02d" % self.snr(4),
            "BDGSV,2,2,05,35,18,330,%02d" % self.snr(1),
            "GNVTG,0.00,T,,M,0.01,N,0.02,K,A",
            "GNZDA,%s,%s,%s,%s,00,00" % (utc, date[:2], date[2:4], "20" + date[4:]),
        ]
        return "".join(sentence(line) for line in lines)

    def snr(self, count):
        return tuple(self.random.randint(28, 46) for _ in range(count))

    def second(self):
        """One second of output."""
        self.uptime += 1
        out = self.telemetry() if self.tlm_enabled else ""
        return (out + self.nmea()).encode()

    def encoder_change(self):
        """A setting changed on the front panel."""
        self.settings["contrast"] = self.random.randint(40, 100)
        return self.cfg().encode()

    # -- to the GPSDO
    def feed(self, data):
        out = []
        self.rx += data
        while True:
            end = self.rx.find(b"\n")
            if end < 0:
                break
            line = bytes(self.rx[:end + 1])
            del self.rx[:end + 1]
            if line.startswith(b"$PGPSDO,"):
                out.append(self.handle(line.decode("ascii", "replace").rstrip("\r\n")))
            else:
                self.forwarded += line
        return "".join(out).encode()

    @staticmethod
    def ack(echo, name, error):
        parts = ["PGPSDO,ACK"]
        if echo:
            parts.append(echo)
        if name:
            parts.append(name[:32])
        if error:
            parts += ["ERR", error]
        elif echo:
            parts.append("OK")
        return sentence(",".join(parts))

    def handle(self, line):
        star = line.rfind("*")
        if star < 0 or len(line) != star + 3:
            return self.ack(None, None, "CHECKSUM")
        body = line[1:star]
        try:
            if int(line[star + 1:], 16) != checksum(body):
                return self.ack(None, None, "CHECKSUM")
        except ValueError:
            return self.ack(None, None, "CHECKSUM")
        fields = body.split(",")[1:]
        if len(fields) > 3:
            return self.ack(None, None, "FORMAT")
        kind = fields[0] if fields else ""
        name = fields[1] if len(fields) > 1 else None
        value = fields[2] if len(fields) > 2 else None
        if kind == "SET":
            return self.handle_set(name, value)
        if kind == "CMD":
            return self.handle_cmd(name, value)
        if kind == "GET":
            return self.handle_get(name, value)
        return self.ack(None, None, "FORMAT")

    def handle_get(self, name, extra):
        if name is None or extra is not None:
            return self.ack("GET", name, "FORMAT")
        if name == "VER":
            return self.ack("GET", name, None) + self.version()
        if name == "CFG":
            return self.ack("GET", name, None) + self.cfg()
        return self.ack("GET", name, "KEY")

    def handle_cmd(self, name, value):
        if name is None:
            return self.ack("CMD", name, "FORMAT")
        if name == "TLM":
            if value is None:
                return self.ack("CMD", name, "FORMAT")
            if value not in ("0", "1"):
                return self.ack("CMD", name, "RANGE")
            self.tlm_enabled = value == "1"
            return self.ack("CMD", name, None)
        if value is not None:
            return self.ack("CMD", name, "FORMAT" if name in ("SAVEPWM", "FORCESYNC") else "KEY")
        if name == "SAVEPWM":
            if self.saved_pwm != self.pwm:
                self.saved_pwm = self.pwm
                self.writes += 1
            return self.ack("CMD", name, None)
        if name == "FORCESYNC":
            if self.settings["pps_sync_on"]:
                self.sync_count += 1
            return self.ack("CMD", name, None)
        return self.ack("CMD", name, "KEY")

    def handle_set(self, key, text):
        if key is None:
            return self.ack("SET", key, "FORMAT")
        if key not in DEFAULTS or key == "gps_model":
            return self.ack("SET", key, "KEY")
        try:
            if text is None or len(text.lstrip("-")) > 10 or not text.lstrip("-").isdigit():
                raise ValueError
            value = int(text)
        except ValueError:
            return self.ack("SET", key, "FORMAT")
        error = self.validate(key, value)
        if error:
            return self.ack("SET", key, error)
        before = dict(self.settings)
        if key == "trend_v_scale":
            value = round_v_scale(value)
        elif key == "trend_h_scale":
            value = round_h_scale(value)
        self.settings[key] = value
        if key == "ocxo_model" and before[key] != value:
            self.settings["warmup_time"] = OCXO_WARMUP[value]
        if key == "correction_algo" and before[key] != value:
            self.settings["correction_factor"] = ALGO_FACTOR_DEFAULT[value]
        reply = self.ack("SET", key, None)
        if self.settings != before:
            self.writes += 1
            reply += self.cfg()
        return reply

    def validate(self, key, value):
        if key in RANGES:
            low, high = RANGES[key]
            if not low <= value <= high:
                return "RANGE"
        if key == "correction_factor":
            low, high = ALGO_FACTOR_LIMITS[self.settings["correction_algo"]]
            if not low <= value <= high:
                return "RANGE"
        if key in ("gps_baudrate", "comm_baudrate") and value not in BAUDRATES:
            return "RANGE"
        if key == "gps_baudrate" and self.settings["gps_model"] == 0 and value > 115200:
            return "RANGE"
        if key == "trend_v_scale" and self.settings["trend_auto_v"]:
            return "BUSY"
        if key == "trend_h_scale" and self.settings["trend_auto_h"]:
            return "BUSY"
        return None


def corrupt(data, rng, rate):
    """Flips a byte in a share of the lines."""
    if rate <= 0:
        return data
    lines = data.split(b"\n")
    for i, line in enumerate(lines):
        if len(line) > 8 and rng.random() < rate:
            position = rng.randrange(1, len(line) - 4)
            damaged = bytearray(line)
            damaged[position] ^= 0x01
            lines[i] = bytes(damaged)
    return b"\n".join(lines)


def read_nmea_file(path):
    with open(path, errors="replace") as source:
        return [line.strip() for line in source if line.startswith("$") and not line.startswith("$PGPSDO")]


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--port", help="serial port connected to the ESP32")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--nmea", help="file of recorded NMEA to replay")
    parser.add_argument("--bad-rate", type=float, default=0.0, help="share of sentences to corrupt")
    parser.add_argument("--loss-every", type=float, default=0, help="seconds between link losses")
    parser.add_argument("--loss-for", type=float, default=10, help="duration of a link loss, in seconds")
    parser.add_argument("--encoder-every", type=float, default=0, help="seconds between encoder changes")
    parser.add_argument("--warmed-up", action="store_true", help="start with warm-up over and frequency settled")
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--dump", type=int, help="print this many seconds of output, without a serial port")
    parser.add_argument("--verbose", action="store_true", help="print commands and replies")
    args = parser.parse_args()

    sim = SimGpsdo(args.seed, read_nmea_file(args.nmea) if args.nmea else None)
    if args.warmed_up:
        sim.uptime = 3600
        sim.samples = 128
        sim.ppb_error = 2.0
    rng = random.Random(args.seed + 1)

    if args.dump is not None:
        sys.stdout.buffer.write(sim.boot())
        for _ in range(args.dump):
            sys.stdout.buffer.write(corrupt(sim.second(), rng, args.bad_rate))
        return

    if not args.port:
        parser.error("--port or --dump is required")
    import serial  # Only needed with real hardware

    link = serial.Serial(args.port, args.baud, timeout=0.02)
    print("Simulating a GPSDO on %s at %d baud, Ctrl-C to stop" % (args.port, args.baud))
    link.write(sim.boot())
    start = time.monotonic()
    next_second = start + 1
    next_encoder = start + args.encoder_every if args.encoder_every else None
    next_loss = start + args.loss_every if args.loss_every else None
    loss_until = 0
    try:
        while True:
            now = time.monotonic()
            silent = now < loss_until
            if next_loss and now >= next_loss:
                loss_until = now + args.loss_for
                next_loss = now + args.loss_every
                print("link loss for %g s" % args.loss_for)
            received = link.read(256)
            if received and not silent:
                reply = sim.feed(received)
                if args.verbose:
                    print("<", received.decode("ascii", "replace").strip())
                    print(">", reply.decode("ascii", "replace").strip())
                link.write(corrupt(reply, rng, args.bad_rate))
            if now >= next_second:
                next_second += 1
                output = sim.second()
                if not silent:
                    link.write(corrupt(output, rng, args.bad_rate))
            if next_encoder and now >= next_encoder:
                next_encoder = now + args.encoder_every
                output = sim.encoder_change()
                print("encoder: contrast = %d" % sim.settings["contrast"])
                if not silent:
                    link.write(output)
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
