#!/usr/bin/env bash
# Export the ALISHA and HushType projects into clean, GitHub-ready folders.
#
#   bash ~/Documents/ALISHA-Setup/scripts/export_for_github.sh
#
# Output (re-run any time to refresh; your working files are never modified):
#   ~/Documents/GitHub/alisha      - assistant, ESP32 HUD firmware + bridge, desktop, configs
#   ~/Documents/GitHub/hushtype    - HushType, the offline Whisper dictation tool
#
# Left out on purpose: build output, downloaded models, Python venvs, whisper.cpp
# (setup.sh clones it), *.bak backups, personal voice recordings, and the original
# ESP32 flash dump (it contains a leaked OpenAI key and the Wi-Fi password).
# The script finishes with a secret scan and refuses to commit if anything is found.
set -euo pipefail

SRC_A="$HOME/Documents/ALISHA-Setup"
SRC_L="$HOME/Documents/HushType/hushtype"
OUT="$HOME/Documents/GitHub"
A="$OUT/alisha"
L="$OUT/hushtype"

# copy SRC DEST: copy one file, creating folders as needed
copy() { mkdir -p "$(dirname "$2")"; cp -p "$1" "$2"; }
# note FILE TEXT: put a comment block at the top of a config file in the export
note() { local t; t=$(mktemp); printf '%s\n' "$2" | cat - "$1" > "$t"; mv "$t" "$1"; }

# Clear old exported files but keep each repo's .git history
for d in "$A" "$L"; do
    mkdir -p "$d"
    find "$d" -mindepth 1 -maxdepth 1 ! -name .git -exec rm -rf {} +
done

# ------------------------------------------------------------------ ALISHA
EXCL=(--exclude build/ --exclude managed_components/ --exclude .venv/ --exclude __pycache__/
      --exclude 'sdkconfig' --exclude 'sdkconfig.old' --exclude 'sdkconfig.debug' --exclude '*.bak*'
      --exclude 'build-*/' --exclude 'sim/obj/' --exclude 'sim/shots/' --exclude 'sim/ui.c.*')
rsync -a "${EXCL[@]}" "$SRC_A/esp32-hud/" "$A/esp32-hud/"
copy "$SRC_A/desktop/deepdive.py"           "$A/desktop/deepdive.py"
copy "$SRC_A/scripts/make_wallpaper.py"     "$A/scripts/make_wallpaper.py"
copy "$SRC_A/scripts/make_lvgl_font.py"     "$A/scripts/make_lvgl_font.py"
copy "$SRC_A/scripts/export_for_github.sh"  "$A/scripts/export_for_github.sh"
# ALISHA-SETUP.md is a personal machine log (installed tools, host fixes) and stays private
copy "$SRC_A/docs/banner.png"               "$A/docs/banner.png"
copy "$SRC_A/docs/hud-gallery.png"          "$A/docs/hud-gallery.png"
copy "$SRC_A/docs/architecture.png"         "$A/docs/architecture.png"
copy "$SRC_A/docs/architecture.svg"         "$A/docs/architecture.svg"
copy "$SRC_A/scripts/make_architecture.py"  "$A/scripts/make_architecture.py"
copy "$SRC_A/scripts/make_readme_art.py"    "$A/scripts/make_readme_art.py"

copy "$HOME/.config/alisha/voice/alisha_voice.py"  "$A/voice/alisha_voice.py"
copy "$HOME/.config/alisha/voice/setup_voice.sh"   "$A/voice/setup_voice.sh"
copy "$HOME/.config/alisha/voice/voice.txt"        "$A/voice/voice.txt"
copy "$HOME/.config/alisha/alisha.zsh"             "$A/shell/alisha.zsh"
copy "$HOME/.config/alisha/greeting.sh"            "$A/shell/greeting.sh"
copy "$HOME/.config/conky/alisha.conf"             "$A/conky/alisha.conf"
copy "$HOME/.config/fastfetch/config.jsonc"        "$A/fastfetch/config.jsonc"

copy "$HOME/.config/systemd/user/alisha-voice.service" "$A/systemd/alisha-voice.service"
note "$A/systemd/alisha-voice.service" "# Runs the ALISHA voice assistant (voice/alisha_voice.py) in the desktop session.
# Install: copy to ~/.config/systemd/user/ then: systemctl --user enable --now alisha-voice"
copy "$HOME/.config/autostart/alisha-hud.desktop"   "$A/autostart/alisha-hud.desktop"
note "$A/autostart/alisha-hud.desktop" "# Starts the conky A.L.I.S.H.A. panel at login. Install: copy to ~/.config/autostart/"
copy "$HOME/.config/autostart/deepdive-live.desktop" "$A/autostart/deepdive-live.desktop"
note "$A/autostart/deepdive-live.desktop" "# Starts the DEEP DIVE live wallpaper at login. Install: copy to ~/.config/autostart/"
copy /usr/libexec/xfce4-screensaver/deepdive "$A/xfce-screensaver/deepdive"
copy /usr/share/applications/screensavers/xfce-deepdive.desktop "$A/xfce-screensaver/xfce-deepdive.desktop"
note "$A/xfce-screensaver/xfce-deepdive.desktop" "# Registers DEEP DIVE as an xfce4-screensaver theme.
# Install: sudo cp to /usr/share/applications/screensavers/ (launcher goes to /usr/libexec/xfce4-screensaver/)"

