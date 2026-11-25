#!/usr/bin/env python3
"""
ALISHA HUD bridge  —  Raspberry Pi side of the ESP32 HUD link.

  ESP32 mic --USB serial--> this bridge --> PipeWire virtual mic "ALISHA-ESP32-Mic"
  Pi stats / journal events / Alisha state --> this bridge --USB serial--> ESP32 screen
  ESP32 touch actions --> allowlisted commands --> Alisha (unix socket)

Security notes
  * Runs as your normal user, no root. Sockets live in $XDG_RUNTIME_DIR (mode 0700).
  * Every frame from the device is CRC + length checked; unknown frames are dropped.
  * Device "actions" are small integers mapped to a fixed allowlist below. The
    device can never send free-form commands.
  * Text from Alisha is sanitised to printable ASCII and length-capped before sending.

Usage
  alisha_hud_bridge.py                 run the bridge
  alisha_hud_bridge.py --levels        print per-channel mic levels (find the real mics)
  alisha_hud_bridge.py --mic 1 --gain 30
"""
import argparse
import array
import collections
import glob
import json
import os
import re
import select
import shutil
import signal
import socket
import struct
import subprocess
import sys
import threading
import time

import serial  # pyserial

DEFAULT_PORT = "/dev/serial/by-id/usb-Silicon_Labs_CP2102_USB_to_UART_Bridge_Controller_0001-if00-port0"
BAUD = 921600
RUNTIME = os.environ.get("XDG_RUNTIME_DIR") or f"/run/user/{os.getuid()}"
FIFO = os.path.join(RUNTIME, "alisha-esp32-mic.fifo")
HUD_SOCK = os.path.join(RUNTIME, "alisha-hud.sock")   # Alisha -> bridge (state/heard/reply)
CTL_SOCK = os.path.join(RUNTIME, "alisha-ctl.sock")   # bridge -> Alisha (allowlisted commands)
SOURCE_NAME = "alisha_esp32_mic"
SINK_NAME = "alisha_esp32_speaker"
SPK_FIFO = os.path.join(RUNTIME, "alisha-esp32-spk.fifo")
SPK_CHUNK = 640                   # 20 ms of 16 kHz mono s16le

SOF = b"\xA5\x5A"
MSG_HELLO, MSG_AUDIO, MSG_LEVELS, MSG_EVENT = 0x01, 0x02, 0x03, 0x04
MSG_STATS, MSG_STATE, MSG_LOG, MSG_HEARD, MSG_REPLY, MSG_CONFIG, MSG_TIME, MSG_PING, MSG_SAVER, MSG_HOSTS, MSG_SPEAKER = range(0x10, 0x1B)
EVT_MIC_MUTED, EVT_ACTION, EVT_BOOT = 1, 2, 3
LOG_INFO, LOG_OK, LOG_WARN, LOG_ALERT = 0, 1, 2, 3
STATES = {"idle": 0, "listening": 1, "thinking": 2, "speaking": 3, "asleep": 4}
MAX_IN = 1024
TEXT_MAX, LOG_MAX = 120, 60

# Device action id -> exact phrase handed to Alisha. Nothing else is ever sent.
ACTIONS = {1: "system status", 2: "lock the screen", 3: "run matrix", 4: "what time is it"}

STATS_FMT = "<BBBhHIIIHHB16s16s"
assert struct.calcsize(STATS_FMT) == 56


def crc16(data: bytes, crc: int = 0xFFFF) -> int:
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def ascii_clean(s: str, n: int) -> bytes:
    return "".join(c if 0x20 <= ord(c) <= 0x7E else "?" for c in str(s))[:n].encode("ascii")


