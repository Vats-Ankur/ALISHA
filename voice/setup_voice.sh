#!/bin/bash
# One-time setup for the A.L.I.S.H.A. offline voice assistant.
# Installs: espeak-ng (TTS), portaudio (mic I/O), a Python venv with Vosk + sounddevice,
# and the small offline English speech model (~40 MB download).
set -e
echo "[*] A.L.I.S.H.A. voice setup"

VDIR="$HOME/alisha-voice"
MODELS="$VDIR/models"
mkdir -p "$MODELS"

echo "[*] Installing system packages (needs sudo)…"
sudo apt-get update -q
sudo apt-get install -y --no-install-recommends \
    espeak-ng libportaudio2 portaudio19-dev python3-venv python3-dev \
    alsa-utils unzip wget

echo "[*] Creating Python venv…"
python3 -m venv "$VDIR/venv"
"$VDIR/venv/bin/pip" install -q --upgrade pip
"$VDIR/venv/bin/pip" install -q vosk sounddevice

if [ ! -d "$MODELS/vosk-model" ]; then
  echo "[*] Downloading offline speech model (~40 MB)…"
  wget -q --show-progress -O "$MODELS/vosk.zip" \
    https://alphacephei.com/vosk/models/vosk-model-small-en-us-0.15.zip
  unzip -q "$MODELS/vosk.zip" -d "$MODELS"
  mv "$MODELS"/vosk-model-small-en-us-0.15 "$MODELS/vosk-model"
  rm -f "$MODELS/vosk.zip"
fi

echo
echo "[✓] Voice assistant ready."
echo "    A USB microphone must be plugged in (none is detected right now)."
echo "    Start it with:  $VDIR/venv/bin/python ~/.config/alisha/voice/alisha_voice.py"
echo "    Or enable the background service:  systemctl --user enable --now alisha-voice"
