#!/usr/bin/env python3
"""
DEEP DIVE - live hacker-console wallpaper + screensaver for the Pi desktop.

Four panels in a 2x2 grid, all driven by REAL data from this machine:
  01 RADAR    sweep over the actual LAN hosts in the ARP table
  02 GLOBE    spinning wireframe globe with traceroute arcs
  03 DECRYPT  hex stream + a payload decrypting into this node's identity
  04 SIGNAL   live oscilloscope + voiceprint from the default mic (screensaver)
              / network traffic spectrum (wallpaper)

Usage
  deepdive.py --window                 animated, in a normal window (try it out)
  deepdive.py --fullscreen             animated, full screen (Esc / any key quits)
  deepdive.py                          screensaver mode: draws into $XSCREENSAVER_WINDOW
  deepdive.py --wallpaper OUT.png      render one full-resolution frame
  deepdive.py --wallpaper-loop         re-render + set the XFCE wallpaper every 60 s

Privacy: the SIGNAL panel reads mic *levels* only while the screensaver is on screen.
Nothing is recorded, stored or sent anywhere. All data stays on this machine.
"""
import argparse
import array
import math
import os
import random
import shutil
import socket
import subprocess
import sys
import threading
import time

os.environ.setdefault("PYGAME_HIDE_SUPPORT_PROMPT", "1")

# ------------------------------------------------------------------ palette
BG = (2, 8, 6)
GREEN = (57, 255, 20)
DGREEN = (14, 74, 34)
MGREEN = (24, 140, 50)
CYAN = (0, 229, 255)
MAG = (255, 42, 212)
AMBER = (255, 176, 0)
RED = (255, 42, 42)
WHITE = (216, 255, 232)