class Link:
    def __init__(self, port: str):
        self.ser = serial.Serial()
        self.ser.port = port
        self.ser.baudrate = BAUD
        self.ser.timeout = 0.1
        # Keep DTR/RTS released so opening the port does not reset the board.
        self.ser.dtr = False
        self.ser.rts = False
        self.ser.open()
        self.lock = threading.Lock()
        self.seq = 0
        self.buf = bytearray()

    def send(self, typ: int, payload: bytes = b"") -> None:
        if len(payload) > 1100:
            raise ValueError("payload too large")
        with self.lock:
            hdr = bytes([typ, self.seq, len(payload) & 0xFF, len(payload) >> 8])
            self.seq = (self.seq + 1) & 0xFF
            crc = crc16(hdr + payload)
            self.ser.write(SOF + hdr + payload + struct.pack("<H", crc))

    def frames(self):
        """Yield (type, payload) for every valid frame. Resyncs on garbage."""
        while True:
            chunk = self.ser.read(4096)
            if chunk:
                self.buf += chunk
            while True:
                i = self.buf.find(SOF)
                if i < 0:
                    del self.buf[:-1]
                    break
                if i:
                    del self.buf[:i]
                if len(self.buf) < 6:
                    break
                typ, _seq, ln = self.buf[2], self.buf[3], self.buf[4] | (self.buf[5] << 8)
                if ln > MAX_IN:
                    del self.buf[:2]
                    continue
                if len(self.buf) < 8 + ln:
                    break
                frame = bytes(self.buf[2:6 + ln])
                (want,) = struct.unpack_from("<H", self.buf, 6 + ln)
                if crc16(frame) == want:
                    del self.buf[:8 + ln]
                    yield typ, frame[4:]
                else:
                    del self.buf[:2]
            if not chunk:
                yield None, b""   # idle tick


# --------------------------------------------------------------- virtual mic

class VirtualSpeaker:
    """PipeWire pipe-sink "ALISHA-ESP32-Speaker": whatever plays to it is streamed to the board's
    ES8311 speaker at real-time pace. Pure silence is not sent, so the amplifier can switch off."""

    def __init__(self, link):
        self.link = link
        self.module_id = None
        self.q = collections.deque()
        self.lock = threading.Lock()
        self.running = False

    def start(self) -> None:
        mods = subprocess.run(["pactl", "list", "short", "modules"], capture_output=True, text=True).stdout
        for line in mods.splitlines():
            if "module-pipe-sink" in line and SINK_NAME in line:
                subprocess.run(["pactl", "unload-module", line.split()[0]], check=False)
        out = subprocess.run(
            ["pactl", "load-module", "module-pipe-sink", f"sink_name={SINK_NAME}", f"file={SPK_FIFO}",
             "format=s16le", "rate=16000", "channels=1",
             "sink_properties=device.description=ALISHA-ESP32-Speaker"],
            capture_output=True, text=True, check=True)
        self.module_id = out.stdout.strip()
        self.running = True
        threading.Thread(target=self._reader, daemon=True).start()
        threading.Thread(target=self._sender, daemon=True).start()

    def _reader(self):
        fd = os.open(SPK_FIFO, os.O_RDONLY)
        buf, quiet = b"", 1.0
        while self.running:
            data = os.read(fd, 4096)
            if not data:
                time.sleep(0.01)
                continue
            buf += data
            while len(buf) >= SPK_CHUNK:
                chunk, buf = buf[:SPK_CHUNK], buf[SPK_CHUNK:]
                a = array.array("h", chunk)
                silent = max(abs(min(a)), abs(max(a))) < 24
                quiet = quiet + 0.02 if silent else 0.0
                if quiet < 0.25:                         # keep a short tail, drop long silence
                    with self.lock:
                        if len(self.q) < 400:            # cap ~8 s of backlog
                            self.q.append(chunk)

    def _sender(self):
        """Send each 20 ms chunk at its real-time slot, 200 ms early, so the board's 1 s
        buffer stays a little ahead without overflowing. Schedule resets after 300 ms idle."""
        AHEAD, idle_since, t0, sent = 0.2, None, None, 0
        while self.running:
            with self.lock:
                chunk = self.q.popleft() if self.q else None
            now = time.monotonic()
            if chunk is None:
                idle_since = idle_since or now
                if t0 is not None and now - idle_since > 0.3:
                    t0 = None
                time.sleep(0.005)
                continue
            idle_since = None
            if t0 is None:
                t0, sent = now, 0
            delay = (t0 + sent * 0.02 - AHEAD) - now
            if delay > 0:
                time.sleep(delay)
            try:
                self.link.send(MSG_SPEAKER, chunk)
            except (serial.SerialException, ValueError):
                return
            sent += 1

    def stop(self) -> None:
        self.running = False
        if self.module_id:
            subprocess.run(["pactl", "unload-module", self.module_id], check=False)
            self.module_id = None


