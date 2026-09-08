# Study Clock — ESPHome

Replaces the closed‑source `yuan910715/Esp8266_Wifi_Matrix_Clock` `.bin` on the
**second** clock with an ESPHome build.

| | |
|---|---|
| Board | NodeMCU / ESP8266 (`nodemcuv2`) |
| Matrix | **4× MAX7219 FC‑16** (32×8 px) — CLK `D5/GPIO14`, DIN `D7/GPIO13`, CS `D4/GPIO2` |
| RTC | none on this unit → **NTP‑only** |

Display: big `HH:MM` (5×8 font) left, small `SS` (tom‑thumb) bottom‑right.

## Setup

**1. Fonts** — the clock needs two bitmap fonts. They live in `esphome/fonts/`
in this repo; copy them to your ESPHome config folder (`/config/esphome/fonts/`
on the HA add‑on). Or fetch them directly:

```bash
mkdir -p /config/esphome/fonts && cd /config/esphome/fonts
wget https://raw.githubusercontent.com/olikraus/u8g2/master/tools/font/bdf/5x8.bdf
wget https://raw.githubusercontent.com/olikraus/u8g2/master/tools/font/bdf/tom-thumb.bdf
```

**2. Secrets** — only Wi‑Fi is external. In `secrets.yaml` (same folder as the
YAML):

```yaml
wifi_ssid: "..."
wifi_password: "..."
```

The API key, OTA password and setup‑AP password are inline in the YAML.

## First flash — over USB

The stock firmware isn't ESPHome, so there's no OTA into it. Plug in USB and
install from the ESPHome dashboard (*Install → Plug into this computer*) or:

```bash
esphome run esphome/secondclock.yaml
```

Erases the yuan910715 firmware; re‑flash `4.1.bin` from that repo to roll back.
After the first flash, updates are OTA.

## Home Assistant

Auto‑discovered via the ESPHome integration (native API, no MQTT):

| Entity | Purpose |
|---|---|
| `text.study_clock_message` | scroll a message; clears after *Message Duration* |
| `number.study_clock_brightness` | 0–15 |
| `number.study_clock_message_duration` | seconds a message stays up |
| `button.study_clock_show_clock` | drop the message now |
| `button.study_clock_sync_time_ntp` | force an SNTP sync |
| `button.study_clock_restart` | reboot |
| `sensor.study_clock_date` / `wifi_signal` / `uptime` | telemetry |

Same controls are on the device's web page at `http://<device-ip>/`
(`web_server` v3, assets streamed from `oi.esphome.io`, OTA/log off to save RAM).
If it gets flaky under browser load, drop to `version: 2` or remove the block.

The root‑level `smart_clock.yaml` / `lovelace_card.yaml` are MQTT config for the
**first** clock and don't apply here.

## Tuning the layout

`substitutions` block at the top of `secondclock.yaml`:

| knob | effect |
|---|---|
| `time_x` / `time_y` | position of `HH:MM` |
| `sec_x` / `sec_y` | position of `SS` (bottom‑right anchor) |

Panels reversed → add `reverse_enable: true` under `display:`; characters
mirrored → add `flip_x: true`. For a seconds **progress bar** instead of digits,
swap to the commented alternative in the `lambda:`.
