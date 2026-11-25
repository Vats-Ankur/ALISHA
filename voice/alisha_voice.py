#!/usr/bin/env python3
"""
A.L.I.S.H.A. (Advanced Learning Intelligent System for Human Assistance) — offline voice command assistant for Kali on Raspberry Pi 5.

Pipeline (all local, no cloud):
  microphone --> Vosk (speech-to-text) --> wake word "alisha" --> command match
             --> action --> Piper/espeak-ng (text-to-speech) response

Run:   ~/alisha-voice/venv/bin/python ~/.config/alisha/voice/alisha_voice.py
Setup: bash ~/.config/alisha/voice/setup_voice.sh   (installs deps + models)

Say:  "Alisha, system status"      -> speaks CPU load, temperature, memory
      "Alisha, what time is it"
      "Alisha, open terminal" / "open browser" / "open burp"
      "Alisha, set lights red|green|blue|rainbow"
      "Alisha, run matrix"
      "Alisha, lock the screen"
      "Alisha, type"               -> HushType dictation: speak, pause, text is typed
      "Alisha, type hello world"   -> types "Hello world" straight away
      "Alisha, go to sleep"        -> stops listening for commands until woken
      "Alisha, shut down"          -> asks for confirmation (say "confirm")
"""
import os, tempfile, sys, json, queue, subprocess, shutil, time, re

WAKE_WORDS = ("alisha", "hey alisha")  # legacy; wake matching uses SPOT_RE / WAKE_RE below
MODEL_DIR = os.path.expanduser("~/alisha-voice/models/vosk-model")
DISPLAY = os.environ.get("DISPLAY", ":0")

# ---------- ESP32 HUD link (optional; silently does nothing if the bridge is not running) ----------
import socket, threading
_RUNTIME = os.environ.get("XDG_RUNTIME_DIR") or f"/run/user/{os.getuid()}"
_HUD_SOCK = os.path.join(_RUNTIME, "alisha-hud.sock")
_CTL_SOCK = os.path.join(_RUNTIME, "alisha-ctl.sock")
# Only these exact phrases are accepted from the HUD's touch buttons.
CTL_ALLOWED = {"system status", "lock the screen", "run matrix", "what time is it"}
CTL_QUEUE = queue.Queue()
GREETING = "Yes, sir, how can I help you?"   # spoken when she is called by name alone
LISTEN_WAIT = 8.0                           # seconds she keeps listening for the command

def hud(t, v=""):
    try:
        with socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM) as s:
            s.sendto(json.dumps({"t": t, "v": v})[:1000].encode(), _HUD_SOCK)
    except OSError:
        pass

def _ctl_listener():
    try:
        if os.path.exists(_CTL_SOCK):
            os.unlink(_CTL_SOCK)
        s = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
        s.bind(_CTL_SOCK)
        os.chmod(_CTL_SOCK, 0o600)
    except OSError:
        return
    while True:
        phrase = s.recv(256).decode("utf-8", "replace").strip().lower()
        if phrase in CTL_ALLOWED:
            CTL_QUEUE.put(phrase)

# ---------- HushType dictation link (~/Documents/HushType) ----------
_LF_SOCK = os.path.join(_RUNTIME, "hushtype.sock")
_LF_BUSY = os.path.join(_RUNTIME, "hushtype.busy")   # exists while HushType records/transcribes
DICTATE_RE = re.compile(r"^\W*(?:type|types|dictate|dictation|write|take a note)\b\W*(.*)$")

def hushtype(msg):
    try:
        with socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM) as s:
            s.sendto(msg[:4000].encode(), _LF_SOCK)
        return True
    except OSError:
        return False

def hushtype_busy():
    return os.path.exists(_LF_BUSY)