class VirtualMic:
    """PipeWire/PulseAudio pipe-source: whatever we write to the FIFO is a microphone."""

    def __init__(self):
        self.module_id = None
        self.fd = None
        self.prev_default = None

    def start(self) -> None:
        if not shutil.which("pactl"):
            raise RuntimeError("pactl not found (install pulseaudio-utils)")
        self._unload_stale()
        self.prev_default = subprocess.run(["pactl", "get-default-source"], capture_output=True, text=True).stdout.strip()
        out = subprocess.run(
            ["pactl", "load-module", "module-pipe-source", f"source_name={SOURCE_NAME}", f"file={FIFO}",
             "format=s16le", "rate=16000", "channels=1",
             "source_properties=device.description=ALISHA-ESP32-Mic"],
            capture_output=True, text=True, check=True)
        self.module_id = out.stdout.strip()
        subprocess.run(["pactl", "set-default-source", SOURCE_NAME], check=False)
        self.fd = os.open(FIFO, os.O_WRONLY | os.O_NONBLOCK)

    def _unload_stale(self) -> None:
        mods = subprocess.run(["pactl", "list", "short", "modules"], capture_output=True, text=True).stdout
        for line in mods.splitlines():
            if "module-pipe-source" in line and SOURCE_NAME in line:
                subprocess.run(["pactl", "unload-module", line.split()[0]], check=False)

    def write(self, pcm: bytes) -> None:
        if self.fd is None:
            return
        try:
            os.write(self.fd, pcm)
        except BlockingIOError:
            pass  # reader is behind; drop rather than block the link

    def stop(self) -> None:
        if self.fd is not None:
            os.close(self.fd)
            self.fd = None
        if self.module_id:
            subprocess.run(["pactl", "unload-module", self.module_id], check=False)
            self.module_id = None
        if self.prev_default and self.prev_default != SOURCE_NAME:
            subprocess.run(["pactl", "set-default-source", self.prev_default], check=False)


# ------------------------------------------------------------------- stats

