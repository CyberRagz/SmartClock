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
| Home Assistant | Auto-discovery for RTC time, date, temperature & status sensors |
| OTA Updates | ArduinoOTA over Wi-Fi (`hostname: smartclock`) |
| Web Server | ESPAsyncWebServer on port 80 (extensible) |

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
- `ESPAsyncTCP` by me-no-dev
- `ESPAsyncWebServer` by me-no-dev
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
| `smart_clock/rtc/status` | `OK` / `INVALID` | Published every 60 s |
| `smart_clock/rtc/time` | `HH:MM:SS` | Published every 60 s |
| `smart_clock/rtc/date` | `DD/MMM/YY` | Published every 60 s |
| `smart_clock/rtc/temperature` | `°C` float | DS3231 on-chip sensor |

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

## License

MIT — see [LICENSE](LICENSE).