# =================================================================== data
class Data:
    """Live facts about this machine. Cheap to poll; all reads are local."""

    def __init__(self, want_mic=False):
        self.host = socket.gethostname()
        self.prev_cpu = None
        self.prev_net = None
        self.cpu = 0
        self.mem = 0
        self.temp = 0.0
        self.uptime = 0
        self.ip = "0.0.0.0"
        self.hosts = []                       # [(ip, mac)]
        self.rx_hist = [0] * 96
        self.tx_hist = [0] * 96
        self.wave = array.array("h", [0] * 512)
        self.levels = [0] * 96
        self.mic_ok = False
        self.lock = threading.Lock()
        self.poll()
        if want_mic:
            threading.Thread(target=self._mic, daemon=True).start()

    @staticmethod
    def _read(p, d=""):
        try:
            with open(p) as f:
                return f.read()
        except OSError:
            return d

    def poll(self):
        f = [int(x) for x in self._read("/proc/stat").split("\n", 1)[0].split()[1:]]
        idle, total = f[3] + f[4], sum(f)
        if self.prev_cpu:
            dt = total - self.prev_cpu[1]
            self.cpu = int(100 * (1 - (idle - self.prev_cpu[0]) / dt)) if dt else 0
        self.prev_cpu = (idle, total)
        m = {}
        for line in self._read("/proc/meminfo").splitlines():
            k, v = line.split(":", 1)
            m[k] = int(v.split()[0])
        self.mem = int(100 * (1 - m.get("MemAvailable", 0) / max(m.get("MemTotal", 1), 1)))
        self.temp = int(self._read("/sys/class/thermal/thermal_zone0/temp", "0").strip() or 0) / 1000
        self.uptime = int(float(self._read("/proc/uptime", "0 0").split()[0]))
        try:
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
                s.connect(("192.0.2.1", 9))           # no packet is sent
                self.ip = s.getsockname()[0]
        except OSError:
            self.ip = "offline"
        hosts = []
        for line in self._read("/proc/net/arp").splitlines()[1:]:
            p = line.split()
            if len(p) >= 4 and p[2] == "0x2":
                hosts.append((p[0], p[3]))
        self.hosts = sorted(set(hosts), key=lambda h: tuple(int(o) for o in h[0].split(".")))
        rx = tx = 0
        for line in self._read("/proc/net/dev").splitlines()[2:]:
            name, d = line.split(":", 1)
            if name.strip() != "lo":
                d = d.split()
                rx += int(d[0]); tx += int(d[8])
        now = time.monotonic()
        if self.prev_net:
            dt = max(now - self.prev_net[2], 1e-3)
            self.rx_hist = self.rx_hist[1:] + [max(0, int((rx - self.prev_net[0]) / dt))]
            self.tx_hist = self.tx_hist[1:] + [max(0, int((tx - self.prev_net[1]) / dt))]
        self.prev_net = (rx, tx, now)

    def _mic(self):
        if not shutil.which("parec"):
            return
        try:
            p = subprocess.Popen(["parec", "--rate=16000", "--channels=1", "--format=s16le", "--raw",
                                  "--latency-msec=50"], stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
        except OSError:
            return
        self.mic_ok = True
        while True:
            data = p.stdout.read(1024)
            if not data:
                self.mic_ok = False
                return
            a = array.array("h", data)
            rms = math.sqrt(sum(s * s for s in a) / len(a)) if a else 0
            lvl = max(0, min(100, int((20 * math.log10(rms + 1) - 30) * 2.2)))
            with self.lock:
                self.wave = self.wave[len(a):] + a
                self.levels = self.levels[1:] + [lvl]


# ================================================================ drawing
class Deck:
    def __init__(self, pg, size, data, mode, dock=None, area=None):
        self.pg = pg
        self.W, self.H = size
        self.data = data
        self.mode = mode                     # "saver" | "wallpaper"
        self.s = max(self.H / 720.0, 0.25)   # scale relative to a 1280x720-ish design
        self.font_path = self._font()
        self.f_s = pg.font.Font(self.font_path, max(8, int(13 * self.s)))
        self.f_m = pg.font.Font(self.font_path, max(9, int(17 * self.s)))
        self.f_l = pg.font.Font(self.font_path, max(10, int(30 * self.s)))
        self.f_xl = pg.font.Font(self.font_path, max(12, int(46 * self.s)))
        self.screen = pg.Surface((self.W, self.H))
        self.glow = pg.Surface((self.W, self.H))
        self.overlay = self._scanlines()          # scanlines + vignette baked into one layer
        self.overlay.blit(self._vignette(), (0, 0))
        self.hex_lines = [self._hex_row() for _ in range(64)]
        self.hex_addr = 0x7F3A0000
        self.arcs = []
        self.glow_amt = {}
        self.term_idx = 0
        self.term_t0 = 0.0
        self.glitch_until = 0.0
        self.next_glitch = 6.0
        self.anim = mode in ("saver", "live")     # animated (vs one still frame)
        self.dock = None                          # frame around the conky A.L.I.S.H.A. panel
        # usable area = screen minus desktop panels (from _NET_WORKAREA); whole screen if unknown
        ax, ay, aw, ah = area or (0, 0, self.W, self.H)
        self.area = pg.Rect(ax, ay, aw, ah)
        pad, top, bot = int(14 * self.s), int(40 * self.s), int(40 * self.s)
        ch = (ah - top - bot - pad) // 2
        top += ay
        row1_right = ax + aw - pad
        if dock:                                  # dock = conky window rect, in render pixels
            m = max(4, int(7 * self.s))
            self.dock = pg.Rect(dock).inflate(2 * m, 2 * m)
            row1_right = self.dock.left - pad
        cw1 = (row1_right - ax - 2 * pad) // 2
        cw2 = (aw - 3 * pad) // 2
        self.panels = [
            pg.Rect(ax + pad, top, cw1, ch), pg.Rect(ax + 2 * pad + cw1, top, row1_right - (ax + 2 * pad + cw1), ch),
            pg.Rect(ax + pad, top + ch + pad, cw2, ch),
            pg.Rect(ax + 2 * pad + cw2, top + ch + pad, aw - 3 * pad - cw2, ch),
        ]

    # ------------------------------------------------------------ helpers
    def _font(self):
        for name in ("hack", "firacode", "dejavusansmono", "liberationmono"):
            p = self.pg.font.match_font(name)
            if p:
                return p
        return None

    def _scanlines(self):
        pg = self.pg
        s = pg.Surface((self.W, self.H), pg.SRCALPHA)
        step = max(2, int(3 * self.s))
        for y in range(0, self.H, step):
            pg.draw.line(s, (0, 0, 0, 55), (0, y), (self.W, y))
        return s

    def _vignette(self):
        pg = self.pg
        small = pg.Surface((64, 32), pg.SRCALPHA)
        for y in range(32):
            for x in range(64):
                d = math.hypot((x - 31.5) / 32, (y - 15.5) / 16)
                small.set_at((x, y), (0, 0, 0, int(max(0, min(1, (d - 0.55) / 0.6)) * 170)))
        return pg.transform.smoothscale(small, (self.W, self.H))

    def text(self, surf, font, color, s, pos, glow=False, anchor="topleft"):
        img = font.render(s, True, color)
        r = img.get_rect(**{anchor: pos})
        surf.blit(img, r)
        if glow:
            self.glow.blit(img, r.move(surf.get_abs_offset()) if surf is not self.screen else r)
        return r

    def line(self, surf, color, a, b, w=1, glow=False):
        if w <= 1:
            self.pg.draw.aaline(surf, color, a, b)
        else:
            self.pg.draw.line(surf, color, a, b, w)
        if glow:
            off = surf.get_abs_offset()
            self.pg.draw.line(self.glow, color, (a[0] + off[0], a[1] + off[1]), (b[0] + off[0], b[1] + off[1]), max(2, w + 1))

    def frame(self, r, title, idx, t):
        pg, sc = self.pg, self.screen
        pg.draw.rect(sc, (5, 16, 11), r)
        pg.draw.rect(sc, DGREEN, r, 1)
        c = int(18 * self.s)
        for (x, y, dx, dy) in ((r.left, r.top, 1, 1), (r.right - 1, r.top, -1, 1),
                               (r.left, r.bottom - 1, 1, -1), (r.right - 1, r.bottom - 1, -1, -1)):
            pg.draw.line(sc, GREEN, (x, y), (x + dx * c, y), 2)
            pg.draw.line(sc, GREEN, (x, y), (x, y + dy * c), 2)
        label = f"[{idx:02d}] {title}"
        self.text(sc, self.f_s, MAG, label, (r.left + int(12 * self.s), r.top + int(6 * self.s)))
        blink = "●" if int(t * 2) % 2 else " "
        self.text(sc, self.f_s, RED, f"{blink} LIVE", (r.right - int(12 * self.s), r.top + int(6 * self.s)), anchor="topright")

    # ------------------------------------------------------------- panels
    def radar(self, r, t):
        pg = self.pg
        sub = self.screen.subsurface(r)
        cx, cy = int(r.w * 0.36), int(r.h * 0.54)
        R = int(min(r.h * 0.40, r.w * 0.30))
        ang = (t * 150) % 360
        # phosphor trail
        trail = pg.Surface((r.w, r.h), pg.SRCALPHA)
        for i in range(24):
            a0 = math.radians(ang - i * 2.2)
            a1 = math.radians(ang - (i + 1) * 2.2)
            alpha = int(120 * (1 - i / 24) ** 1.6)
            pg.draw.polygon(trail, (*GREEN, alpha), [(cx, cy), (cx + R * math.cos(a0), cy + R * math.sin(a0)),
                                                       (cx + R * math.cos(a1), cy + R * math.sin(a1))])
        sub.blit(trail, (0, 0))
        for k in range(1, 5):
            pg.draw.circle(sub, MGREEN if k < 4 else GREEN, (cx, cy), R * k // 4, 1)
        pg.draw.line(sub, DGREEN, (cx - R, cy), (cx + R, cy))
        pg.draw.line(sub, DGREEN, (cx, cy - R), (cx, cy + R))
        for a in range(0, 360, 10):
            rr = R + (6 if a % 30 == 0 else 3) * self.s
            x0, y0 = cx + R * math.cos(math.radians(a)), cy + R * math.sin(math.radians(a))
            pg.draw.line(sub, GREEN, (x0, y0), (cx + rr * math.cos(math.radians(a)), cy + rr * math.sin(math.radians(a))))
        ex, ey = cx + R * math.cos(math.radians(ang)), cy + R * math.sin(math.radians(ang))
        self.line(sub, WHITE, (cx, cy), (ex, ey), 2, glow=True)

        hosts = self.data.hosts or []
        gw = self.data.ip.rsplit(".", 1)[0] + ".1" if "." in self.data.ip else ""
        rows = []
        for i, (ip, mac) in enumerate(hosts[:24]):
            h = sum(ord(c) * (k + 7) for k, c in enumerate(ip + mac))
            a = h % 360
            rad = R * (0.25 + (h // 360 % 70) / 100)
            x, y = cx + rad * math.cos(math.radians(a)), cy + rad * math.sin(math.radians(a))
            since = (ang - a) % 360
            g = max(0.15, 1 - since / 300) if self.anim else 0.9
            col = AMBER if ip == gw else GREEN
            c = tuple(int(v * g) for v in col)
            pg.draw.circle(sub, c, (int(x), int(y)), max(3, int(5 * self.s)))
            pg.draw.circle(self.glow, col if g > 0.7 else c, (int(x + r.x), int(y + r.y)), max(4, int(9 * self.s)))
            self.text(sub, self.f_s, c, "." + ip.rsplit(".", 1)[-1], (int(x + 8 * self.s), int(y - 8 * self.s)))
            rows.append((ip, mac, ip == gw, since < 25))
        # side table
        tx = int(r.w * 0.70)
        self.text(sub, self.f_m, CYAN, "NET RECON", (tx, int(r.h * 0.12)))
        self.text(sub, self.f_l, WHITE, f"{len(hosts):02d} HOSTS", (tx, int(r.h * 0.20)), glow=True)
        y = int(r.h * 0.36)
        if not rows:
            self.text(sub, self.f_s, MGREEN, "awaiting ARP data...", (tx, y))
        for ip, mac, is_gw, hot in rows[:9]:
            tag = "GW " if is_gw else ("PING" if hot else "up ")
            col = AMBER if is_gw else (WHITE if hot else GREEN)
            self.text(sub, self.f_s, col, f"{'>' if hot else ' '}{ip:<15} {tag}", (tx, y))
            y += int(19 * self.s)
        self.text(sub, self.f_s, MGREEN, f"self {self.data.ip}", (tx, r.h - int(28 * self.s)))

    def globe(self, r, t):
        pg = self.pg
        sub = self.screen.subsurface(r)
        gx = 0.36
        cx, cy = int(r.w * gx), int(r.h * 0.54)
        R = int(min(r.h * 0.40, r.w * 0.30))
        rot, tilt = t * 14.0, 0.38

        def proj(lat, lon, alt=1.0):
            la, lo = math.radians(lat), math.radians(lon + rot)
            x, y, z = math.cos(la) * math.sin(lo), math.sin(la), math.cos(la) * math.cos(lo)
            y2 = y * math.cos(tilt) - z * math.sin(tilt)
            z2 = y * math.sin(tilt) + z * math.cos(tilt)
            return cx + R * alt * x, cy - R * alt * y2, z2

        pg.draw.circle(sub, (4, 30, 26), (cx, cy), R)
        pg.draw.circle(sub, CYAN, (cx, cy), R + int(8 * self.s), 1)
        for lat in range(-75, 90, 15):
            pts = [proj(lat, lon) for lon in range(0, 361, 8)]
            for a, b in zip(pts, pts[1:]):
                front = (a[2] + b[2]) > 0
                pg.draw.aaline(sub, MGREEN if front else DGREEN, a[:2], b[:2])
        for lon in range(0, 360, 20):
            pts = [proj(lat, lon) for lat in range(-90, 91, 6)]
            for a, b in zip(pts, pts[1:]):
                front = (a[2] + b[2]) > 0
                pg.draw.aaline(sub, MGREEN if front else DGREEN, a[:2], b[:2])
        cities = [(51, 0), (40, -74), (35, 139), (-33, 151), (1, 103), (55, 37), (-23, -46),
                  (37, -122), (19, 72), (30, 31), (60, 25), (-34, 18), (64, -21), (-41, 174)]
        for lat, lon in cities:
            x, y, z = proj(lat, lon)
            if z > 0:
                pg.draw.circle(sub, CYAN, (int(x), int(y)), max(2, int(3 * self.s)))
                pg.draw.circle(self.glow, CYAN, (int(x + r.x), int(y + r.y)), max(3, int(6 * self.s)))
        # traceroute arcs (deterministic in wallpaper mode, live in saver mode)
        rnd = random.Random(int(t // 3) if self.anim else 7)
        if not self.arcs or not self.anim:
            self.arcs = [(rnd.randrange(len(cities)), rnd.randrange(len(cities)), t - k * 1.1, k) for k in range(3)]
        new = []
        for (i, j, t0, k) in self.arcs:
            p = (t - t0) / 3.0 if self.anim else 0.65 + 0.1 * k
            if p > 1.0:
                i, j, t0 = j, rnd.randrange(len(cities)), t
                p = 0
            new.append((i, j, t0, k))
            if i == j:
                continue
            col = (AMBER, MAG, RED)[k % 3]
            la1, lo1 = map(math.radians, cities[i]); la2, lo2 = map(math.radians, cities[j])
            A = (math.cos(la1) * math.cos(lo1), math.cos(la1) * math.sin(lo1), math.sin(la1))
            B = (math.cos(la2) * math.cos(lo2), math.cos(la2) * math.sin(lo2), math.sin(la2))
            om = math.acos(max(-1, min(1, sum(a * b for a, b in zip(A, B)))))
            n = 28
            draw_to = int(n * min(1, p / 0.6))
            pts = []
            for q in range(draw_to + 1):
                u = q / n
                s1 = math.sin((1 - u) * om) / math.sin(om) if om > 1e-3 else 1 - u
                s2 = math.sin(u * om) / math.sin(om) if om > 1e-3 else u
                v = [s1 * a + s2 * b for a, b in zip(A, B)]
                lat = math.degrees(math.asin(max(-1, min(1, v[2]))))
                lon = math.degrees(math.atan2(v[1], v[0]))
                pts.append(proj(lat, lon, 1 + 0.3 * math.sin(u * math.pi))[:2])
            if len(pts) > 1:
                pg.draw.lines(sub, col, False, pts, max(1, int(2 * self.s)))
                pg.draw.lines(self.glow, col, False, [(x + r.x, y + r.y) for x, y in pts], max(2, int(5 * self.s)))
            if p > 0.6:
                rr = int((p - 0.6) * 60 * self.s) + 3
                ex, ey = pts[-1]
                pg.draw.circle(sub, col, (int(ex), int(ey)), rr, 1)
        self.arcs = new
        tx = int(r.w * 0.70)
        self.text(sub, self.f_m, CYAN, "TRACE ROUTE", (tx, int(r.h * 0.12)))
        y = int(r.h * 0.22)
        for (i, j, t0, k) in self.arcs:
            ms = 12 + (i * 37 + j * 11) % 180
            self.text(sub, self.f_s, (AMBER, MAG, RED)[k % 3], f"NODE-{i:02d} -> NODE-{j:02d}", (tx, y))
            self.text(sub, self.f_s, GREEN, f"  hop {3 + (i + j) % 9}   {ms:3d} ms", (tx, y + int(18 * self.s)))
            y += int(48 * self.s)

    def _hex_row(self):
        b = [random.randrange(256) for _ in range(16)]
        asc = "".join(chr(x) if 33 <= x < 127 else "." for x in b)
        return " ".join(f"{x:02x}" for x in b) + "  " + asc

    def decrypt(self, r, t):
        pg = self.pg
        sub = self.screen.subsurface(r)
        lh = int(17 * self.s)
        nl = r.h // lh
        if self.anim and int(t * 12) != getattr(self, "_hex_tick", -1):
            self._hex_tick = int(t * 12)
            self.hex_lines = self.hex_lines[1:] + [self._hex_row()]
            self.hex_addr += 16
        for i in range(min(nl, len(self.hex_lines))):
            addr = self.hex_addr + (i - nl) * 16
            self.text(sub, self.f_s, DGREEN, f"{addr & 0xffffffff:08x}  {self.hex_lines[-nl + i]}",
                      (int(12 * self.s), int(28 * self.s) + i * lh))
        bw, bh = int(r.w * 0.66), int(r.h * 0.44)
        box = pg.Rect((r.w - bw) // 2, (r.h - bh) // 2, bw, bh)
        pg.draw.rect(sub, (3, 12, 8), box)
        phases = ["ACCESS GRANTED", f"NODE {self.data.host.upper()}", f"ADDR {self.data.ip}",
                  f"UPTIME {self.data.uptime // 86400}D {self.data.uptime // 3600 % 24:02d}H {self.data.uptime // 60 % 60:02d}M"]
        cyc = 3.2
        ph = int(t // cyc) % len(phases) if self.anim else 1
        p = (t % cyc) / cyc if self.anim else 1.0
        target = phases[ph]
        lock = int(len(target) * min(1, max(0, (p - 0.1) / 0.55)))
        glyph = "!#$%&*+<>?@ABCDEFGHJKLMNPQRSTUVWXYZ0123456789/\\|="
        shown = "".join(c if (k < lock or c == " ") else random.choice(glyph) for k, c in enumerate(target))
        done = lock >= len(target)
        border = WHITE if done and int(t * 6) % 2 else GREEN
        pg.draw.rect(sub, border, box, max(1, int(2 * self.s)))
        pg.draw.rect(self.glow, border, box.move(r.x, r.y), max(2, int(3 * self.s)))
        self.text(sub, self.f_s, CYAN, "DECRYPTING PAYLOAD  //  AES-256-GCM", (box.x + int(18 * self.s), box.y + int(14 * self.s)))
        font = self.f_xl if self.f_xl.size(target)[0] < box.w - int(40 * self.s) else self.f_l
        self.text(sub, font, GREEN if done else WHITE, shown, (box.centerx, box.centery - int(6 * self.s)),
                  glow=True, anchor="center")
        bar = pg.Rect(box.x + int(18 * self.s), box.bottom - int(34 * self.s), box.w - int(110 * self.s), int(10 * self.s))
        pg.draw.rect(sub, DGREEN, bar)
        pct = 100 if done else int(min(99, p / 0.65 * 100))
        pg.draw.rect(sub, GREEN, (bar.x, bar.y, bar.w * pct // 100, bar.h))
        self.text(sub, self.f_s, GREEN, f"{pct:3d}%", (bar.right + int(12 * self.s), bar.y - int(3 * self.s)))

    def signal(self, r, t):
        pg = self.pg
        sub = self.screen.subsurface(r)
        live_mic = self.mode == "saver" and self.data.mic_ok
        title = "SIGNAL INTERCEPT // MIC" if live_mic else "SIGNAL INTERCEPT // NET TRAFFIC"
        self.text(sub, self.f_m, CYAN, title, (int(16 * self.s), int(30 * self.s)))
        scope = pg.Rect(int(16 * self.s), int(58 * self.s), r.w - int(32 * self.s), int(r.h * 0.40))
        pg.draw.rect(sub, DGREEN, scope, 1)
        for k in range(1, 4):
            y = scope.y + scope.h * k // 4
            pg.draw.line(sub, (8, 40, 20), (scope.x, y), (scope.right, y))
        for k in range(1, 10):
            x = scope.x + scope.w * k // 10
            pg.draw.line(sub, (8, 40, 20), (x, scope.y), (x, scope.bottom))
        mid = scope.centery
        if live_mic:
            with self.data.lock:
                w = list(self.data.wave)
                levels = list(self.data.levels)
            peak = max(600, max(abs(v) for v in w))
            n = 256
            pts = [(scope.x + i * scope.w / (n - 1), mid - w[i * len(w) // n] * (scope.h * 0.45) / peak) for i in range(n)]
            lvl = levels[-1]
        else:
            # synthetic carrier modulated by real traffic
            rx = self.data.rx_hist
            mx = max(max(rx), 2048)
            amp = 0.15 + 0.8 * (rx[-1] / mx)
            n = 200
            pts = [(scope.x + i * scope.w / (n - 1),
                    mid - scope.h * 0.42 * amp * math.sin(i * 0.21 + t * 6) * math.sin(i * 0.031 + t)) for i in range(n)]
            levels = [int(100 * v / mx) for v in rx]
            lvl = levels[-1]
        col = CYAN if lvl > 40 else GREEN
        pg.draw.lines(sub, col, False, pts, max(1, int(2 * self.s)))
        pg.draw.lines(self.glow, col, False, [(x + r.x, y + r.y) for x, y in pts], max(2, int(5 * self.s)))
        # voiceprint / traffic bars
        by = scope.bottom + int(40 * self.s)
        bh = r.h - by - int(30 * self.s)
        self.text(sub, self.f_s, CYAN, "VOICEPRINT" if live_mic else "RX SPECTRUM", (scope.x, scope.bottom + int(12 * self.s)))
        nb = len(levels)
        bwid = scope.w / nb
        for i, v in enumerate(levels):
            h = max(2, int(bh * v / 100))
            c = RED if v > 70 else AMBER if v > 40 else GREEN
            pg.draw.rect(sub, c, (scope.x + i * bwid + 1, by + bh - h, max(1, bwid - 2), h))
        db = -60 + lvl * 0.6
        state = ("VOICE DETECTED" if lvl > 40 else "listening") if live_mic else (
            f"rx {self._rate(self.data.rx_hist[-1])}  tx {self._rate(self.data.tx_hist[-1])}")
        self.text(sub, self.f_l, WHITE, f"{db:+.0f} dB" if live_mic else state, (scope.right, scope.bottom + int(6 * self.s)),
                  glow=True, anchor="topright")
        if live_mic:
            self.text(sub, self.f_s, CYAN if lvl > 40 else GREEN, f"[ {state} ]",
                      (scope.right - int(220 * self.s), scope.bottom + int(14 * self.s)), anchor="topright")

    @staticmethod
    def _rate(b):
        for unit in ("B/s", "KB/s", "MB/s"):
            if b < 1000:
                return f"{b:.0f}{unit}"
            b /= 1000
        return f"{b:.1f}GB/s"

    # --------------------------------------------------------------- chrome
    TERM = ["$ arp-scan --localnet", "$ nmap -sV -T4 --top-ports 100 gateway", "[+] 3 services fingerprinted",
            "$ tcpdump -i wlan0 -c 500 port 53", "$ hashcat -m 22000 capture.hc22000",
            "[*] candidates 41.2 kH/s  ETA 03:12", "$ ssh -J bastion ops@node-07", "[+] tunnel up  aes256-gcm",
            "$ tail -f /var/log/auth.log", "[!] intrusion detection: nominal"]

    def chrome(self, t):
        sc, d = self.screen, self.data
        top = f"DEEP DIVE  //  {d.host}  //  {d.ip}  //  CPU {d.cpu:3d}%  MEM {d.mem:3d}%  {d.temp:4.1f}C"
        A = self.area
        self.text(sc, self.f_m, GREEN, top, (A.x + int(14 * self.s), A.y + int(10 * self.s)), glow=True)
        right = (self.dock.left - int(14 * self.s)) if self.dock else A.right - int(14 * self.s)
        self.text(sc, self.f_m, WHITE, time.strftime("%H:%M:%S  %a %d %b"),
                  (right, A.y + int(10 * self.s)), anchor="topright")
        # terminal along the bottom
        line = self.TERM[self.term_idx]
        el = t - self.term_t0
        k = int(el * 22)
        if k > len(line) + 40:
            self.term_idx = (self.term_idx + 1) % len(self.TERM)
            self.term_t0 = t
            k = 0
        if self.mode == "wallpaper":
            line, k = "$ deepdive --live --nodes all", 999
        cur = "_" if int(t * 2) % 2 else " "
        col = AMBER if line.startswith("[!]") else CYAN if line.startswith("[") else GREEN
        self.text(sc, self.f_m, col, line[:k] + cur, (A.x + int(14 * self.s), A.bottom - int(30 * self.s)), glow=True)

    # --------------------------------------------------------------- frame
    def render(self, t):
        pg = self.pg
        self.screen.fill(BG)
        self.glow.fill((0, 0, 0))
        names = ["RADAR", "GLOBE", "DECRYPT", "SIGNAL"]
        fns = [self.radar, self.globe, self.decrypt, self.signal]
        for i, (r, name, fn) in enumerate(zip(self.panels, names, fns)):
            self.frame(r, name, i + 1, t)
            fn(r, t)
        self.chrome(t)
        # bloom: blur the glow layer and add it on top
        # fast bloom: nearest-pixel shrink, smooth blur at tiny size, smooth back up
        quarter = pg.transform.scale(self.glow, (max(1, self.W // 4), max(1, self.H // 4)))
        small = pg.transform.smoothscale(quarter, (max(1, self.W // 10), max(1, self.H // 10)))
        small.fill((170, 170, 170), special_flags=pg.BLEND_MULT)    # dim while tiny: ~free
        bloom = pg.transform.smoothscale(small, (self.W, self.H))
        self.screen.blit(bloom, (0, 0), special_flags=pg.BLEND_ADD)
        self.screen.blit(self.overlay, (0, 0))
        # glitch (rarer on the live wallpaper than in the screensaver)
        if self.anim:
            if t > self.next_glitch:
                self.glitch_until = t + random.uniform(0.15, 0.4)
                self.next_glitch = t + (random.uniform(7, 14) if self.mode == "saver" else random.uniform(25, 50))
            if t < self.glitch_until:
                self._glitch()
        if self.dock:
            self.dock_frame()      # drawn last: identical in the still and the live wallpaper
        return self.screen

    def dock_frame(self):
        """Frame that the conky A.L.I.S.H.A. panel sits in exactly. Never animated: conky's
        pseudo-transparent background copies this area from the static desktop image."""
        pg, sc, d = self.pg, self.screen, self.dock
        pg.draw.rect(sc, (3, 11, 10), d)
        pg.draw.rect(sc, (0, 90, 100), d, 1)
        c, w = int(22 * self.s), max(2, int(3 * self.s))
        for (x, y, dx, dy) in ((d.left, d.top, 1, 1), (d.right - 1, d.top, -1, 1),
                               (d.left, d.bottom - 1, 1, -1), (d.right - 1, d.bottom - 1, -1, -1)):
            pg.draw.line(sc, CYAN, (x, y), (x + dx * c, y), w)
            pg.draw.line(sc, CYAN, (x, y), (x, y + dy * c), w)
        # thin tick marks along the left edge, like a bezel
        for y in range(d.top + c + 6, d.bottom - c - 6, max(6, int(14 * self.s))):
            pg.draw.line(sc, (0, 70, 80), (d.left - max(3, int(5 * self.s)), y), (d.left - 1, y))

    def _glitch(self):
        pg, sc = self.pg, self.screen
        copy = sc.copy()
        for _ in range(random.randint(4, 9)):
            h = random.randint(2, max(3, self.H // 30))
            y = random.randint(0, self.H - h)
            dx = random.randint(-self.W // 30, self.W // 30)
            sc.blit(copy, (dx, y), pg.Rect(0, y, self.W, h))
        tint = copy.copy()
        tint.fill((255, 0, 180), special_flags=pg.BLEND_MULT)
        sc.blit(tint, (random.randint(3, 9), 0), special_flags=pg.BLEND_ADD)


# ================================================================== modes
def run_animated(args):
    win_id = os.environ.get("XSCREENSAVER_WINDOW")
    if win_id and not (args.window or args.fullscreen):
        os.environ["SDL_WINDOWID"] = str(int(win_id, 0))
    import pygame as pg
    pg.display.init()
    pg.font.init()
    if args.fullscreen:
        screen = pg.display.set_mode((0, 0), pg.FULLSCREEN)
    elif args.window:
        screen = pg.display.set_mode((1720, 720))
        pg.display.set_caption("DEEP DIVE")
    else:
        size = (0, 0)
        if win_id and shutil.which("xwininfo"):
            out = subprocess.run(["xwininfo", "-id", win_id], capture_output=True, text=True).stdout
            w = [l for l in out.splitlines() if "Width:" in l]
            h = [l for l in out.splitlines() if "Height:" in l]
            if w and h:
                size = (int(w[0].split()[-1]), int(h[0].split()[-1]))
        screen = pg.display.set_mode(size)
    pg.mouse.set_visible(False)
    W, H = screen.get_size()
    # render below native resolution on big screens, then scale up (keeps ~25-30 fps on a Pi 5)
    scale = 2 if W > 1920 else 1
    data = Data(want_mic=True)
    deck = Deck(pg, (W // scale, H // scale), data, "saver")
    clock = pg.time.Clock()
    t0 = time.monotonic()
    last_poll = 0.0
    while True:
        for e in pg.event.get():
            if e.type == pg.QUIT:
                return
            if (args.window or args.fullscreen) and e.type in (pg.KEYDOWN, pg.MOUSEBUTTONDOWN):
                return
        t = time.monotonic() - t0
        if t - last_poll > 1.0:
            data.poll()
            last_poll = t
        frame = deck.render(t)
        if scale != 1:
            pg.transform.scale(frame, (W, H), screen)          # crisp CRT-style pixels, ~8 ms
        else:
            screen.blit(frame, (0, 0))
        pg.display.flip()
        clock.tick(30)


def render_wallpaper(path, size, data=None, dock=None, area=None):
    os.environ["SDL_VIDEODRIVER"] = "dummy"
    import pygame as pg
    pg.display.init()
    pg.font.init()
    data = data or Data()
    deck = Deck(pg, size, data, "wallpaper", dock=dock, area=area)
    img = deck.render(37.3)
    tmp = path + ".tmp.png"
    pg.image.save(img, tmp)
    os.replace(tmp, path)


def workarea():
    """(x, y, w, h) the window manager leaves free of panels, or None."""
    try:
        out = subprocess.run(["xprop", "-root", "_NET_WORKAREA"], capture_output=True, text=True, timeout=3).stdout
        v = [int(x) for x in out.split("=", 1)[1].replace(",", " ").split()[:4]]
        return tuple(v) if v[2] > 0 and v[3] > 0 else None
    except Exception:
        return None


def conky_rect():
    """(x, y, w, h) of the visible conky window, or None."""
    try:
        ids = subprocess.run(["xdotool", "search", "--class", "conky"], capture_output=True,
                             text=True, timeout=3).stdout.split()
        for wid in ids:
            out = subprocess.run(["xwininfo", "-id", wid], capture_output=True, text=True, timeout=3).stdout
            if "IsViewable" not in out:
                continue
            g = {}
            for line in out.splitlines():
                line = line.strip()
                for key in ("Absolute upper-left X", "Absolute upper-left Y", "Width", "Height"):
                    if line.startswith(key + ":"):
                        g[key] = int(line.split(":")[1])
            if len(g) == 4 and g["Width"] > 50:
                return (g["Absolute upper-left X"], g["Absolute upper-left Y"], g["Width"], g["Height"]), int(wid)
    except Exception:
        pass
    return None, None


_flip = [0]


def refresh_static(data=None):
    """Render the still wallpaper for the current screen + conky position and set it in XFCE.
    It is also what conky's pseudo-transparent background shows, so it must match the live frame."""
    out_dir = os.path.expanduser("~/Pictures/wallpapers")
    os.makedirs(out_dir, exist_ok=True)
    path = os.path.join(out_dir, f"deepdive-{_flip[0]}.png")    # alternate names so xfdesktop reloads
    _flip[0] ^= 1
    dock, _ = conky_rect()
    render_wallpaper(path, screen_size(), data, dock=dock, area=workarea())
    set_wallpaper(path)
    return path


def screen_size():
    try:
        out = subprocess.run(["xrandr", "--current"], capture_output=True, text=True, timeout=5).stdout
        for line in out.splitlines():
            if "*" in line:
                w, h = line.split()[0].split("x")
                return int(w), int(h)
    except Exception:
        pass
    return 1920, 1080


def set_wallpaper(path):
    """Point every XFCE monitor/workspace backdrop at `path` (zoom to fill)."""
    out = subprocess.run(["xfconf-query", "-c", "xfce4-desktop", "-l"], capture_output=True, text=True).stdout
    props = [p for p in out.split() if p.endswith("/last-image")]
    try:
        mons = [l.split()[0] for l in subprocess.run(["xrandr", "--current"], capture_output=True, text=True).stdout.splitlines()
                if " connected" in l]
        nws = int(subprocess.run(["xfconf-query", "-c", "xfwm4", "-p", "/general/workspace_count"],
                                 capture_output=True, text=True).stdout.strip() or 4)
        for m in mons:
            for w in range(nws):
                p = f"/backdrop/screen0/monitor{m}/workspace{w}/last-image"
                if p not in props:
                    props.append(p)
    except Exception:
        pass
    for p in props:
        subprocess.run(["xfconf-query", "-c", "xfce4-desktop", "-p", p, "-n", "-t", "string", "-s", path])
        subprocess.run(["xfconf-query", "-c", "xfce4-desktop", "-p", p.replace("last-image", "image-style"),
                        "-n", "-t", "int", "-s", "5"])


def wallpaper_loop(interval):
    os.environ.setdefault("DISPLAY", ":0")
    data = Data()
    while True:
        data.poll()
        refresh_static(data)
        for _ in range(int(interval)):
            time.sleep(1)
            data.poll()          # keeps the traffic history filled between frames


def run_live(fps=10):
    """Animated wallpaper in a click-through desktop-level window behind everything,
    with conky kept on top. Adapts to screen-size and conky changes on the fly."""
    os.environ.setdefault("DISPLAY", ":0")
    from Xlib import X, Xatom, display as xdisplay
    from Xlib.ext import shape  # noqa: F401  (registers the SHAPE extension)

    xd = xdisplay.Display()
    scr = xd.screen()
    root = scr.root
    A = xd.intern_atom

    def screen_geom():
        g = root.get_geometry()
        return g.width, g.height

    W, H = screen_geom()
    win = root.create_window(0, 0, W, H, 0, scr.root_depth, X.InputOutput, X.CopyFromParent,
                             background_pixel=0, event_mask=X.StructureNotifyMask)
    win.set_wm_name("DEEP DIVE live wallpaper")
    win.set_wm_class("deepdive", "DeepDive")
    win.change_property(A("_NET_WM_WINDOW_TYPE"), Xatom.ATOM, 32, [A("_NET_WM_WINDOW_TYPE_DESKTOP")])
    win.change_property(A("_NET_WM_STATE"), Xatom.ATOM, 32,
                        [A(n) for n in ("_NET_WM_STATE_BELOW", "_NET_WM_STATE_STICKY",
                                        "_NET_WM_STATE_SKIP_TASKBAR", "_NET_WM_STATE_SKIP_PAGER")])
    win.change_property(A("_NET_WM_DESKTOP"), Xatom.CARDINAL, 32, [0xFFFFFFFF])
    # click-through: an empty input region, so the desktop (right-click menu, icons) still gets clicks
    win.shape_rectangles(0, 2, 0, 0, 0, [])          # ShapeSet, ShapeInput, Unsorted
    win.map()
    xd.sync()

    os.environ["SDL_WINDOWID"] = str(win.id)
    import pygame as pg
    pg.display.init()
    pg.font.init()
    data = Data()
    state = {}

    def build():
        W, H = screen_geom()
        scale = 2 if W > 1920 else 1
        dock, cid = conky_rect()
        area = workarea()
        state.update(W=W, H=H, scale=scale, dock=dock, cid=cid, area=area,
                     screen=pg.display.set_mode((W, H)))
        sd = tuple(v // scale for v in dock) if dock else None
        sa = tuple(v // scale for v in area) if area else None
        state["deck"] = Deck(pg, (W // scale, H // scale), data, "live", dock=sd, area=sa)
        refresh_static(data)                       # conky's backdrop must match the new layout

    def keep_stacking():
        """Our window just above xfdesktop, conky above us. xfwm4 keeps desktop-type windows in
        the order they were mapped and ignores restack requests between them, so if conky ended
        up underneath, restart it with its exact command line (at most once a minute)."""
        try:
            stack = list(root.get_full_property(A("_NET_CLIENT_LIST_STACKING"), Xatom.WINDOW).value)
            _, cid = conky_rect()
            state["cid"] = cid
            if not (cid and win.id in stack and cid in stack and stack.index(cid) < stack.index(win.id)):
                return
            if time.monotonic() - state.get("conky_restart", -1e9) < 60:
                return
            state["conky_restart"] = time.monotonic()
            pid = subprocess.run(["pgrep", "-x", "conky"], capture_output=True, text=True).stdout.split()
            if not pid:
                return
            with open(f"/proc/{pid[0]}/cmdline", "rb") as f:
                cmd = [a.decode() for a in f.read().split(b"\0") if a]
            os.kill(int(pid[0]), 15)
            time.sleep(0.8)
            subprocess.Popen(cmd, start_new_session=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        except Exception:
            pass

    def desktop_covered():
        """True when the active window is maximised or full screen: no point drawing."""
        try:
            act = root.get_full_property(A("_NET_ACTIVE_WINDOW"), Xatom.WINDOW)
            if not act or not act.value[0]:
                return False
            w = xd.create_resource_object("window", act.value[0])
            st = w.get_full_property(A("_NET_WM_STATE"), Xatom.ATOM)
            st = set(st.value) if st else set()
            return A("_NET_WM_STATE_FULLSCREEN") in st or {
                A("_NET_WM_STATE_MAXIMIZED_VERT"), A("_NET_WM_STATE_MAXIMIZED_HORZ")} <= st
        except Exception:
            return False

    def click_through():
        """Empty input region on our window AND on the frame xfwm4 wraps it in; otherwise the
        frame swallows clicks and the desktop right-click menu never opens."""
        try:
            win.shape_rectangles(0, 2, 0, 0, 0, [])
            parent = win.query_tree().parent
            if parent.id != root.id:
                parent.shape_rectangles(0, 2, 0, 0, 0, [])
            xd.sync()
        except Exception:
            pass

    time.sleep(0.3)                                  # let xfwm4 reparent us into its frame
    click_through()
    build()
    keep_stacking()
    clock = pg.time.Clock()
    t0 = time.monotonic()
    last_check = last_poll = 0.0
    covered = False
    while True:
        # SDL turns SIGTERM/SIGINT into a QUIT event: honour it so `kill` stops us cleanly
        if any(e.type == pg.QUIT for e in pg.event.get()):
            pg.display.quit()
            win.destroy()
            xd.sync()
            return
        while xd.pending_events():
            xd.next_event()
        now = time.monotonic()
        if now - last_poll > 1.0:
            data.poll()
            last_poll = now
        if now - last_check > 3.0:
            last_check = now
            W, H = screen_geom()
            dock, _ = conky_rect()
            if (W, H) != (state["W"], state["H"]) or dock != state["dock"] or workarea() != state["area"]:
                if (W, H) != (state["W"], state["H"]):
                    win.configure(width=W, height=H)
                    xd.sync()
                build()                              # screen or conky changed: re-layout
            keep_stacking()
            click_through()
            covered = desktop_covered()
        if covered:
            time.sleep(0.5)                          # nothing visible: use no CPU
            continue
        frame = state["deck"].render(now - t0)
        if state["scale"] != 1:
            pg.transform.scale(frame, (state["W"], state["H"]), state["screen"])
        else:
            state["screen"].blit(frame, (0, 0))
        pg.display.flip()
        clock.tick(fps)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--window", action="store_true")
    ap.add_argument("--fullscreen", action="store_true")
    ap.add_argument("--wallpaper", metavar="OUT.png")
    ap.add_argument("--size", default="", help="WxH for --wallpaper (default: current screen)")
    ap.add_argument("--wallpaper-loop", action="store_true")
    ap.add_argument("--live", action="store_true", help="animated live wallpaper (needs python-xlib: use .venv)")
    ap.add_argument("--fps", type=int, default=10)
    ap.add_argument("--interval", type=int, default=60)
    ap.add_argument("-root", action="store_true", help=argparse.SUPPRESS)   # xscreensaver-style flag
    args = ap.parse_args()
    if args.wallpaper:
        size = tuple(int(v) for v in args.size.split("x")) if args.size else screen_size()
        d = Data()
        time.sleep(1)
        d.poll()
        render_wallpaper(args.wallpaper, size, d, dock=conky_rect()[0], area=workarea())
    elif args.live:
        run_live(args.fps)
    elif args.wallpaper_loop:
        wallpaper_loop(args.interval)
    else:
        run_animated(args)


if __name__ == "__main__":
    main()
