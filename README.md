# SmartClock

ESP8266 + MAX7219 LED‑matrix clock with NTP time, MQTT, and Home Assistant
auto‑discovery. Two units in this repo, same firmware architecture:

| | [`SmartClock/`](SmartClock/) | [`StudyClock/`](StudyClock/) |
|---|---|---|
| Panels | 6× MAX7219 (48×8) | 4× MAX7219 (32×8) |
| CS pin | D8 (GPIO15) | D4 (GPIO2) |
| Static IP | `192.168.0.171` | `192.168.0.170` |
| MQTT namespace | `smart_clock/…` | `study_clock/…` |
| HA device | "Smart Clock" | "Study Clock" |
| Climate sensor | DHT22 support present, compiled out (`ENABLE_DHT 0`) | DHT22 on D2 |

CLK → D5 (GPIO14), DIN → D7 (GPIO13) on both. Time is **NTP only** — no RTC.

---

## Firmware

Arduino IDE, ESP8266 core. Board *NodeMCU 1.0 (ESP‑12E Module)*.

**Libraries** (Library Manager): `MD_Parola`, `MD_MAX72xx`, `ArduinoJson` (v6),
`PubSubClient`. StudyClock also needs `DHT sensor library` (Adafruit) +
`Adafruit Unified Sensor`.

```bash
cp SmartClock/secrets_example.h SmartClock/secrets.h    # SmartClock
cp StudyClock/secrets_example.h StudyClock/secrets.h    # StudyClock
# then edit Wi-Fi + MQTT + OTA password in each
```

`secrets.h` is git‑ignored. First flash over USB; after that OTA works
(hostname `smartclock` / `studyclock`, password from `secrets.h`). If the build
errors on `secrets.h` with "extended character", replace any non‑ASCII box
characters in its comments with `--`.

### Features

- **NTP** via `configTime()` (`Asia/Kolkata`, no DST). Retries every 30 s until
  synced, then hourly. Shows `--:--` until first sync.
- **Display** (single‑zone MD_Parola): `HH:MM` with blinking colon, weekday+date
  auto‑scrolls every 5 min, messages scroll on demand (1–5 repeats).
- **Persisted to EEPROM**: brightness, night brightness, night‑dimming toggle,
  12/24‑hour, message‑repeat count.
- **Night dimming** 22:00–07:00 (edit `NIGHT_START`/`NIGHT_END`) off the
  device's own clock — works with HA offline.
- **OTA** with a progress bar on the panel; `otaActive` freezes the loop during
  a flash write.
- **Wi‑Fi** auto‑reconnect; static IP (`USE_STATIC_IP 0` for DHCP).
- **Self‑contained HA discovery** — no YAML package required for the controls.

---

## Home Assistant

Auto‑discovered under one device. Entities (`smart_clock_*` / `study_clock_*`):

| Group | Entities |
|---|---|
| Sensors | Time, Date, Display state |
| Diagnostic | Wi‑Fi Signal, IP, MAC, Free Heap, Uptime, Wi‑Fi Reconnects |
| Controls | Message (text), Brightness |
| Config | Night Brightness, Message Repeats, Night Dimming, 12 Hour Format |
| Buttons | Show Clock, Show Date, Restart *(+ Show Climate when DHT enabled)* |

### MQTT topics

Telemetry is retained. Commands:

| Topic (`<ns>` = `smart_clock` / `study_clock`) | Payload | Action |
|---|---|---|
| `<ns>/cmd/message` | string | scroll it (empty → back to clock) |
| `<ns>/cmd/brightness` / `night_brightness` | `0`–`15` | day / night intensity |
| `<ns>/cmd/night_dimming` / `format_12h` | `ON` / `OFF` | toggles |
| `<ns>/cmd/message_repeats` | `1`–`5` | scroll count per message |
| `<ns>/cmd/show_date` / `show_climate` / `reset` / `restart` | any | one‑shot actions |

### Optional packages

[`smart_clock.yaml`](smart_clock.yaml) / [`study_clock.yaml`](study_clock.yaml)
add convenience scripts (push weather‑station / sensor values to the display,
presets) and a "random quote every 5 min" automation. Drop in `packages/`,
reference it in `configuration.yaml`, **restart HA**.

[`lovelace_card.yaml`](lovelace_card.yaml) /
[`StudyClock/lovelace_card.yaml`](StudyClock/lovelace_card.yaml) — Card 1 (main,
no package needed) + a commented Card 2 for the package's quick‑message buttons.

---

## Adding a DHT22 to SmartClock

Set `#define ENABLE_DHT 1`, wire the sensor to `DHT_PIN` (D2/GPIO4, with a 10k
pull‑up), install the two Adafruit libraries, reflash. Temperature/Humidity
sensors and a Show Climate button then appear in HA automatically.

---

## License

MIT — see [LICENSE](LICENSE).
