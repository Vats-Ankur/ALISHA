# A.L.I.S.H.A. ESP32 HUD

*A.L.I.S.H.A. = **Advanced Learning Intelligent System for Human Assistance***

The LCKFB "Shizhanpai" ESP32-S3 board becomes a **USB microphone and touch HUD** for the Pi's offline Alisha voice assistant.

```
[ESP32 mic + touch screen] --USB serial (CRC frames)--> [alisha_hud_bridge.py] --> PipeWire mic "ALISHA-ESP32-Mic" --> Alisha
                           <-- stats, ops feed, Alisha state --
```

## Build and flash the firmware
```bash
. ~/esp/esp-idf/export.sh
cd ~/Documents/ALISHA-Setup/esp32-hud/firmware
idf.py build
idf.py -p /dev/ttyUSB0 flash
```

## Run the bridge
```bash
python3 host/alisha_hud_bridge.py              # virtual mic + HUD
python3 host/alisha_hud_bridge.py --levels     # speak and watch which channels move
python3 host/alisha_hud_bridge.py --mic 1      # pick the ES7210 channel to use (-1 = mix)
```
As a service:
```bash
ln -sf ~/Documents/ALISHA-Setup/esp32-hud/host/alisha-hud-bridge.service ~/.config/systemd/user/
systemctl --user daemon-reload
systemctl --user enable --now alisha-hud-bridge
```

## Controls on the device
- **Swipe** between pages: CORE, SYS, NET, OPS, ACT
- **BOOT button** or the **MIC** tile: mute/unmute (enforced on the device)
- "Aura Clash" battle screensaver after 3 minutes idle (or the AURA CLASH tile); touch to dismiss

## Restore the original firmware
```bash
esptool --port /dev/ttyUSB0 write-flash 0 ../esp32-backup/szp-original-16MB.bin
```

The protocol is documented in `firmware/main/proto.h`. The security design is in `SECURITY.md`.
