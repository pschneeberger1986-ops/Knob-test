# ☕ LM Knob — standalone La Marzocco controller

A complete ESP32-S3 firmware for the **Guition JC3636K718C** round knob display
that controls a La Marzocco espresso machine directly via **Bluetooth** —
no Home Assistant, no cloud subscription required after first pairing.

![Flash page](https://img.shields.io/badge/flash-via%20browser-gold)
![License](https://img.shields.io/badge/license-MIT-blue)

---

## Features

- ☕ Live coffee boiler temperature (current + target)
- 💨 Steam boiler status & level control
- 🔛 Power on / standby toggle (short press)
- 🎛️ Settings overlay via long press + knob rotation
- 💧 Water tank empty warning
- 🔵 BLE connection indicator
- 📶 Optional Wi-Fi (for OTA updates only)
- ⚡ One-click browser flashing via [ESP Web Tools](https://esphome.github.io/esp-web-tools/)

---

## Compatible hardware

| Component | Model |
|-----------|-------|
| Board | Guition JC3636K718C (ESP32-S3, 360×360 round display) |
| Machine | La Marzocco Linea Micra / Linea Mini / GS3 |

---

## Flash (no software required)

1. Open **[YOUR_GITHUB_PAGES_URL]** in Chrome or Edge (89+)
2. Connect the Guition board via USB
3. Click **Flash firmware** and select the serial port
4. Done — the board reboots automatically

---

## First-time pairing

The knob communicates with the machine via BLE using a token that is
read **directly from the machine** — no La Marzocco account needed.

1. On first boot the display shows the setup screen
2. Put your La Marzocco in **pairing mode**
   (hold the connectivity button until the LED blinks blue)
3. **Press the knob button** — it scans, connects, and reads the BLE token
4. Token + MAC address are stored in flash; the machine pairs permanently

> The pairing token is a shared secret stored on the machine itself.
> The Guition board reads it via the `GET_TOKEN` BLE characteristic
> (UUID `0c0b7847-e12b-09a8-b04b-8e0922a9abab`) — the same approach
> used by the official La Marzocco Home app.

---

## Controls

| Input | Action |
|-------|--------|
| Short press | Toggle power (BrewingMode ↔ StandBy) |
| Long press | Open / close settings overlay |
| Rotate (in settings) | Adjust coffee temp (±0.5°C) or steam level |
| Press (in settings) | Confirm value, cycle to next setting |

---

## Wi-Fi (optional)

Wi-Fi is only used for OTA firmware updates — the machine control is
100% Bluetooth local. On first boot, the device creates an open AP called
**LM-Knob-Setup**. Connect to it from your phone and open `192.168.4.1`
to enter your Wi-Fi credentials. You can skip this step entirely.

---

## Build from source

```bash
# Install ESP-IDF v5.2 (https://docs.espressif.com/projects/esp-idf)
idf.py set-target esp32s3
idf.py build
idf.py flash monitor
```

---

## BLE protocol reference

All protocol details were reverse-engineered and cross-checked against
[pylamarzocco](https://github.com/zweckj/pylamarzocco) and
[kaspizzo/lamarzocco](https://github.com/kaspizzo/lamarzocco).

| Characteristic | UUID | Direction |
|----------------|------|-----------|
| READ | `0a0b7847-e12b-09a8-b04b-8e0922a9abab` | Machine → device |
| WRITE | `0b0b7847-e12b-09a8-b04b-8e0922a9abab` | Device → machine |
| GET_TOKEN | `0c0b7847-e12b-09a8-b04b-8e0922a9abab` | Read in pairing mode |
| AUTH | `0d0b7847-e12b-09a8-b04b-8e0922a9abab` | Write token to authenticate |

Commands are JSON objects written to WRITE, null-terminated.

---

## Disclaimer

This project is not affiliated with or endorsed by La Marzocco Srl.
Use at your own risk.

---

## License

MIT
