#!/usr/bin/env python3
"""Draw the README artwork for the GitHub pages (Pillow only, no downloads).

    python3 scripts/make_readme_art.py

Writes:
  docs/banner.png               ALISHA header banner
  docs/hud-gallery.png          six HUD screens from the desktop preview (esp32-hud/sim/shots)
  ~/Documents/HushType/hushtype/docs/banner.png   HushType header banner
Run esp32-hud/sim/build.sh first so the HUD screenshots are current.
"""
import math, os, random
from PIL import Image, ImageDraw, ImageFilter, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HUSH = os.path.expanduser("~/Documents/HushType/hushtype")
HACK = "/usr/share/fonts/truetype/hack/Hack-{}.ttf"
BG, CYAN, GREEN, VIOLET, DIM, MUTED, TEXT = (3, 7, 11), (34, 211, 238), (52, 211, 153), (167, 139, 250), (16, 42, 51), (95, 127, 136), (226, 243, 247)


def font(size, bold=False):
    return ImageFont.truetype(HACK.format("Bold" if bold else "Regular"), size)


def spaced(draw, xy, text, f, fill, spacing):
    """Text with extra letter spacing; returns the end x."""
    x, y = xy
    for ch in text:
        draw.text((x, y), ch, font=f, fill=fill)
        x += draw.textlength(ch, font=f) + spacing
    return x


def spaced_len(draw, text, f, spacing):
    return sum(draw.textlength(c, font=f) + spacing for c in text) - spacing


def base(w, h, seed):
    random.seed(seed)
    img = Image.new("RGB", (w, h), BG)
    d = ImageDraw.Draw(img)
    for x in range(0, w, 32):                       # faint grid
        d.line([(x, 0), (x, h)], fill=(8, 20, 26))
    for y in range(0, h, 32):
        d.line([(0, y), (w, y)], fill=(8, 20, 26))
    for _ in range(140):                            # data dust
        x, y = random.randrange(w), random.randrange(h)
        d.point((x, y), fill=(20, 70, 84))
    return img


def brackets(d, box, color, n=26, t=3):
    x0, y0, x1, y1 = box
    for (cx, cy, sx, sy) in ((x0, y0, 1, 1), (x1, y0, -1, 1), (x0, y1, 1, -1), (x1, y1, -1, -1)):
        d.line([(cx, cy), (cx + sx * n, cy)], fill=color, width=t)
        d.line([(cx, cy), (cx, cy + sy * n)], fill=color, width=t)


def glow_layer(size, draw_fn, radius):
    layer = Image.new("RGB", size, (0, 0, 0))
    draw_fn(ImageDraw.Draw(layer))
    return layer.filter(ImageFilter.GaussianBlur(radius))


def add(img, layer):
    """Additive blend so glows brighten the background."""
    a, b = img.load(), layer.load()
    w, h = img.size
    for y in range(h):
        for x in range(w):
            p, q = a[x, y], b[x, y]
            if q != (0, 0, 0):
                a[x, y] = (min(255, p[0] + q[0]), min(255, p[1] + q[1]), min(255, p[2] + q[2]))


