# Study Clock

`SmartClock` tailored for the **second** unit. Arduino sketch, uploaded from the
Arduino IDE.

## Differences from `SmartClock/`

| | SmartClock | **StudyClock** |
|---|---|---|
| Panels | 6× MAX7219 | **4× MAX7219** |
| CS pin | D8 (GPIO15) | **D4 (GPIO2)** |
| RTC | DS3231 (optional) | **none — NTP only, no I²C** |
| Time source | RTClib + NTPClient | **ESP8266 `configTime()` / `<time.h>`** |
| MQTT topics | `smart_clock/…` | **`study_clock/…`** |
| HA device | "Smart Clock" | **"Study Clock"** (separate device) |
| HA controls | needs `smart_clock.yaml` package | **self-contained** — Message / Brightness / buttons auto-discovered |

CLK→D5 (GPIO14) and DIN→D7 (GPIO13) are unchanged.

## Display

Single-zone MD_Parola: **`HH:MM`** (12/24h switchable) with a blinking colon, the
date (`WED 08/SEP/26`) auto-scrolls every 5 minutes, and messages scroll on
demand — 1–5 repeats, set from HA. No numeric seconds — the multi-zone seconds
display was tried on this hardware and reverted (see the root `CHANGELOG.md`,
"Attempted and reverted"); the colon blink is the seconds tick.

## Persisted settings (EEPROM)

Brightness, night brightness, night-dimming on/off, 12/24h and message-repeat
count survive reboots. **Night dimming** drops to *Night Brightness* between
22:00–07:00 (edit `NIGHT_START` / `NIGHT_END` in the sketch), using the clock's
own NTP time so it works even if HA is down.

## Build

Arduino IDE, ESP8266 core. Board: *NodeMCU 1.0 (ESP-12E Module)*.

Libraries (Library Manager): **MD_Parola**, **MD_MAX72xx**, **ArduinoJson v6**,
**PubSubClient**.

```
cp StudyClock/secrets_example.h StudyClock/secrets.h   # then edit
```

Open `StudyClock/StudyClock.ino`, select the board + port, Upload. First upload
is over USB; after that OTA works (hostname `studyclock`, password from
`secrets.h`).

## MQTT topics

Telemetry (retained):

| Topic | Payload |
|---|---|
| `study_clock/status` | `online` / `offline` (LWT) |
| `study_clock/time` | `HH:MM:SS` (always 24h) |
| `study_clock/date` | `WED DD/MMM/YY` |
| `study_clock/display` | `CLOCK` / `MESSAGE` / `DATE` / `BOOT` |
| `study_clock/rssi` | Wi-Fi RSSI, dBm |
| `study_clock/ip` / `mac` | network identity |
| `study_clock/heap` / `uptime` / `wifi_reconnects` | diagnostics |
| `study_clock/brightness` / `night_brightness` | current `0`–`15` |
| `study_clock/night_dimming` / `format_12h` | `ON` / `OFF` |
| `study_clock/message_repeats` | `1`–`5` |
| `study_clock/message` | current message |

Commands:

| Topic | Payload | Action |
|---|---|---|
| `study_clock/cmd/message` | any string | scroll it (empty string → back to clock) |
| `study_clock/cmd/brightness` | `0`–`15` | day intensity (persisted) |
| `study_clock/cmd/night_brightness` | `0`–`15` | night intensity (persisted) |
| `study_clock/cmd/night_dimming` | `ON` / `OFF` | enable the 22:00–07:00 dim |
| `study_clock/cmd/format_12h` | `ON` / `OFF` | 12- vs 24-hour face |
| `study_clock/cmd/message_repeats` | `1`–`5` | how many times a message scrolls |
| `study_clock/cmd/reset` | any | return to clock |
| `study_clock/cmd/show_date` | any | scroll the date now |
| `study_clock/cmd/restart` | any | reboot the ESP |

## Home Assistant

Auto-discovered under one **Study Clock** device:

- **Sensors:** Time, Date, Display; *diagnostic:* Wi-Fi Signal, IP, MAC, Free Heap,
  Uptime, Wi-Fi Reconnects
- **Controls:** Message (text), Brightness (slider); *config:* Night Brightness,
  Message Repeats, Night Dimming (switch), 12 Hour Format (switch)
- **Buttons:** Show Clock, Show Date, Restart

No YAML packages needed. `study_clock.yaml` in the repo root adds optional
convenience scripts (push a sensor value to the display, random-quote automation).