class Stats:
    def __init__(self):
        self.prev_cpu = None
        self.prev_net = None
        self.prev_t = None

    @staticmethod
    def _read(path: str, default: str = "") -> str:
        try:
            with open(path) as f:
                return f.read()
        except OSError:
            return default

    def cpu(self) -> int:
        f = [int(x) for x in self._read("/proc/stat").split("\n", 1)[0].split()[1:]]
        idle, total = f[3] + f[4], sum(f)
        pct = 0
        if self.prev_cpu:
            di, dt = idle - self.prev_cpu[0], total - self.prev_cpu[1]
            pct = int(100 * (1 - di / dt)) if dt else 0
        self.prev_cpu = (idle, total)
        return max(0, min(100, pct))

    def mem(self) -> int:
        m = {}
        for line in self._read("/proc/meminfo").splitlines():
            k, v = line.split(":", 1)
            m[k] = int(v.split()[0])
        return int(100 * (1 - m["MemAvailable"] / m["MemTotal"]))

    def temp_x10(self) -> int:
        t = self._read("/sys/class/thermal/thermal_zone0/temp", "0").strip() or "0"
        return int(int(t) / 100)

    def fan(self) -> int:
        for p in glob.glob("/sys/devices/platform/cooling_fan/hwmon/*/fan1_input") + \
                 glob.glob("/sys/class/hwmon/hwmon*/fan1_input"):
            v = self._read(p).strip()
            if v.isdigit():
                return min(int(v), 65535)
        return 0

    def net(self):
        rx = tx = 0
        for line in self._read("/proc/net/dev").splitlines()[2:]:
            name, data = line.split(":", 1)
            if name.strip() == "lo":
                continue
            f = data.split()
            rx += int(f[0])
            tx += int(f[8])
        now = time.monotonic()
        rbps = tbps = 0
        if self.prev_net:
            dt = max(now - self.prev_t, 1e-3)
            rbps = int((rx - self.prev_net[0]) / dt)
            tbps = int((tx - self.prev_net[1]) / dt)
        self.prev_net, self.prev_t = (rx, tx), now
        return max(rbps, 0), max(tbps, 0)

    @staticmethod
    def ip() -> str:
        try:
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
                s.connect(("192.0.2.1", 9))  # TEST-NET; UDP connect sends nothing
                return s.getsockname()[0]
        except OSError:
            return "offline"

    def lan_hosts(self):
        hosts = set()
        for line in self._read("/proc/net/arp").splitlines()[1:]:
            f = line.split()
            if len(f) >= 4 and f[2] == "0x2":
                hosts.add((f[0], f[3]))
        return hosts

    def tcp(self):
        listen, ssh = 0, 0
        for path in ("/proc/net/tcp", "/proc/net/tcp6"):
            for line in self._read(path).splitlines()[1:]:
                f = line.split()
                if len(f) < 4:
                    continue
                port = int(f[1].rsplit(":", 1)[1], 16)
                if f[3] == "0A":
                    listen += 1
                elif f[3] == "01" and port == 22:
                    ssh += 1
        return listen, ssh

    def pack(self) -> bytes:
        rx, tx = self.net()
        listen, ssh = self.tcp()
        up = int(float(self._read("/proc/uptime", "0 0").split()[0]))
        disk = shutil.disk_usage("/")
        return struct.pack(
            STATS_FMT, self.cpu(), self.mem(), int(100 * disk.used / disk.total), self.temp_x10(),
            self.fan(), min(rx, 2**32 - 1), min(tx, 2**32 - 1), up, min(len(self.lan_hosts()), 65535),
            min(listen, 65535), min(ssh, 255), ascii_clean(self.ip(), 16), ascii_clean(socket.gethostname(), 16))


# ---------------------------------------------------------------- ops feed

JOURNAL_RULES = [
    (re.compile(r"sshd.*Failed password for (?:invalid user )?(\S+) from (\S+)"), LOG_ALERT, "SSH FAIL {0}@{1}"),
    (re.compile(r"sshd.*Accepted \S+ for (\S+) from (\S+)"), LOG_WARN, "SSH LOGIN {0}@{1}"),
    (re.compile(r"sudo:\s+(\S+) : .*COMMAND=(.*)"), LOG_WARN, "SUDO {0}: {1}"),
    (re.compile(r"usb \S+: New USB device found, idVendor=(\w+), idProduct=(\w+)"), LOG_WARN, "USB NEW {0}:{1}"),
    (re.compile(r"usb \S+: Product: (.*)"), LOG_INFO, "USB  {0}"),
    (re.compile(r"usb \S+: USB disconnect"), LOG_INFO, "USB disconnected"),
    (re.compile(r"(\w+): Link is (Up|Down)"), LOG_INFO, "LINK {0} {1}"),
    (re.compile(r"NetworkManager.*device \((\w+)\): state change: \w+ -> (activated|disconnected)"), LOG_INFO, "NET {0} {1}"),
    (re.compile(r"Under-voltage detected"), LOG_ALERT, "UNDER-VOLTAGE"),
]


