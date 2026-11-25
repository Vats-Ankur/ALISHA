#!/usr/bin/env python3
"""Miro-style architecture boards for the ALISHA and HushType READMEs.

    python3 scripts/make_architecture.py

Writes an editable SVG plus a PNG (rendered with headless Chromium) for each board:
  docs/architecture.svg|png                              ALISHA
  ~/Documents/HushType/hushtype/docs/architecture.svg|png HushType
The board style imitates a whiteboard: dotted canvas, titled frames, sticky notes,
component cards and numbered connectors that follow one request end to end.
"""
import html, os, shutil, subprocess, tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HUSH = os.path.expanduser("~/Documents/HushType/hushtype")

FONT = "Lato, 'Open Sans', 'Segoe UI', Helvetica, Arial, sans-serif"
INK, SUB, LINE = "#1a1a2e", "#5b6170", "#2b2f3a"
STICKY = {  # whiteboard sticky-note colours
    "yellow": "#fff6a8", "green": "#d3f5b0", "blue": "#c4e3ff", "pink": "#ffd1e3",
    "violet": "#e3d8ff", "orange": "#ffdcb0", "grey": "#eceef2",
}
ACCENT = {"cyan": "#0fb5d4", "green": "#2fbf71", "violet": "#7c5cff", "amber": "#f0a400",
          "red": "#ef4565", "blue": "#3d7bfd", "grey": "#8a93a3"}