# Machine settings (firewall, SSH) are NOT exported: they belong to the host, not the project.
# Only the sudo rule ALISHA needs for "set the lights" and "shut down" is published.
mkdir -p "$A/system"
cat > "$A/system/sudoers-alisha.example" <<'EOF'
# /etc/sudoers.d/alisha  - lets ALISHA run exactly these root commands without a password.
# Needed for the "set the lights ..." (Pironman 5 case LEDs) and "shut down" voice commands.
# Replace "kali" with your user, then install with:  sudo visudo -f /etc/sudoers.d/alisha
# Paths and arguments must match exactly: ALISHA calls them by full path, and sudo
# refuses a bare "pironman5" even when this rule exists.
Cmnd_Alias ALISHA_LIGHTS = /usr/local/bin/pironman5 -rs rainbow, \
    /usr/local/bin/pironman5 -rs solid -rc ff0000, /usr/local/bin/pironman5 -rs solid -rc 00ff00, \
    /usr/local/bin/pironman5 -rs solid -rc 0000ff, /usr/local/bin/pironman5 -rs solid -rc aa00ff, \
    /usr/local/bin/pironman5 -rs solid -rc 1fb6ff, /usr/local/bin/pironman5 -rs solid -rc ffffff
Cmnd_Alias ALISHA_POWER = /usr/sbin/poweroff ""
kali ALL=(root) NOPASSWD: ALISHA_LIGHTS, ALISHA_POWER
EOF

cat > "$A/desktop/requirements.txt" <<'EOF'
# DEEP DIVE desktop (desktop/deepdive.py). Create the venv with:
#   python3 -m venv --system-site-packages desktop/.venv && desktop/.venv/bin/pip install -r desktop/requirements.txt
pygame>=2.6
python-xlib>=0.33
EOF

cat > "$A/.gitignore" <<'EOF'
# ESP-IDF build output and downloaded components (re-created by `idf.py build`)
esp32-hud/firmware/build/
esp32-hud/firmware/managed_components/
esp32-hud/firmware/sdkconfig
esp32-hud/firmware/sdkconfig.old
# Desktop preview build output (esp32-hud/sim/build.sh)
esp32-hud/sim/obj/
esp32-hud/sim/shots/
# Python
__pycache__/
.venv/
venv/
# Models and firmware dumps (large, downloaded, or may contain secrets)
*.onnx
*.onnx.json
vosk-model*/
*.bin
esp32-backup/
# Local backups
*.bak*
EOF

# README, license and notice are maintained in the project root (README.md, LICENSE, NOTICE)
copy "$SRC_A/README.md"                     "$A/README.md"
copy "$SRC_A/LICENSE"                       "$A/LICENSE"
copy "$SRC_A/NOTICE"                        "$A/NOTICE"

# ------------------------------------------------------------------ HushType
rsync -a --exclude whisper.cpp/ --exclude .venv/ --exclude __pycache__/ --exclude samples/ \
      --exclude '*.bak*' --exclude localflow.py --exclude localflow "$SRC_L/" "$L/"
copy "$HOME/.config/systemd/user/hushtype.service" "$L/systemd/hushtype.service"
note "$L/systemd/hushtype.service" "# Runs HushType in the desktop session (hotkey + \"Alisha, type\" voice trigger).
# Install: copy to ~/.config/systemd/user/ then: systemctl --user enable --now hushtype"
cat > "$L/.gitignore" <<'EOF'
# Cloned and built by setup.sh
whisper.cpp/
# Python
.venv/
__pycache__/
# Personal voice recordings used for testing
samples/
*.wav
# Local backups
*.bak*
EOF

# ------------------------------------------------------------------ secret scan
echo "==> scanning for secrets"
PAT='sk-[A-Za-z0-9_-]{20,}|api[_-]?key *[:=] *["'"'"'][^"'"'"']{8,}|BEGIN [A-Z ]*PRIVATE KEY|ghp_[A-Za-z0-9]{30,}|xox[bp]-|wifi_?pass|psk='
if grep -rInEi --exclude-dir=.git --exclude=export_for_github.sh "$PAT" "$A" "$L"; then
    echo "!! possible secret found above - fix it before committing" >&2
    exit 1
fi
echo "    clean"

# ------------------------------------------------------------------ local git commit
for d in "$A" "$L"; do
    cd "$d"
    [ -d .git ] || git init -q -b master
    git add -A
    if git diff --cached --quiet; then
        echo "==> $(basename "$d"): no changes"
    else
        git -c user.name="Ankur Vats" -c user.email="8893950+Vats-Ankur@users.noreply.github.com" \
            commit -q -m "${EXPORT_MSG:-Update from the Pi ($(date +%F))}"   # author only: no co-author lines (owner's rule)
        echo "==> $(basename "$d"): committed $(git rev-parse --short HEAD)"
    fi
done
echo "Done. Publish with:  git -C $A push   and   git -C $L push"
