#!/usr/bin/env python3
# ALISHA / Iron Man HUD wallpaper generator (Pillow). Local only, no downloads.
import sys, math, socket
from PIL import Image, ImageDraw, ImageFont, ImageFilter

W, H = 3440, 1440
OUT = sys.argv[1]
CYAN = (31, 182, 255)
host = socket.gethostname().upper()

def font(sz, bold=False):
    paths = [
        "/usr/share/fonts/truetype/hack/Hack-Bold.ttf" if bold else "/usr/share/fonts/truetype/hack/Hack-Regular.ttf",
        "/usr/share/fonts/truetype/firacode/FiraCode-Bold.ttf" if bold else "/usr/share/fonts/truetype/firacode/FiraCode-Regular.ttf",
    ]
    for p in paths:
        try:
            return ImageFont.truetype(p, sz)
        except Exception:
            pass
    return ImageFont.load_default()

# --- base radial gradient (near-black centre -> deep blue-black edge) ---
base = Image.new("RGB", (W, H))
px = base.load()
cx, cy = W // 2, H // 2
maxd = math.hypot(cx, cy)
c_in = (6, 10, 16)
c_out = (1, 3, 10)
for y in range(H):
    for x in range(0, W, 4):  # step 4 for speed, fill block
        d = math.hypot(x - cx, y - cy) / maxd
        r = int(c_in[0] + (c_out[0] - c_in[0]) * d)
        g = int(c_in[1] + (c_out[1] - c_in[1]) * d)
        b = int(c_in[2] + (c_out[2] - c_in[2]) * d)
        for k in range(4):
            if x + k < W:
                px[x + k, y] = (r, g, b)

draw = ImageDraw.Draw(base, "RGBA")

# --- faint grid ---
step = 80
for x in range(0, W + 1, step):
    draw.line([(x, 0), (x, H)], fill=(18, 58, 90, 28), width=1)
for y in range(0, H + 1, step):
    draw.line([(0, y), (W, y)], fill=(18, 58, 90, 28), width=1)

# --- arc-reactor concentric rings on a transparent layer, then glow ---
rings = Image.new("RGBA", (W, H), (0, 0, 0, 0))
rd = ImageDraw.Draw(rings)
def ring(r, op, w):
    rd.ellipse([cx - r, cy - r, cx + r, cy + r], outline=(31, 182, 255, int(255 * op)), width=w)
for r, op, w in [(380, 0.55, 3), (300, 0.38, 2), (230, 0.30, 2), (150, 0.22, 2), (92, 0.65, 4)]:
    ring(r, op, w)
# tick marks
for a in range(0, 360, 10):
    rad = math.radians(a)
    r1, r2 = (392, 410) if a % 30 == 0 else (394, 406)
    rd.line([(cx + math.cos(rad) * r1, cy + math.sin(rad) * r1),
             (cx + math.cos(rad) * r2, cy + math.sin(rad) * r2)],
            fill=(31, 182, 255, 140), width=2)
# broken outer arcs (HUD style)
for a0, a1 in [(20, 80), (110, 160), (200, 250), (280, 340)]:
    rd.arc([cx - 450, cy - 450, cx + 450, cy + 450], a0, a1, fill=(31, 182, 255, 70), width=2)

glow = rings.filter(ImageFilter.GaussianBlur(6))
base = Image.alpha_composite(base.convert("RGBA"), glow)
base = Image.alpha_composite(base, rings)
draw = ImageDraw.Draw(base, "RGBA")

# --- corner HUD text ---
draw.text((72, 56), "A.L.I.S.H.A.", font=font(40, True), fill=(31, 182, 255, 235))
draw.text((74, 112), "Advanced Learning Intelligent System for Human Assistance", font=font(22), fill=(74, 127, 160, 220))
draw.text((72, 150), "KALI LINUX  //  RASPBERRY PI 5", font=font(18), fill=(47, 106, 138, 200))

msg = f"{host}  //  ALL SYSTEMS NOMINAL"
f2 = font(20)
tb = draw.textbbox((0, 0), msg, font=f2)
draw.text((W - (tb[2] - tb[0]) - 72, H - 84), msg, font=f2, fill=(47, 106, 138, 210))
draw.text((W - 360, H - 120), "● ONLINE", font=font(18, True), fill=(31, 182, 255, 220))

base.convert("RGB").save(OUT, "PNG")
print("Wrote", OUT, base.size)
