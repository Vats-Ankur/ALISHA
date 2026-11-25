#!/usr/bin/env bash
# Build the desktop preview against the firmware's own LVGL copy and render screenshots.
#   cd esp32-hud/sim && ./build.sh        -> shots/*.png and shots/sheet.png
set -euo pipefail
cd "$(dirname "$0")"
FW=../firmware
LV=$FW/managed_components/lvgl__lvgl
CF="-O1 -w -DLV_CONF_INCLUDE_SIMPLE -I. -Istubs -I$LV -I$FW/main"
mkdir -p obj
# LVGL is compiled once and cached in obj/liblvgl.a
if [ ! -f obj/liblvgl.a ]; then
    find "$LV/src" -name '*.c' | xargs -P"$(nproc)" -I{} sh -c 'o=obj/$(echo "{}" | md5sum | cut -c1-12).o; gcc '"$CF"' -c "{}" -o "$o"'
    ar rcs obj/liblvgl.a obj/*.o
fi
gcc $CF -Wall sim_main.c $FW/main/ui.c $FW/main/saver.c $FW/main/font_*.c obj/liblvgl.a -lm -o obj/hud_sim
rm -f shots/*.ppm
./obj/hud_sim
python3 - <<'PY'
from PIL import Image
import glob, os
files = sorted(glob.glob("shots/*.ppm"))
imgs = []
for f in files:
    im = Image.open(f).resize((640, 480), Image.NEAREST)
    im.save(f[:-4] + ".png"); os.remove(f); imgs.append(im)
cols = 3; rows = (len(imgs) + cols - 1) // cols
sheet = Image.new("RGB", (cols * 650, rows * 490), (40, 40, 40))
for i, im in enumerate(imgs):
    sheet.paste(im, ((i % cols) * 650 + 5, (i // cols) * 490 + 5))
sheet.save("shots/sheet.png")
print("sheet: shots/sheet.png")
PY
