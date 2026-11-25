#!/bin/bash
# A.L.I.S.H.A. terminal greeting. Shows a time-aware line + fastfetch system HUD.
CY='\033[38;5;39m'; DIM='\033[38;5;24m'; B='\033[1m'; R='\033[0m'

h=$(date +%H)
if   [ "$h" -lt 12 ]; then part="morning"
elif [ "$h" -lt 18 ]; then part="afternoon"
else part="evening"; fi

user=$(whoami)
[ "$user" = "root" ] && addr="sir (root)" || addr="sir"

echo -e "${CY}${B}  Good ${part}, ${addr}.${R}"
echo -e "${DIM}  All systems are operational. Standing by.${R}\n"

if command -v fastfetch >/dev/null 2>&1; then
    fastfetch
elif command -v neofetch >/dev/null 2>&1; then
    neofetch
fi
