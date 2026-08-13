# SmartClock

An ESP8266-based smart clock with a 6-panel MAX7219 LED matrix display, DS3231 RTC, NTP time synchronisation, MQTT integration, and Home Assistant auto-discovery.

---

## Features

| Feature | Details |
|---|---|
| LED Matrix | 6× MAX7219 panels driven by MD_Parola / MD_MAX72xx |
| Real-Time Clock | DS3231 via I²C — maintains time across power cycles |
| NTP Sync | Syncs to `pool.ntp.org` (IST UTC+5:30) every hour |
| MQTT | PubSubClient — publishes telemetry, subscribes to commands |
| Home Assistant | Auto-discovery for RTC time, date, temperature & status sensors, grouped under one device |
| OTA Updates | ArduinoOTA over Wi-Fi (`hostname: smartclock`) |
| Wi-Fi Resilience | Automatic reconnect if the connection drops after boot |

---

## Hardware

| Component | Connection |
|---|---|
| NodeMCU / ESP8266 | — |
| 6× MAX7219 LED panels | CLK → D5 (GPIO14), DATA → D7 (GPIO13), CS → D8 (GPIO15) |
| DS3231 RTC module | SDA → D2 (GPIO4), SCL → D1 (GPIO5) |

---

## Getting Started

### 1 — Prerequisites

Install the following libraries via Arduino Library Manager or PlatformIO:

- `ESP8266WiFi` (bundled with ESP8266 core)
- `NTPClient` by Fabrice Weinberg
- `MD_Parola` by MajicDesigns
- `MD_MAX72xx` by MajicDesigns
- `ArduinoJson` by Benoît Blanchon (v6)
- `PubSubClient` by Nick O'Leary
- `RTClib` by Adafruit
- `ArduinoOTA` (bundled with ESP8266 core)

### 2 — Configure credentials

```bash
cp secrets_example.h secrets.h
```

Edit `secrets.h` and fill in your Wi-Fi SSID/password, MQTT broker address, credentials, and OTA password. **`secrets.h` is git-ignored and will never be committed.**

### 3 — Flash

Open `SmartClock.ino` in Arduino IDE, select your ESP8266 board, and upload.

---

## MQTT Topics

### Telemetry (published by the clock)

| Topic | Payload | Notes |
|---|---|---|
| `smart_clock/status` | `online` / `offline` | LWT |
| `smart_clock/rtc/status` | `OK` / `NTP_ONLY` / `INVALID` | Published every 60 s — see below |
| `smart_clock/rtc/time` | `HH:MM:SS` | Published every 60 s whenever any valid time source is available |
| `smart_clock/rtc/date` | `DD/MMM/YY` | Published every 60 s whenever any valid time source is available |
| `smart_clock/rtc/temperature` | `°C` float | DS3231 on-chip sensor — only published if the RTC chip is physically detected |

### Commands (subscribe from HA / MQTT client)

| Topic | Payload | Action |
|---|---|---|
| `smart_clock/cmd/message` | Any string | Scroll custom message on display |
| `smart_clock/cmd/preset` | Any string | Same as message |
| `smart_clock/cmd/brightness` | `0`–`15` | Set display intensity |
| `smart_clock/cmd/sync_rtc` | any | Force NTP → RTC sync |
| `smart_clock/cmd/reset` | any | Return to clock display |

---

## Home Assistant

The clock publishes MQTT discovery payloads automatically on connection. Four sensors will appear in HA:

- **RTC Status** — `mdi:clock-check`
- **RTC Time** — `mdi:clock-digital`
- **RTC Date** — `mdi:calendar`
- **RTC Temperature** — device class `temperature`, unit `°C`

---

## Time Source: RTC vs NTP-Only Fallback