def journal_watcher(emit):
    if not shutil.which("journalctl"):
        return
    p = subprocess.Popen(["journalctl", "-f", "-n", "0", "-o", "cat", "--no-pager"],
                         stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True, errors="replace")
    for line in p.stdout:
        for rx, lvl, fmt in JOURNAL_RULES:
            m = rx.search(line)
            if m:
                emit(lvl, fmt.format(*m.groups()))
                break


# ------------------------------------------------------------------- main

class Bridge:
    def __init__(self, args):
        self.args = args
        self.link = Link(args.port)
        self.mic = VirtualMic()
        self.spk = VirtualSpeaker(self.link)
        self.stats = Stats()
        self.known_hosts = None
        self.running = True
        self.hud = None

    def log(self, lvl: int, text: str) -> None:
        print(f"[ops] {text}", flush=True)
        try:
            self.link.send(MSG_LOG, bytes([lvl]) + ascii_clean(text, LOG_MAX))
        except (serial.SerialException, ValueError):
            pass

    def alisha_cmd(self, phrase: str) -> None:
        try:
            with socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM) as s:
                s.sendto(phrase.encode(), CTL_SOCK)
        except OSError:
            self.log(LOG_WARN, "Alisha not running")

    def hud_listener(self) -> None:
        """Receive {"t": "state|heard|reply|log", "v": ...} datagrams from Alisha."""
        if os.path.exists(HUD_SOCK):
            os.unlink(HUD_SOCK)
        self.hud = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
        self.hud.bind(HUD_SOCK)
        os.chmod(HUD_SOCK, 0o600)
        while self.running:
            r, _, _ = select.select([self.hud], [], [], 1.0)
            if not r:
                continue
            data = self.hud.recv(1024)
            try:
                msg = json.loads(data.decode("utf-8", "replace"))
                t, v = msg.get("t"), msg.get("v", "")
            except (ValueError, AttributeError):
                continue
            if t == "state" and v in STATES:
                self.link.send(MSG_STATE, bytes([STATES[v]]))
            elif t == "heard":
                self.link.send(MSG_HEARD, ascii_clean(v, TEXT_MAX))
            elif t == "reply":
                self.link.send(MSG_REPLY, ascii_clean(v, TEXT_MAX))
            elif t == "log":
                self.log(LOG_INFO, str(v))
            elif t == "saver":
                self.link.send(MSG_SAVER, bytes([1 if v else 0]))

    def send_hosts(self, hosts) -> None:
        """Real LAN hosts (from the ARP table) for the screensaver's radar. Max 32."""
        ips = []
        for ip, _mac in sorted(hosts, key=lambda h: tuple(int(o) for o in h[0].split("."))):
            try:
                ips.append(socket.inet_aton(ip))
            except OSError:
                continue
        ips = ips[:32]
        self.link.send(MSG_HOSTS, bytes([len(ips)]) + b"".join(ips))

    def periodic(self) -> None:
        last_time = 0.0
        while self.running:
            try:
                self.link.send(MSG_STATS, self.stats.pack())
                now = time.time()
                if now - last_time > 60:
                    off = int((time.mktime(time.localtime(now)) - time.mktime(time.gmtime(now))) / 60)
                    self.link.send(MSG_TIME, struct.pack("<Ih", int(now), off))
                    last_time = now
                hosts = self.stats.lan_hosts()
                if int(now) % 5 == 0:
                    self.send_hosts(hosts)
                if self.known_hosts is not None:
                    for ip, mac in sorted(hosts - self.known_hosts):
                        if ip not in {h[0] for h in self.known_hosts}:
                            self.log(LOG_ALERT, f"NEW HOST {ip} {mac}")
                    self.known_hosts |= hosts
                else:
                    self.known_hosts = set(hosts)
            except serial.SerialException:
                self.running = False
            time.sleep(1.0)

    def run(self) -> None:
        if not self.args.levels:
            self.mic.start()
            try:
                self.spk.start()
                print(f"[+] virtual speaker '{SINK_NAME}' ready", flush=True)
            except Exception as e:
                print(f"[!] speaker unavailable: {e}", flush=True)
            print(f"[+] virtual mic '{SOURCE_NAME}' is now the default input", flush=True)
        if self.args.mic is not None or self.args.gain is not None:
            ch = 0xFF if self.args.mic == -1 else (self.args.mic or 0)
            self.link.send(MSG_CONFIG, bytes([ch, self.args.gain if self.args.gain is not None else 30]))

        threading.Thread(target=self.periodic, daemon=True).start()
        threading.Thread(target=self.hud_listener, daemon=True).start()
        threading.Thread(target=journal_watcher, args=(self.log,), daemon=True).start()
        self.log(LOG_OK, "Pi bridge online")

        last_ping = 0.0
        for typ, p in self.link.frames():
            if not self.running:
                break
            if time.monotonic() - last_ping > 1.0:
                self.link.send(MSG_PING)
                last_ping = time.monotonic()
            if typ is None:
                continue
            if typ == MSG_AUDIO and len(p) % 2 == 0 and not self.args.levels:
                self.mic.write(p)
            elif typ == MSG_LEVELS and len(p) == 8:
                if self.args.levels:
                    lv = struct.unpack("<4H", p)
                    bars = "  ".join(f"ch{i}:{v:5d} {'#' * min(30, v // 200):<30}" for i, v in enumerate(lv))
                    print("\r" + bars, end="", flush=True)
            elif typ == MSG_EVENT and len(p) == 2:
                code, arg = p
                if code == EVT_MIC_MUTED:
                    self.log(LOG_WARN if arg else LOG_OK, "MIC MUTED (device)" if arg else "MIC LIVE")
                elif code == EVT_ACTION and arg in ACTIONS:
                    self.log(LOG_INFO, f"ACTION {ACTIONS[arg]}")
                    self.alisha_cmd(ACTIONS[arg])
                elif code == EVT_BOOT:
                    print("[i] device booted", flush=True)
            elif typ == MSG_HELLO and len(p) == 16:
                fw = p[1:11].split(b"\x00", 1)[0].decode("ascii", "replace")
                chip = (p[12] << 8) | p[13]
                spk = "ES8311 speaker OK" if chip == 0x8311 and p[14] & 2 else f"speaker chip id 0x{chip:04x}, ready={bool(p[14] & 2)}"
                print(f"[+] device: proto v{p[0]} fw {fw} | mic={'yes' if p[14] & 1 else 'no'} | {spk}", flush=True)

    def stop(self) -> None:
        self.running = False
        self.mic.stop()
        self.spk.stop()
        if os.path.exists(HUD_SOCK):
            os.unlink(HUD_SOCK)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", default=DEFAULT_PORT)
    ap.add_argument("--levels", action="store_true", help="show per-channel mic levels only")
    ap.add_argument("--mic", type=int, choices=[-1, 0, 1, 2, 3], help="ES7210 channel to use (-1 = mix)")
    ap.add_argument("--saver", choices=["on", "off"], help="show/hide the screensaver, then exit")
    ap.add_argument("--gain", type=int, choices=range(0, 43), metavar="0-42", help="mic gain in dB")
    args = ap.parse_args()

    os.umask(0o077)
    if args.saver:
        # if the bridge is running, ask it; otherwise talk to the device directly
        if os.path.exists(HUD_SOCK):
            with socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM) as s:
                s.sendto(json.dumps({"t": "saver", "v": args.saver == "on"}).encode(), HUD_SOCK)
        else:
            link = Link(args.port)
            for _ in range(3):
                link.send(MSG_PING)
                link.send(MSG_SAVER, bytes([1 if args.saver == "on" else 0]))
                time.sleep(0.2)
        return
    b = Bridge(args)

    def _sig(*_):
        b.stop()
        sys.exit(0)
    signal.signal(signal.SIGTERM, _sig)
    signal.signal(signal.SIGINT, _sig)
    try:
        b.run()
    finally:
        b.stop()


if __name__ == "__main__":
    main()