class Board:
    def __init__(self, w, h):
        self.w, self.h, self.out = w, h, []

    def add(self, s):
        self.out.append(s)

    def text(self, x, y, s, size=15, weight=400, color=INK, anchor="start", italic=False):
        st = ' font-style="italic"' if italic else ""
        self.add(f'<text x="{x}" y="{y}" font-family="{FONT}" font-size="{size}" font-weight="{weight}" '
                 f'fill="{color}" text-anchor="{anchor}"{st}>{html.escape(s)}</text>')

    def lines(self, x, y, lines, size=14, gap=1.35, **kw):
        for i, s in enumerate(lines):
            self.text(x, y + i * size * gap, s, size=size, **kw)

    def frame(self, x, y, w, h, title, color="grey"):
        self.add(f'<rect x="{x}" y="{y}" width="{w}" height="{h}" rx="6" fill="#ffffff" '
                 f'stroke="#d5d8de" stroke-width="1.5"/>')
        self.add(f'<rect x="{x}" y="{y}" width="{w}" height="5" rx="2" fill="{ACCENT[color]}"/>')
        self.text(x + 2, y - 10, title, size=15, weight=700, color=SUB)

    def sticky(self, x, y, w, h, lines, color="yellow", size=14, bold_first=False):
        self.add(f'<rect x="{x + 3}" y="{y + 4}" width="{w}" height="{h}" fill="#000" opacity="0.08"/>')
        self.add(f'<rect x="{x}" y="{y}" width="{w}" height="{h}" fill="{STICKY[color]}"/>')
        for i, s in enumerate(lines):
            self.text(x + 12, y + 24 + i * size * 1.35, s, size=size,
                      weight=700 if (bold_first and i == 0) else 400)

    def card(self, x, y, w, h, title, lines=(), color="cyan", icon=""):
        self.add(f'<rect x="{x}" y="{y + 3}" width="{w}" height="{h}" rx="10" fill="#000" opacity="0.06"/>')
        self.add(f'<rect x="{x}" y="{y}" width="{w}" height="{h}" rx="10" fill="#ffffff" '
                 f'stroke="{ACCENT[color]}" stroke-width="2"/>')
        self.add(f'<rect x="{x}" y="{y}" width="8" height="{h}" rx="4" fill="{ACCENT[color]}"/>')
        self.text(x + 20, y + 26, (icon + "  " if icon else "") + title, size=16, weight=700)
        self.lines(x + 20, y + 48, list(lines), size=13, color=SUB)

    def arrow(self, pts, label=None, lx=None, ly=None, color=LINE, dashed=False, step=None, sx=None, sy=None):
        d = "M " + " L ".join(f"{x} {y}" for x, y in pts)
        dash = ' stroke-dasharray="7 6"' if dashed else ""
        self.add(f'<path d="{d}" fill="none" stroke="{color}" stroke-width="2.2"{dash} '
                 f'stroke-linejoin="round" marker-end="url(#ah)"/>')
        if label:
            if lx is None:
                (x1, y1), (x2, y2) = pts[len(pts) // 2 - 1], pts[len(pts) // 2]
                lx, ly = (x1 + x2) / 2, (y1 + y2) / 2
            w = 7.2 * len(label) + 18
            self.add(f'<rect x="{lx - w / 2}" y="{ly - 12}" width="{w}" height="24" rx="12" fill="#ffffff" '
                     f'stroke="#d5d8de"/>')
            self.text(lx, ly + 5, label, size=12.5, color=SUB, anchor="middle")
        if step is not None:
            self.badge(sx if sx is not None else pts[0][0], sy if sy is not None else pts[0][1], step)

    def badge(self, x, y, n, color="#ff5a5f"):
        self.add(f'<circle cx="{x}" cy="{y}" r="14" fill="{color}" stroke="#fff" stroke-width="2.5"/>')
        self.text(x, y + 5, str(n), size=14, weight=900, color="#ffffff", anchor="middle")

    def svg(self):
        dots = ('<pattern id="dots" width="24" height="24" patternUnits="userSpaceOnUse">'
                '<circle cx="2" cy="2" r="1.4" fill="#cfd2d8"/></pattern>')
        marker = ('<marker id="ah" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="7" markerHeight="7" '
                  f'orient="auto-start-reverse"><path d="M0 0 L10 5 L0 10 z" fill="{LINE}"/></marker>')
        return (f'<svg xmlns="http://www.w3.org/2000/svg" width="{self.w}" height="{self.h}" '
                f'viewBox="0 0 {self.w} {self.h}"><defs>{dots}{marker}</defs>'
                f'<rect width="100%" height="100%" fill="#f6f6f3"/><rect width="100%" height="100%" fill="url(#dots)"/>'
                + "".join(self.out) + "</svg>")


def header(b, title, subtitle, credit):
    b.text(48, 62, title, size=34, weight=900)
    b.text(48, 94, subtitle, size=17, color=SUB)
    b.text(b.w - 48, 62, credit, size=14, color=SUB, anchor="end")
    b.text(b.w - 48, 86, "Architecture board · 2026", size=14, color=SUB, anchor="end")


def legend(b, x, y, items):
    b.sticky(x, y, 250, 30 + 22 * len(items), ["Legend"] + items, color="grey", size=13, bold_first=True)


# ---------------------------------------------------------------------------- ALISHA
def alisha():
    b = Board(2280, 1000)
    header(b, "A.L.I.S.H.A.  —  System Architecture",
           "Advanced Learning Intelligent System for Human Assistance · offline voice assistant + holographic HUD",
           "Ankur Vats · github.com/Vats-Ankur/ALISHA")

    # --- YOU
    b.frame(48, 160, 250, 500, "YOU", "red")
    b.sticky(72, 190, 200, 92, ['"Alisha, system', 'status"', "speak a command"], "yellow")
    b.sticky(72, 300, 200, 70, ["Hear the reply", "(board speaker)"], "yellow")
    b.sticky(72, 388, 200, 70, ["Watch the HUD", "(320x240 screen)"], "yellow")
    b.sticky(72, 476, 200, 92, ["Buttons", "UP / DOWN = pages", "BOOT = hardware mute"], "yellow")
    b.sticky(72, 586, 200, 52, ['"Alisha, type …"'], "violet")

    # --- BOARD
    bx, by = 360, 160
    b.frame(bx, by, 640, 760, "ESP32-S3 HUD BOARD  ·  firmware hud-0.4 (ESP-IDF 5.4 · LVGL 9)", "cyan")
    b.text(bx + 24, by + 40, "HARDWARE", size=13, weight=700, color=SUB)
    b.sticky(bx + 24, by + 54, 138, 70, ["ES7210", "4-mic array"], "blue", bold_first=True)
    b.sticky(bx + 176, by + 54, 138, 70, ["ES8311", "DAC + amp"], "blue", bold_first=True)
    b.sticky(bx + 328, by + 54, 138, 70, ["LCD", "320 x 240"], "blue", bold_first=True)
    b.sticky(bx + 480, by + 54, 138, 70, ["Buttons", "UP DOWN BOOT"], "blue", bold_first=True)
    b.card(bx + 24, by + 176, 288, 96, "audio_task", ["16 kHz mic capture, levels", "echo guard while speaking"], "cyan")
    b.card(bx + 328, by + 176, 288, 96, "speaker_task", ["240 ms jitter buffer", "amp on only while playing"], "cyan")
    b.card(bx + 24, by + 290, 288, 118, "ui.c  ·  HUD", ["drives the LCD: 5 pages", "VU orb, radar sweep, holo panels",
                                                         "boot splash, decrypt/typewriter"], "violet")
    b.card(bx + 328, by + 290, 288, 118, "saver.c  ·  DEEP DIVE", ["radar of real LAN hosts", "globe, decrypt, live mic scope",
                                                                   "after 3 min idle"], "violet")
    b.card(bx + 24, by + 426, 592, 96, "link.c  ·  USB serial protocol",
           ["frames A5 5A · type · seq · len · payload · CRC-16  —  921600 baud, exact sizes per type"], "amber")
    b.sticky(bx + 24, by + 546, 290, 150, ["SECURITY BY DESIGN", "no Wi-Fi / Bluetooth linked", "no secrets stored on device",
                                           "mute enforced in firmware", "hardened build (-Werror, canaries)"], "pink", 13, True)
    b.sticky(bx + 330, by + 546, 286, 150, ["MIC  ->  Pi", "MSG_AUDIO  20 ms PCM", "MSG_LEVELS  4 channels",
                                            "Pi  ->  BOARD", "STATS · STATE · HEARD · REPLY", "SPEAKER PCM · HOSTS · TIME"],
             "orange", 13, True)

    # --- PI
    px, py = 1150, 160
    b.frame(px, py, 1082, 760, "RASPBERRY PI 5  ·  KALI LINUX  ·  everything runs locally", "green")
    b.card(px + 28, py + 40, 330, 210, "alisha-hud-bridge", ["serial link + reconnect", "virtual mic  -> PipeWire",
                                                             "virtual speaker <- PipeWire", "stats · ARP hosts · journal",
                                                             "unix sockets (mode 0600)", "systemd user service"], "amber")
    b.card(px + 28, py + 290, 330, 120, "PipeWire", ["alisha_esp32_mic (source)", "alisha_esp32_speaker (sink)",
                                                     "16 kHz mono"], "grey")
    b.card(px + 400, py + 40, 340, 370, "alisha-voice", [], "green")
    b.sticky(px + 420, py + 90, 300, 56, ["Vosk wake spotter", '"Alisha" + variants'], "green", 13, True)
    b.sticky(px + 420, py + 160, 300, 56, ["Whisper command capture", "record until 1.2 s pause"], "green", 13, True)
    b.sticky(px + 420, py + 230, 300, 56, ["Intent router", "phrase -> action, confirm risky"], "green", 13, True)
    b.sticky(px + 420, py + 300, 300, 92, ["Piper neural voice", "en_GB Cori · streamed per", "sentence · 150 Hz high-pass"],
             "green", 13, True)
    b.frame(px + 780, py + 64, 276, 346, "HUSHTYPE (companion repo)", "violet")
    b.card(px + 798, py + 84, 240, 96, "whisper-server", ["whisper.cpp · base.en", "127.0.0.1:8178 only"], "violet")
    b.card(px + 798, py + 196, 240, 96, "cleanup.clean()", ["fillers, corrections,", "dictionary"], "violet")
    b.card(px + 798, py + 308, 240, 84, "dictation", ['hotkey or "Alisha, type"'], "violet")
    b.text(px + 28, py + 456, "ACTIONS", size=13, weight=700, color=SUB)
    acts = [("status / time", "spoken readout"), ("open apps", "terminal, browser, burp"),
            ("case lights", "pironman5 (exact sudo)"), ("lock / sleep", "screen, wake word"),
            ("shut down", 'only after "confirm"')]
    for i, (t, s) in enumerate(acts):
        b.sticky(px + 28 + i * 208, py + 470, 196, 70, [t, s], "yellow", 13, True)
    b.frame(px + 28, py + 590, 500, 146, "DESKTOP", "blue")
    b.sticky(px + 44, py + 610, 150, 110, ["conky", "A.L.I.S.H.A.", "status panel"], "blue", 13, True)
    b.sticky(px + 206, py + 610, 150, 110, ["DEEP DIVE", "live wallpaper", "+ screensaver"], "blue", 13, True)
    b.sticky(px + 368, py + 610, 146, 110, ["zsh greeting", "+ fastfetch", "banner"], "blue", 13, True)
    b.frame(px + 560, py + 590, 494, 146, "HARDENING", "red")
    b.sticky(px + 576, py + 610, 150, 110, ["nftables", "inbound drop,", "LAN SSH only"], "pink", 13, True)
    b.sticky(px + 738, py + 610, 150, 110, ["sshd", "keys only,", "post-quantum kex"], "pink", 13, True)
    b.sticky(px + 900, py + 610, 140, 110, ["sudo", "exact commands", "only"], "pink", 13, True)

    # --- connectors (the life of one command): right-angle routes through clear lanes
    b.arrow([(272, 236), (bx + 24, 236)], step=1, sx=318, sy=236)                       # speak -> mic
    b.arrow([(bx + 93, by + 124), (bx + 93, by + 176)])                                  # mic -> audio_task
    b.arrow([(bx + 616, by + 474), (1120, by + 474), (1120, py + 140), (px + 28, py + 140)],
            "USB · CRC frames", lx=1060, ly=by + 474, step=2, sx=1120, sy=py + 300)   # board -> bridge
    b.arrow([(px + 193, py + 250), (px + 193, py + 290)])                               # bridge -> PipeWire
    b.arrow([(px + 358, py + 330), (px + 379, py + 330), (px + 379, py + 118), (px + 420, py + 118)],
            step=3, sx=px + 379, sy=py + 230)                                           # virtual mic -> wake
    b.arrow([(px + 720, py + 188), (px + 760, py + 188), (px + 760, py + 132), (px + 798, py + 132)],
            step=4, sx=px + 760, sy=py + 166)                                           # capture -> whisper
    b.arrow([(px + 918, py + 180), (px + 918, py + 196)])
    b.arrow([(px + 798, py + 258), (px + 720, py + 258)], "clean text", lx=px + 760, ly=py + 236)
    b.arrow([(px + 570, py + 410), (px + 570, py + 470)], step=5, sx=px + 596, sy=py + 440)  # voice -> actions
    b.arrow([(px + 420, py + 380), (px + 358, py + 380)], step=6, sx=px + 389, sy=py + 400)  # piper -> sink
    b.arrow([(px + 28, py + 70), (1040, py + 70), (1040, by + 224), (bx + 616, by + 224)],
            "sound + HUD", lx=1040, ly=py + 140, step=7, sx=1080, sy=py + 70)          # bridge -> board
    b.arrow([(bx + 360, by + 176), (bx + 360, by + 150), (bx + 245, by + 150), (bx + 245, by + 124)])  # -> DAC

    legend(b, 48, 690, ["1 speak   2 board -> Pi", "3 wake word   4 Whisper", "5 run action   6 Piper voice",
                        "7 sound + HUD -> board"])
    b.sticky(48, 850, 250, 60, ["No cloud. No API keys.", "Nothing leaves the desk."], "pink", 13, True)
    b.text(48, b.h - 40, "Every box runs on the user's own hardware. Connector numbers follow one voice command "
                         "from microphone to reply.", size=14, color=SUB, italic=True)
    return b


# ---------------------------------------------------------------------------- HushType
def hushtype():
    b = Board(2110, 1000)
    header(b, "HushType  —  Architecture",
           "Speak it. It types. Nothing leaves your machine. · private, offline voice typing for Linux",
           "Ankur Vats · github.com/Vats-Ankur/hushtype")
    cols = [(48, "TRIGGER", "red"), (388, "CAPTURE", "amber"), (728, "TRANSCRIBE", "cyan"),
            (1068, "CLEAN", "green"), (1408, "POLISH  (optional)", "grey"), (1748, "OUTPUT", "violet")]
    for x, t, c in cols:
        b.frame(x, 160, 310, 560, t, c)

    b.card(68, 190, 270, 110, "Hotkey", ["hold Right Ctrl (pynput)", "or toggle mode"], "red")
    b.card(68, 320, 270, 130, "Voice trigger socket", ["$XDG_RUNTIME_DIR/", "hushtype.sock", "listen  |  text:<words>"], "red")
    b.sticky(68, 470, 270, 92, ['"Alisha, type …"', "ALISHA hands dictation", "to HushType"], "violet", 13, True)
    b.sticky(68, 580, 270, 116, ["busy flag", "hushtype.busy exists while", "recording, so other voice apps",
                                 "stay quiet"], "yellow", 13, True)

    b.card(408, 190, 270, 130, "Recorder", ["pw-record / parecord / arecord", "16 kHz mono WAV", "temp file, deleted after"], "amber")
    b.card(408, 340, 270, 110, "record_until_silence", ["voice trigger: stops after", "1.2 s of quiet (end_silence)"], "amber")
    b.sticky(408, 470, 270, 92, ["Silence gate", "near-silent clips are", "dropped, not transcribed"], "yellow", 13, True)
    b.sticky(408, 580, 270, 116, ["Feedback", "start / stop beeps", "notify-send toasts", "HUD: listening -> thinking"],
             "orange", 13, True)

    b.card(748, 190, 270, 150, "whisper-server", ["whisper.cpp HTTP server", "bound to 127.0.0.1:8178", "model base.en  (small.en",
                                                  "for more accuracy)"], "cyan")
    b.card(748, 360, 270, 110, "Prompt hint", ["names you say often", "(config: whisper.prompt)"], "cyan")
    b.sticky(748, 490, 270, 116, ["Shared engine", "ALISHA reuses this server", "for voice commands"], "violet", 13, True)

    tricks = [("Fillers", "um, uh, hmm removed"), ("Corrections", '"3, no wait, 4" -> 4'), ("Scratch that", "drops last sentence"),
              ("New line", "line / paragraph breaks"), ("Dictionary", '"get hub" -> GitHub'), ("Tidy up", "spacing, caps, punctuation")]
    for i, (t, s) in enumerate(tricks):
        b.sticky(1088, 190 + i * 84, 270, 70, [t, s], "green", 13, True)

    b.card(1428, 190, 270, 150, "Local LLM", ["any OpenAI-compatible", "server on this machine", "(Ollama, llama.cpp)",
                                             "off by default"], "grey")
    b.sticky(1428, 360, 270, 92, ["Never a cloud API", "url must point at a", "local server"], "pink", 13, True)

    b.card(1768, 190, 270, 130, "Type", ["xdotool type", "line by line", "shift+Enter for new lines"], "violet")
    b.card(1768, 340, 270, 130, "Paste", ["xclip + Ctrl+V", "Ctrl+Shift+V in terminals", "faster for long text"], "violet")
    b.sticky(1768, 490, 270, 92, ["Any focused app", "terminal, browser,", "editor, chat"], "yellow", 13, True)

    b.arrow([(338, 245), (408, 245)], step=1, sx=360, sy=222)
    b.arrow([(678, 255), (748, 255)], "WAV", step=2, sx=700, sy=222)
    b.arrow([(1018, 222), (1088, 222)], step=3, sx=1053, sy=200)
    b.arrow([(1358, 265), (1428, 265)], step=4, sx=1393, sy=243)
    b.arrow([(1698, 265), (1768, 255)], step=5, sx=1720, sy=222)
    b.arrow([(1358, 640), (1500, 640), (1500, 760), (1903, 760), (1903, 582)], "LLM off: straight to output",
            lx=1700, ly=760, dashed=True)
    b.arrow([(203, 562), (203, 580)], color="#9aa0aa")

    legend(b, 48, 800, ["1 trigger  2 record", "3 transcribe  4 clean", "5 polish (optional)  -> type",
                        "dashed = default path"])
    b.sticky(330, 800, 300, 86, ["PRIVACY", "audio + text never leave", "127.0.0.1 / your machine"], "pink", 13, True)
    b.text(48, b.h - 40, "One utterance travels left to right in about a second on a Raspberry Pi 5.",
           size=14, color=SUB, italic=True)
    return b


def render(board, out_base):
    os.makedirs(os.path.dirname(out_base), exist_ok=True)
    svg = board.svg()
    with open(out_base + ".svg", "w") as f:
        f.write(svg)
    chrome = shutil.which("chromium") or shutil.which("chromium-browser")
    if not chrome:
        print("chromium not found: wrote SVG only")
        return
    with tempfile.TemporaryDirectory() as td:
        page = os.path.join(td, "b.html")
        with open(page, "w") as f:
            f.write(f'<html><body style="margin:0">{svg}</body></html>')
        subprocess.run([chrome, "--headless=new", "--disable-gpu", "--no-sandbox", "--hide-scrollbars",
                        f"--window-size={board.w},{board.h}", "--force-device-scale-factor=1",
                        f"--screenshot={out_base}.png", f"file://{page}"],
                       check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    print("wrote", out_base + ".svg/.png")


if __name__ == "__main__":
    render(alisha(), os.path.join(ROOT, "docs/architecture"))
    render(hushtype(), os.path.join(HUSH, "docs/architecture"))