# ---------- accurate command capture: HushType's Whisper server + its cleanup rules ----------
# Vosk (small, fast, rough) is only the always-on wake-word spotter. Once it hears "Alisha",
# the audio of that utterance is re-transcribed by whisper.cpp (the server HushType runs)
# and passed through HushType's cleanup.clean() + dictionary - the same correction logic
# as "Alisha, type". If the Whisper server is down we fall back to the Vosk text.
import array, io, math, wave, uuid, urllib.request, collections
LF_DIR = os.path.expanduser("~/Documents/HushType/hushtype")
RATE, CHUNK = 16000, 1600                     # 0.1 s chunks
# names Vosk/Whisper produce for "Alisha" (incl. "journalist(s)", seen in the logs)
# Vosk's small model has no word "Alisha"; it hears sound-alikes. SPOT_RE is the loose net
# for Vosk (cheap, always on); WAKE_RE is the strict check Whisper's transcript must pass.
# Tested with synthetic voices: Vosk gave "elissa", "ellie", "ali", "elisa"; Whisper gave "Alisha".
SPOT_RE = re.compile(r"\b(?:hey\s+|okay\s+|ok\s+)?(?:alisha|alicia|alissa|alyssa|elissa|elisa|alisa|aleesha|"
                     r"alesha|elisha|eliza|ellie|ali|lisa|a lisa)\b[\s,.!?:;-]*", re.I)
WAKE_RE = re.compile(r"\b(?:hey\s+|okay\s+|ok\s+)?(?:alisha|aleesha|alesha|alicia|alissa|alyssa|elisha|"
                     r"a\s?lisha)\b[\s,.!?:;-]*", re.I)
# Vosk often mangles "Alisha" ("dad was", "hijack jack this is", "what is") but hears "type" fine.
# So "type"/"dictate" within the first 4 words also counts as "Alisha, type". Not "type of ...".
FUZZY_DICTATE_RE = re.compile(r"^\W*(?:[\w']+\W+){1,4}?(?=(?:type|types|dictate|dictation)\b(?!\W+of\b))", re.I)
CMD_PROMPT = ("Alisha, system status. Alisha, what time is it? Alisha, open terminal. Alisha, open browser. "
              "Alisha, open Burp. Alisha, open Wireshark. Alisha, lock the screen. Alisha, run matrix. "
              "Alisha, set the lights to blue. Alisha, lights rainbow. Alisha, what's the date? "
              "Alisha, help. Alisha, type. Alisha, go to sleep. Alisha, wake up. Alisha, shut down. Confirm.")

def _lf_load():
    """Use HushType's own cleanup + dictionary + whisper settings when available."""
    cfg = {}
    try:
        import tomllib
        with open(os.path.join(LF_DIR, "config.toml"), "rb") as f:
            cfg = tomllib.load(f)
    except Exception:
        pass
    clean = None
    try:
        sys.path.insert(0, LF_DIR)
        from cleanup import clean        # HushType's rule-based sentence correction
    except Exception:
        pass
    finally:
        if sys.path and sys.path[0] == LF_DIR:
            sys.path.pop(0)
    port = cfg.get("whisper", {}).get("port", 8178)
    thr = cfg.get("output", {}).get("silence_threshold", 200)
    end = cfg.get("output", {}).get("end_silence", 1.2)
    return clean, cfg.get("dictionary", {}), f"http://127.0.0.1:{port}", thr, end

LF_CLEAN, LF_DICT, WHISPER_URL, SILENCE_RMS, END_SILENCE = _lf_load()