The DS3231 RTC is optional, not required. The clock always prefers it when available (it keeps
ticking correctly across brief Wi-Fi drops and doesn't depend on the network), but if the RTC chip
isn't detected on the I²C bus — not wired up, a dead backup battery, a faulty module — the clock
falls back to using NTP time directly instead of just showing dashes. NTP is resynced from
`pool.ntp.org` hourly once a valid time source exists (30 s retries until then), same as before.

`RTC Status` now reports three states instead of two:

| Status | Meaning |
|---|---|
| `OK` | RTC chip present and holding valid time — the normal case |
| `NTP_ONLY` | No usable RTC (not found, or present but never synced), clock is running on NTP alone |
| `INVALID` | No valid time source at all yet (no RTC, and NTP hasn't synced — usually just after boot, or no network) |

If the RTC is later fixed (reconnected, battery replaced) while the clock is running, it's detected
automatically in the background and takes back over as the time source on its next successful sync
— no reboot needed.

---

## Display State Machine

```
SHOW_WELCOME → SHOW_BRAND → SHOW_IP → SHOW_CLOCK_ENTRY
                                              ↕
                                       SHOW_CLOCK_RUN
                                              ↕  (every 5 min)
                                         SHOW_DATE
                                              ↕
                                      SHOW_MESSAGE (on MQTT cmd)
```

---

## OTA Updates

Once running, the clock is reachable as `smartclock.local` on the network. Use Arduino IDE → Ports to find it and upload wirelessly.

---
# Home Assistant Integration

This folder contains all Home Assistant configuration files for the SmartClock.

## Files

| File | Purpose |
|---|---|
| `smart_clock.yaml` | HA Package — helpers (input_text, input_number, input_select) + all scripts |
| `lovelace_card.yaml` | Lovelace dashboard card — paste directly into a manual card |

---

## Setup

### 1 — Install the Package

Copy `smart_clock.yaml` into your HA packages folder, then reference it in `configuration.yaml`:

```yaml
homeassistant:
  packages: !include_dir_named packages/
```

Restart Home Assistant.

### 2 — Add the Dashboard Card

1. Open your dashboard → **Edit** → **Add Card** → **Manual**
2. Paste the full contents of `lovelace_card.yaml`
3. Save

---

## Helpers Created

| Entity | Type | Purpose |
|---|---|---|
| `input_text.smart_clock_message` | Text | Custom message to scroll on the display |
| `input_number.smart_clock_display_duration` | Number (5–60 s) | How long to show a message |
| `input_select.smart_clock_preset_messages` | Select | Quick-pick preset messages |

---

## Scripts

| Script | Action |
|---|---|
| `script.smart_clock_show_message` | Scrolls the custom message on the display |
| `script.smart_clock_show_preset` | Scrolls the selected preset |
| `script.smart_clock_show_indoor_temp` | Shows indoor temperature from `sensor.bar_controller_bar_temperature` |
| `script.smart_clock_show_outdoor_temp` | Shows outdoor temperature from `sensor.mini_weather_station_outdoor_temperature` |
| `script.smart_clock_show_humidity` | Shows humidity from `sensor.oht_waterlevel_oht_humidity` |
| `script.smart_clock_show_weather` | Shows weather summary from `sensor.mini_weather_station_weather_summary` |
| `script.smart_clock_return_to_clock` | Resets display back to clock mode |
| `script.smart_clock_set_brightness` | Sets brightness (0–15) via `brightness` field |
| `script.smart_clock_sync_rtc` | Forces NTP → RTC sync on the device |

> **Note:** Update the sensor entity IDs in `smart_clock.yaml` to match your own sensor names.

---

## MQTT Auto-Discovery Sensors

The clock firmware publishes HA auto-discovery payloads on boot. These sensors will appear automatically:

| Entity | Description |
|---|---|
| `sensor.smart_clock_rtc_status` | `OK` or `INVALID` |
| `sensor.smart_clock_rtc_time` | Current time from DS3231 |
| `sensor.smart_clock_rtc_date` | Current date from DS3231 |
| `sensor.smart_clock_rtc_temperature` | DS3231 on-chip temperature (°C) |

---

## Dashboard Card Layout

```
┌─────────────────────────────────────┐
│  Smart Clock — Message              │
│  [Custom Message input            ] │
│  [Display Duration slider         ] │
│  [Preset Messages dropdown        ] │
├─────────────────────────────────────┤
│  RTC Status                         │
│  RTC Status | Time | Date | Temp    │
│  [Sync RTC with NTP]                │
├──────────────────┬──────────────────┤
│ [Show Custom Msg]│ [Indoor Temp]    │
│ [Show Preset   ] │ [Outdoor Temp]   │
├──────────────────┴──────────────────┤
│ [Humidity]       │ [Weather]        │
├─────────────────────────────────────┤
│  Clock Actions                      │
│  [Return to Clock]                  │
├─────────────────────────────────────┤
│  Brightness Control                 │
│  [Low]    [Medium]    [High]        │
└─────────────────────────────────────┘
```

## License

MIT — see [LICENSE](LICENSE).
