# ALISHA HUD — security design

## Threat model
| Threat | Mitigation |
|---|---|
| Someone steals or plugs into the ESP32 | No secrets on the device: no API keys, no Wi-Fi credentials. Logs never go to the serial port. |
| Remote attacker on the LAN/internet | The device has no Wi-Fi/Bluetooth code at all; its only interface is the USB cable to the Pi. The bridge opens no network ports. |
| Malformed or hostile serial data | Every frame is CRC-16 checked, the length is capped, and each message type must have an exact size. Text is reduced to printable ASCII on both ends. |
| Hot mic / eavesdropping | Mute lives in the firmware (BOOT button or touch). Muted audio is never sent. Audio only flows while the Pi is actively talking to the device. The screen always shows mic state. |
| HUD used to run commands | Touch buttons send small numeric IDs. The bridge maps them to 4 fixed phrases, and Alisha accepts only those exact phrases. No shell strings cross the link. |
| Other local users | Unix sockets live in $XDG_RUNTIME_DIR with mode 0600. The bridge runs unprivileged with NoNewPrivileges. |

## Build hardening (firmware)
- `-Wall -Wextra -Werror -Wformat=2 -Wshadow -Wvla` on our code
- Strong stack protector, hardware stack guard and FreeRTOS stack canaries
- Wi-Fi and Bluetooth not linked; console disabled; bootloader logs disabled
- Dependency versions pinned in `main/idf_component.yml`, with exact hashes in `dependencies.lock` after the first build

## Production lock-down (PERMANENT — do only when the firmware is final)
These burn eFuses and cannot be undone. A lost signing key = a board that can never be updated.
1. Generate a Secure Boot v2 signing key **offline** and keep encrypted backups:
   `espsecure.py generate_signing_key --version 2 --scheme rsa3072 secure_boot_signing_key.pem`
2. `idf.py menuconfig` → Security features:
   - Enable hardware Secure Boot in bootloader (v2)
   - Enable flash encryption on boot → **Release** mode
   - Enable NVS encryption
   - UART ROM download mode → **Secure download mode** (or permanently disabled)
   - Disable JTAG (done automatically in release mode)
3. Build, then flash once with the bootloader. From then on, only signed images will boot.
4. Anti-rollback: enable `CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK` and bump `secure_version` for each security fix.

## Original firmware backup
`../esp32-backup/szp-original-16MB.bin` (mode 0600). It **contains the old OpenAI API key and Wi-Fi password in plain text**. Revoke that key, and delete the backup once you're sure you never want the old firmware back.