def orb(d, cx, cy, color, letter="A"):
    for i in range(48):                             # VU tick ring
        a = math.radians(i * 7.5 - 90)
        r0, r1 = (78, 92) if i % 6 == 0 else (84, 92)
        lit = i < 31
        c = color if lit else DIM
        d.line([(cx + r0 * math.cos(a), cy + r0 * math.sin(a)), (cx + r1 * math.cos(a), cy + r1 * math.sin(a))],
               fill=c, width=3)
    for k, (span, shade) in enumerate(((80, 0.25), (46, 0.5), (16, 1.0))):   # radar sweep trail
        c = tuple(int(v * shade) for v in color)
        d.arc([cx - 70, cy - 70, cx + 70, cy + 70], 300 - span, 300, fill=c, width=4)
    for i in range(24):                             # dashed inner ring
        a = math.radians(i * 15)
        d.line([(cx + 52 * math.cos(a), cy + 52 * math.sin(a)), (cx + 57 * math.cos(a), cy + 57 * math.sin(a))],
               fill=tuple(v // 2 for v in color), width=2)
    d.ellipse([cx - 40, cy - 40, cx + 40, cy + 40], outline=color, width=3, fill=tuple(v // 5 for v in color))
    f = font(46, True)
    w = d.textlength(letter, font=f)
    d.text((cx - w / 2, cy - 30), letter, font=f, fill=(240, 251, 255))


def banner_alisha(out):
    W, H = 1280, 360
    img = base(W, H, 7)
    cx, cy = 210, 180
    add(img, glow_layer((W, H), lambda g: g.ellipse([cx - 46, cy - 46, cx + 46, cy + 46], fill=(0, 120, 140)), 22))
    d = ImageDraw.Draw(img)
    orb(d, cx, cy, CYAN)
    brackets(d, (24, 24, W - 25, H - 25), CYAN)
    x = 360
    spaced(d, (x, 92), "A.L.I.S.H.A.", font(64, True), CYAN, 6)
    d.text((x + 2, 178), "ADVANCED LEARNING INTELLIGENT SYSTEM FOR HUMAN ASSISTANCE", font=font(17), fill=MUTED)
    d.line([(x + 2, 214), (x + 760, 214)], fill=DIM, width=2)
    d.line([(x + 2, 214), (x + 260, 214)], fill=CYAN, width=2)
    tags = ["OFFLINE VOICE AI", "RASPBERRY PI 5", "ESP32-S3 HUD", "KALI LINUX"]
    tx = x + 2
    for t in tags:
        f = font(15, True)
        w = d.textlength(t, font=f)
        d.rectangle([tx, 236, tx + w + 24, 266], outline=(27, 155, 179), width=1, fill=(8, 18, 26))
        d.text((tx + 12, 242), t, font=f, fill=TEXT)
        tx += w + 38
    d.text((x + 2, 290), "> status: ALL SYSTEMS NOMINAL_", font=font(16), fill=GREEN)
    img.save(out)


def banner_hushtype(out):
    W, H = 1280, 360
    img = base(W, H, 11)
    cx, cy = 210, 180
    add(img, glow_layer((W, H), lambda g: g.rounded_rectangle([cx - 70, cy - 70, cx + 70, cy + 70], 24, fill=(70, 50, 140)), 26))
    d = ImageDraw.Draw(img)
    # waveform turning into typed text
    random.seed(3)
    for i in range(26):
        x = cx - 104 + i * 8
        h = int(10 + 60 * abs(math.sin(i * 0.55)) * (0.4 + random.random() * 0.6))
        d.rounded_rectangle([x, cy - h // 2, x + 4, cy + h // 2], 2, fill=VIOLET if i < 13 else CYAN)
    d.rounded_rectangle([cx - 120, cy - 96, cx + 120, cy + 96], 18, outline=(70, 60, 120), width=2)
    brackets(d, (24, 24, W - 25, H - 25), VIOLET)
    x = 380
    spaced(d, (x, 84), "HUSHTYPE", font(72, True), TEXT, 8)
    d.text((x + 4, 176), "Speak it. It types. Nothing leaves your machine.", font=font(24), fill=VIOLET)
    d.line([(x + 4, 222), (x + 760, 222)], fill=DIM, width=2)
    d.line([(x + 4, 222), (x + 300, 222)], fill=VIOLET, width=2)
    tags = ["100% OFFLINE", "WHISPER.CPP", "ANY APP", "LINUX / X11"]
    tx = x + 4
    for t in tags:
        f = font(15, True)
        w = d.textlength(t, font=f)
        d.rectangle([tx, 244, tx + w + 24, 274], outline=(110, 90, 190), width=1, fill=(12, 10, 24))
        d.text((tx + 12, 250), t, font=f, fill=TEXT)
        tx += w + 38
    d.text((x + 4, 296), '> "um so meet at 3, no wait, 4"  ->  "So meet at 4."', font=font(16), fill=GREEN)
    img.save(out)


def gallery(out):
    shots = os.path.join(ROOT, "esp32-hud/sim/shots")
    picks = [("00_boot", "BOOT SEQUENCE"), ("02_listening", "LISTENING"), ("04_speaking", "SPEAKING"),
             ("06_sys", "SYSTEM"), ("07_net", "NETWORK"), ("09_keys", "CONTROLS")]
    tw, th, pad, cap = 640, 480, 24, 40
    W = 3 * tw + 4 * pad
    H = 2 * (th + cap) + 3 * pad
    img = base(W, H, 5)
    d = ImageDraw.Draw(img)
    for i, (name, title) in enumerate(picks):
        x = pad + (i % 3) * (tw + pad)
        y = pad + (i // 3) * (th + cap + pad)
        d.text((x, y + 6), f"[{i + 1:02d}] {title}", font=font(20, True), fill=CYAN)
        shot = Image.open(os.path.join(shots, name + ".png")).convert("RGB")
        img.paste(shot, (x, y + cap))
        brackets(d, (x - 6, y + cap - 6, x + tw + 5, y + cap + th + 5), (27, 155, 179), n=18, t=2)
    img.save(out, optimize=True)


if __name__ == "__main__":
    os.makedirs(os.path.join(ROOT, "docs"), exist_ok=True)
    os.makedirs(os.path.join(HUSH, "docs"), exist_ok=True)
    banner_alisha(os.path.join(ROOT, "docs/banner.png"))
    gallery(os.path.join(ROOT, "docs/hud-gallery.png"))
    banner_hushtype(os.path.join(HUSH, "docs/banner.png"))
    print("art written")