def _rms(pcm):
    a = array.array("h", pcm[: len(pcm) // 2 * 2])
    return math.sqrt(sum(s * s for s in a) / len(a)) if a else 0

def _wav_bytes(pcm):
    b = io.BytesIO()
    with wave.open(b, "wb") as w:
        w.setnchannels(1); w.setsampwidth(2); w.setframerate(RATE); w.writeframes(pcm)
    return b.getvalue()

def whisper_text(pcm):
    """Transcribe raw 16 kHz mono PCM with the local whisper.cpp server; None if unavailable."""
    boundary = uuid.uuid4().hex
    fields = {"response_format": "json", "temperature": "0.0", "language": "en", "prompt": CMD_PROMPT}
    body = b"".join(f"--{boundary}\r\nContent-Disposition: form-data; name=\"{k}\"\r\n\r\n{v}\r\n".encode()
                    for k, v in fields.items())
    body += (f"--{boundary}\r\nContent-Disposition: form-data; name=\"file\"; filename=\"c.wav\"\r\n"
             f"Content-Type: audio/wav\r\n\r\n").encode() + _wav_bytes(pcm) + b"\r\n" + f"--{boundary}--\r\n".encode()
    req = urllib.request.Request(WHISPER_URL + "/inference", data=body,
                                 headers={"Content-Type": f"multipart/form-data; boundary={boundary}"})
    try:
        with urllib.request.urlopen(req, timeout=30) as r:
            return json.loads(r.read()).get("text", "").strip()
    except Exception as e:
        print(f"\033[33mwhisper unavailable ({e}); using vosk text\033[0m", flush=True)
        return None

def transcribe_clean(pcm):
    """Whisper + HushType cleanup. Returns (text, used_whisper)."""
    raw = whisper_text(pcm)
    if raw is None:
        return None, False
    text = LF_CLEAN(raw, LF_DICT) if LF_CLEAN else re.sub(r"\[[^\]]*\]|\([^)]*\)", " ", raw).strip()
    print(f"\033[90mwhisper: {raw!r} -> {text!r}\033[0m", flush=True)
    return text, True

def capture_until_silence(q, wait_secs=5.0, max_secs=15.0):
    """HushType's record_until_silence logic, reading Alisha's own mic stream.
    Returns PCM bytes, or b'' if nobody spoke within wait_secs."""
    frames, started, quiet, t, loud = [], False, 0.0, 0.0, 0
    while t < max_secs:
        try:
            data = q.get(timeout=1.0)
        except queue.Empty:
            break
        t += len(data) / (2 * RATE)
        rms = _rms(data)
        frames.append(data)
        loud = loud + 1 if rms >= SILENCE_RMS else 0
        if loud >= 2 or (started and rms >= SILENCE_RMS):   # 0.2 s of sound, not a click
            started, quiet = True, 0.0
        elif started:
            quiet += len(data) / (2 * RATE)
            if quiet >= END_SILENCE:
                break
        elif t >= wait_secs:
            return b""
        if not started and len(frames) > 3:
            frames = frames[-3:]                               # keep 0.3 s lead-in
    return b"".join(frames) if started else b""

def command_words(text):
    """Lower-case, punctuation-free text for command matching."""
    return re.sub(r"\s+", " ", re.sub(r"[^\w\s']", " ", text.lower())).strip()

def sh(cmd):
    env = dict(os.environ, DISPLAY=DISPLAY)
    return subprocess.run(cmd, shell=True, capture_output=True, text=True, env=env)

# ---------- text to speech ----------
PIPER_BIN = os.path.expanduser("~/alisha-voice/venv/bin/piper")
VOICE_FILE = os.path.expanduser("~/.config/alisha/voice/voice.txt")   # name of the chosen voice
VOICES = ["en_US-hfc_female-medium", "en_GB-cori-medium", "en_GB-jenny_dioco-medium", "en_US-amy-medium"]

def _piper_voice():
    """The voice named in voice.txt, else the first female voice that is installed."""
    names = list(VOICES)
    try:
        names.insert(0, open(VOICE_FILE).read().strip())
    except OSError:
        pass
    for n in names:
        p = os.path.expanduser(f"~/alisha-voice/models/{n}.onnx")
        if n and os.path.exists(p):
            return p
    return None

def say(text):
    print(f"\033[36mA.L.I.S.H.A. ▸ {text}\033[0m", flush=True)
    hud("reply", text); hud("state", "speaking")
    try:
        _speak(text)
    finally:
        hud("state", "idle")

BOARD_SINK = "alisha_esp32_speaker"     # the ESP32 board's speaker (created by alisha-hud-bridge)

def _play_wav(wav):
    """Play WAV bytes on the board's speaker if it is there, otherwise the default output."""
    sinks = subprocess.run(["pactl", "list", "short", "sinks"], capture_output=True, text=True).stdout
    dev = [f"--device={BOARD_SINK}"] if BOARD_SINK in sinks else []
    subprocess.run(["paplay", *dev], input=wav)

_voice = {"path": None, "obj": None}

def _load_voice():
    """Load the chosen Piper voice once and keep it in memory (re-loads if voice.txt changes)."""
    path = _piper_voice()
    if not path:
        return None
    if _voice["path"] != path:
        from piper import PiperVoice           # piper-tts, installed in ~/alisha-voice/venv
        _voice["obj"], _voice["path"] = PiperVoice.load(path), path
    return _voice["obj"]

def _shape(pcm, state):
    """Prepare audio for the board's tiny speaker: remove rumble below ~150 Hz that it cannot
    play (it only wastes amplifier headroom and muddies the voice) and leave a little headroom
    so loud syllables never clip. state carries the filter memory between sentences."""
    import numpy as np
    x = pcm.astype(np.float32)
    a = state["a"]
    y = np.empty_like(x)
    px, py = state["x"], state["y"]
    for i in range(len(x)):                  # one-pole high-pass filter
        py = a * (py + x[i] - px)
        px = x[i]
        y[i] = py
    state["x"], state["y"] = px, py
    return np.clip(y * 0.9, -32767, 32767).astype(np.int16)

def _speak_stream(text):
    """Speak with Piper in-process, playing each sentence as soon as it is synthesised."""
    import math
    import numpy as np
    voice = _load_voice()
    if voice is None:
        return False
    rate = voice.config.sample_rate
    sinks = subprocess.run(["pactl", "list", "short", "sinks"], capture_output=True, text=True).stdout
    dev = [f"--device={BOARD_SINK}"] if BOARD_SINK in sinks else []
    player = subprocess.Popen(["pacat", "--playback", "--raw", "--format=s16le", f"--rate={rate}",
                               "--channels=1", "--latency-msec=120", *dev], stdin=subprocess.PIPE)
    rc = 1.0 / (2 * math.pi * 150.0)
    state = {"a": rc / (rc + 1.0 / rate), "x": 0.0, "y": 0.0}
    gap = np.zeros(int(rate * 0.12), dtype=np.int16)          # short pause between sentences
    try:
        for i, chunk in enumerate(voice.synthesize(text)):
            if i:
                player.stdin.write(gap.tobytes())
            player.stdin.write(_shape(chunk.audio_int16_array, state).tobytes())
            player.stdin.flush()
    finally:
        player.stdin.close()
        player.wait()
    return True

def _speak(text):
    try:
        if _speak_stream(text):
            return
    except Exception as e:
        print(f"[speech] Piper streaming failed ({e}); using fallback", file=sys.stderr, flush=True)
    piper = PIPER_BIN if os.access(PIPER_BIN, os.X_OK) else shutil.which("piper")
    voice = _piper_voice()
    if piper and voice:
        try:
            # piper-tts 1.3 writes an empty file for "--output_file -", so use a temp file
            with tempfile.NamedTemporaryFile(suffix=".wav") as tmp:
                subprocess.run([piper, "--model", voice, "--output_file", tmp.name],
                               input=text.encode(), stdout=subprocess.DEVNULL,
                               stderr=subprocess.DEVNULL, check=True)
                wav = open(tmp.name, "rb").read()
            if len(wav) > 44:
                _play_wav(wav)
                return
        except Exception:
            pass
    if shutil.which("espeak-ng"):
        # last resort: the old synthetic voice
        wav = subprocess.run(["espeak-ng", "-v", "en-gb", "-s", "160", "-p", "35", "--stdout", text],
                             capture_output=True).stdout
        _play_wav(wav)

# ---------- actions ----------
# Privileged helpers are called by FULL PATH: the sudoers rule allows exactly these paths and
# arguments, and sudo refuses the bare name ("pironman5") even though the rule lists it.
PIRONMAN = "/usr/local/bin/pironman5"
POWEROFF = "/usr/sbin/poweroff"

def action_status():
    try:
        load = open("/proc/loadavg").read().split()[0]
        temp = sh("vcgencmd measure_temp").stdout.strip().replace("temp=", "").replace("'C", " degrees")
        mem = sh("free -m | awk '/Mem:/{printf \"%d\", $3*100/$2}'").stdout.strip()
        say(f"All systems nominal. CPU load {load}. Temperature {temp}. Memory at {mem} percent.")
    except Exception:
        say("I could not read the system status.")

def action_time():
    t = time.localtime()
    hour = t.tm_hour % 12 or 12
    say(f"It's {hour}:{t.tm_min:02d} {'AM' if t.tm_hour < 12 else 'PM'}.")

def action_date():
    say("Today is " + time.strftime("%A, %B ") + str(time.localtime().tm_mday) + ".")

APPS = [  # (spoken names, command, name to say)
    (("terminal", "console", "shell", "command line"), "qterminal", "the terminal"),
    (("browser", "firefox", "internet", "web"), "firefox-esr", "the browser"),
    (("burp",), "burpsuite", "Burp Suite"),
    (("wireshark", "wire shark"), "wireshark", "Wireshark"),
    (("bloodhound", "blood hound"), "bloodhound", "BloodHound"),
    (("files", "file manager", "folder"), "thunar", "the file manager"),
]

def action_open(cmd):
    for names, prog, spoken in APPS:
        if any(re.search(rf"\b{n}\b", cmd) for n in names):
            if shutil.which(prog):
                subprocess.Popen(prog, shell=True, env=dict(os.environ, DISPLAY=DISPLAY))
                say(f"Opening {spoken}.")
            else:
                say(f"{spoken} is not installed.")
            return
    say("Which application? Terminal, browser, Burp, Wireshark, BloodHound or files.")

COLOURS = [  # (spoken names, hex allowed by the sudoers rule, name to say)
    (("red",), "ff0000", "red"), (("green",), "00ff00", "green"), (("blue", "navy"), "0000ff", "blue"),
    (("purple", "violet", "magenta", "pink"), "aa00ff", "purple"),
    (("cyan", "teal", "aqua", "turquoise", "jarvis"), "1fb6ff", "cyan"), (("white",), "ffffff", "white"),
]

def action_lights(cmd):
    if re.search(r"\brainbow|colou?rful|disco|party\b", cmd):
        r = sh(f"sudo -n {PIRONMAN} -rs rainbow")
        say("Engaging rainbow lighting." if r.returncode == 0 else "I could not change the lights.")
        return
    for names, hexv, spoken in COLOURS:
        if any(re.search(rf"\b{n}\b", cmd) for n in names):
            r = sh(f"sudo -n {PIRONMAN} -rs solid -rc {hexv}")
            if r.returncode == 0:
                say(f"Lights set to {spoken}.")
            else:
                print(f"[lights] failed: {r.stderr.strip()}", file=sys.stderr, flush=True)
                say("I could not change the lights. The permission rule may be missing.")
            return
    say("Which colour? Red, green, blue, purple, cyan, white or rainbow.")

def action_matrix():
    subprocess.Popen("qterminal -e 'cmatrix -b -C cyan'", shell=True,
                     env=dict(os.environ, DISPLAY=DISPLAY))
    say("Entering the matrix.")

def action_lock():
    say("Locking the screen.")
    sh("xflock4 || loginctl lock-session")

def action_help():
    say("You can say: system status, what time is it, what's the date, open the terminal or browser, "
        "set the lights to a colour, run matrix, lock the screen, type, go to sleep, or shut down.")

PENDING_SHUTDOWN = {"armed": False}
def action_shutdown():
    PENDING_SHUTDOWN["armed"] = True
    say("This will power off the machine. Say confirm to proceed.")

def _any(cmd, *patterns):
    return any(re.search(p, cmd) for p in patterns)

# Intent table, checked top to bottom. Patterns run on lower-case, punctuation-free words and
# cover natural phrasings plus common Whisper near-misses ("flights" for "lights").
INTENTS = [
    ("wake",     (r"^(wake( up)?|are you (there|awake|up)|you there|i'?m back|hello|hi|hey)\b",)),
    ("sleep",    (r"\b(go(ing)? to sleep|sleep mode|stand down|stop listening|take a break|goodnight|good night)\b",)),
    ("shutdown", (r"\b(shut ?down|power (off|down)|turn off (the )?(computer|pi|system|machine))\b",)),
    ("lights",   (r"\b(lights?|lighting|leds?|lamps?|flights?|glow|rgb)\b",
                  r"\b(set|make|turn|change|switch)\b.*\b(red|green|blue|purple|violet|cyan|teal|white|rainbow)\b")),
    ("time",     (r"\btime\b", r"\bclock\b", r"\bwhat hour\b", r"\bo'?clock\b")),
    ("date",     (r"\bdate\b", r"\bwhat day\b", r"\bwhich day\b", r"\btoday\b")),
    ("status",   (r"\bstatus\b", r"\breport\b", r"\bdiagnostics?\b", r"\bhow are you\b", r"\bsystems? check\b",
                  r"\bhealth\b", r"\btemperature\b")),
    ("open",     (r"^(please )?(open|launch|start|run|show)\b(?!.*\bmatrix\b)",)),
    ("matrix",   (r"\bmatrix\b",)),
    ("lock",     (r"\block\b",)),
    ("help",     (r"\bhelp\b", r"\bwhat can you do\b", r"\bcommands\b")),
    ("thanks",   (r"\bthanks?\b", r"\bthank you\b", r"\bcheers\b")),
    ("whoami",   (r"\bwho are you\b", r"\byour name\b", r"\bintroduce yourself\b")),
    ("cancel",   (r"^(never ?mind|cancel|forget it|nothing)\b",)),
]

def match_intent(cmd):
    for name, pats in INTENTS:
        if _any(cmd, *pats):
            return name
    return None

def handle(cmd):
    """Run one command. Returns "sleep" when Alisha should stand by, otherwise True."""
    cmd = command_words(cmd)
    if PENDING_SHUTDOWN["armed"]:
        PENDING_SHUTDOWN["armed"] = False
        if re.search(r"\b(confirm|confirmed|yes|do it)\b", cmd):
            say("Powering down. Goodbye, sir."); time.sleep(2)
            r = sh(f"sudo -n {POWEROFF}")
            if r.returncode != 0:
                say("I could not power off. The permission rule may be missing.")
        else:
            say("Shutdown cancelled.")
        return True
    m = DICTATE_RE.match(cmd)
    if m:
        # no spoken reply here: the mic would record it. HushType beeps instead.
        rest = m.group(1).strip()
        if not hushtype(f"text:{rest}" if rest else "listen"):
            say("Dictation is offline. Start HushType first.")
        return True
    intent = match_intent(cmd)
    print(f"\033[90m[command] {cmd!r} -> {intent}\033[0m", flush=True)
    if intent == "sleep":
        say("Going to sleep. Say Alisha to wake me."); return "sleep"
    {
        "shutdown": action_shutdown,
        "lights":   lambda: action_lights(cmd),
        "time":     action_time,
        "date":     action_date,
        "status":   action_status,
        "open":     lambda: action_open(cmd),
        "matrix":   action_matrix,
        "lock":     action_lock,
        "help":     action_help,
        "thanks":   lambda: say("Always a pleasure, sir."),
        "whoami":   lambda: say("I am Alisha. Advanced Learning Intelligent System for Human Assistance."),
        "cancel":   lambda: say("Okay."),
        "wake":     lambda: say("I'm awake and listening, sir."),
    }.get(intent, lambda: say("I did not catch a known command. Say Alisha, help, to hear what I can do."))()
    return True

# ---------- main listen loop ----------
def main():
    try:
        import sounddevice as sd
        from vosk import Model, KaldiRecognizer
    except Exception:
        print("Missing deps. Run: bash ~/.config/alisha/voice/setup_voice.sh", file=sys.stderr)
        sys.exit(1)
    if not os.path.isdir(MODEL_DIR):
        print(f"Vosk model missing at {MODEL_DIR}. Run setup_voice.sh", file=sys.stderr)
        sys.exit(1)

    # check a microphone exists
    if not any(d["max_input_channels"] > 0 for d in sd.query_devices()):
        print("No microphone detected. Plug in a USB mic and retry.", file=sys.stderr)
        sys.exit(2)

    model = Model(MODEL_DIR)
    rec = KaldiRecognizer(model, RATE)
    q = queue.Queue()
    def cb(indata, frames, t, status):
        q.put(bytes(indata))

    def board_echo_guard():
        """True when replies play on the ESP32 board: its firmware mutes the mic while speaking,
        so speech captured while Alisha was busy is the user's and can be kept."""
        sinks = subprocess.run(["pactl", "list", "short", "sinks"], capture_output=True, text=True).stdout
        return BOARD_SINK in sinks

    def flush(drop_audio=True):
        """Reset recognition. drop_audio also discards queued audio so Alisha never hears itself."""
        while drop_audio and not q.empty():
            try:
                q.get_nowait()
            except queue.Empty:
                break
        rec.Reset()
        utt.clear()

    def listen_for(prompt_state="listening", keep_audio=False):
        """Capture one follow-up sentence (after a bare 'Alisha' or a confirm prompt).
        keep_audio: keep what the user already started saying while Alisha was busy."""
        flush(drop_audio=not (keep_audio and board_echo_guard()))
        hud("state", prompt_state)
        pcm = capture_until_silence(q, wait_secs=LISTEN_WAIT)
        if not pcm:
            return ""
        hud("state", "thinking")
        text, ok = transcribe_clean(pcm)
        return text or ""

    def run_command(text):
        """Dispatch cleaned command text. Returns 'sleep' when Alisha should stand by."""
        words = command_words(text)
        if not words:
            return True
        print(f"\033[90m[heard] {text!r}\033[0m", flush=True)
        hud("heard", text)
        d = re.match(r"^\W*(?:type|types|dictate|dictation|write|take a note)\b\W*(.*)$", text, re.I | re.S)
        if d:   # keep the original casing/punctuation for typed text
            rest = d.group(1).strip()
            if not hushtype(f"text:{rest}" if rest else "listen"):
                say("Dictation is offline. Start HushType first.")
            return True
        res = handle(words)
        if PENDING_SHUTDOWN["armed"]:            # "say confirm" needs one more sentence
            handle(command_words(listen_for()))
        return res

    utt = collections.deque(maxlen=int(15 * RATE / CHUNK))   # current utterance, max 15 s

    threading.Thread(target=_ctl_listener, daemon=True).start()
    say("Alisha online. Say Alisha, followed by a command.")
    awake = True
    was_busy = False
    flush()
    hud("state", "listening")
    with sd.RawInputStream(samplerate=RATE, blocksize=CHUNK, dtype="int16",
                           channels=1, callback=cb):
        while True:
            # button commands from the ESP32 HUD (already allowlisted)
            if not CTL_QUEUE.empty():
                while not CTL_QUEUE.empty():
                    phrase = CTL_QUEUE.get()
                    hud("heard", f"[button] {phrase}"); hud("state", "thinking")
                    handle(phrase)
                flush()
                hud("state", "listening" if awake else "asleep")
            try:
                data = q.get(timeout=0.5)
            except queue.Empty:
                continue
            # while HushType is taking dictation, don't treat the speech as commands
            if hushtype_busy():
                was_busy = True
                continue
            if was_busy:
                was_busy = False
                flush()
                continue
            utt.append(data)
            if not rec.AcceptWaveform(data):
                continue
            vosk = json.loads(rec.Result()).get("text", "")
            pcm = b"".join(utt)
            utt.clear()
            if not vosk or not (SPOT_RE.search(vosk) or FUZZY_DICTATE_RE.match(vosk)):
                if vosk:
                    print(f"\033[90m(ignored, no wake word): {vosk}\033[0m", flush=True)
                continue

            # wake word spotted: re-transcribe this utterance accurately
            print(f"\033[90mvosk: {vosk}\033[0m", flush=True)
            if not command_words(SPOT_RE.sub(" ", vosk)) and len(pcm) < 3 * 2 * RATE:
                # just "Alisha" on its own: skip Whisper (slow on tiny clips), wait for the command.
                # Saying her name is also how she wakes from sleep.
                if not awake:
                    awake = True
                    print("\033[90m(woken by name)\033[0m", flush=True)
                say(GREETING)
                cmd = listen_for()
                if cmd and run_command(WAKE_RE.sub(" ", cmd).strip()) == "sleep":
                    awake = False
                flush()
                hud("state", "listening" if awake else "asleep")
                continue
            hud("state", "thinking")
            text, used_whisper = transcribe_clean(pcm)
            if not used_whisper:
                text = vosk
            m = WAKE_RE.search(text)
            if m:
                cmd = text[m.end():].strip()
            elif used_whisper and re.search(r"\b(?:alisha|alicia|elissa)\b", vosk):
                cmd = text.strip()           # Vosk clearly heard the name; Whisper dropped it
            elif FUZZY_DICTATE_RE.match(text) or FUZZY_DICTATE_RE.match(vosk):
                f = FUZZY_DICTATE_RE.match(text)   # misheard "Alisha" before "type"
                cmd = text[f.end():].strip() if f else vosk[FUZZY_DICTATE_RE.match(vosk).end():].strip()
                print(f"\033[90m(dictation trigger, wake word misheard): {cmd!r}\033[0m", flush=True)
            else:
                print(f"\033[90m(false wake, whisper heard: {text!r})\033[0m", flush=True)
                flush()
                hud("state", "listening" if awake else "asleep")
                continue

            if not awake:
                # her name wakes her; anything said after "wake up" is run as a command
                awake = True
                print("\033[90m(woken by name)\033[0m", flush=True)
                cmd = re.sub(r"\b(please|wake( up)?|are you there|you there|i'?m back|come back|"
                             r"online|hello|hi|hey|good morning)\b", " ", command_words(cmd)).strip()
                if not cmd:
                    say(GREETING)
                    cmd = listen_for()
                    if not cmd:
                        flush()
                        hud("state", "listening")
                        continue

            if not command_words(cmd):       # just "Alisha" -> listen for the command
                say(GREETING)
                cmd = listen_for()
                if not cmd:
                    flush()
                    hud("state", "listening")
                    continue
            if run_command(cmd) == "sleep":
                awake = False
            flush()
            hud("state", "listening" if awake else "asleep")

if __name__ == "__main__":
    main()
