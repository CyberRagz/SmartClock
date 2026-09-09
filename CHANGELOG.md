# Changelog

All notable changes to this project will be documented here.

## [2.0.0] — 2026-09-09

### Added — StudyClock (second unit)
- New `StudyClock/` sketch: 4× MAX7219, CS on D4, no RTC, static IP, DHT22
  temperature/humidity, self-contained MQTT discovery under a "Study Clock"
  device. `study_clock.yaml` package + `StudyClock/lovelace_card.yaml`.

### Changed — SmartClock rebuilt to StudyClock parity
- **DS3231 / RTClib / Wire / NTPClient removed.** Time is now NTP only via the
  ESP8266 core `configTime()` + `<time.h>` (`Asia/Kolkata`). Fewer libraries,
  correct calendar math, DST-ready.
- **`mqtt.setBufferSize(768)`** — the old code never set it, so HA discovery
  payloads (~380 B) could be silently dropped by PubSubClient's 256 B default.
- **Fixed dangling-pointer bug**: scrolled messages are now copied into a static
  buffer instead of handing MD_Parola `customMessage.c_str()`, which could
  realloc mid-scroll and corrupt the display.
- Settings **persisted to EEPROM**: brightness, night brightness, night-dimming
  toggle, 12/24h, message-repeat count.
- **Night dimming** 22:00–07:00 using the device's own clock.
- **Self-contained HA discovery** for the controls — Message text, Brightness /
  Night Brightness / Message Repeats numbers, Night Dimming / 12 Hour switches,
  Show Clock / Show Date / Restart buttons. `smart_clock.yaml` is now optional.
- **Diagnostic sensors**: MAC, free heap, uptime, Wi-Fi reconnect count.
- **OTA progress bar** on the panel; `SHOW_CLOCK_RUN` only redraws on change
  (fixes colon flicker); weekday added to the date scroll.
- **Static IP** `192.168.0.171` (`USE_STATIC_IP 0` for DHCP).
- **DHT22 support** compiled in behind `#define ENABLE_DHT 0` — flip to 1 +
  wire to D2 + install the Adafruit DHT libraries to enable.
- Dead code removed (`lastStateChange`, `scrollComplete`); `°C` moved to an
  ASCII-source escape.
- `smart_clock.yaml` / `lovelace_card.yaml` rewritten for the new entity set
  (weather-station scripts, random-quote automation, two-card split).

### Removed
- `smart_clock/rtc/*` topics and `smart_clock/cmd/sync_rtc` — no RTC. Time and
  date now publish to `smart_clock/time` and `smart_clock/date`.

## [1.1.0] — 2026-08-13

### Fixed
- Added missing `.gitignore` — `secrets.h` was never actually excluded from git despite the README's claim
- Wi-Fi now reconnects automatically if the connection drops after boot (previously required a power cycle)
- NTP sync no longer hammers the server / blocks the loop every iteration when the RTC is invalid; retries are now throttled (30 s backoff)
- RTC calls are now guarded behind a `rtcPresent` flag, with periodic re-init retry if the DS3231 wasn't detected at boot
- Removed the unused `ESPAsyncWebServer`/`ESPAsyncTCP` scaffold — it was never started (`server.begin()` was never called) and did nothing
- Replaced byte-by-byte `String` concatenation and repeated `String` comparisons in the MQTT callback with `strcmp`/fixed buffers to avoid heap fragmentation on long uptimes
- `WiFi.mode(WIFI_STA)` is now set explicitly before connecting
- Brightness command now ignores non-numeric payloads instead of silently applying `0`
- Home Assistant MQTT discovery payloads now include a shared `device` block so all four sensors group under one "Smart Clock" device
- OTA updates were failing/stalling because `loop()` kept doing blocking MQTT/Wi-Fi/display work during a flash write; `otaActive` now freezes everything except `ArduinoOTA.handle()` while an update is in progress, and `mqttClient.setSocketTimeout(2)` caps the worst-case MQTT blocking window generally

### Attempted and reverted (post-release)
- A flip/odometer-style seconds display was added, splitting the panel into an MD_Parola `HH:MM` zone and a seconds zone. It caused a cascade of hard-to-diagnose issues on real hardware across several iterations: a Soft WDT crash-loop (`display.print()`'s blocking wait assumes single-zone), MQTT/HA messages getting cut short (a zone-status check OR'd across zones instead of checking the relevant one), and persistent garbled/malformed characters in the seconds zone that survived multiple isolation attempts (position swap, reverting a tens/units split, swapping the flip effect for an instant one). The last one was never conclusively root-caused. Per explicit request, the entire multi-zone/seconds feature has been reverted — the display is back to the original single-zone architecture (`display.begin()`, no zones). All the fixes unrelated to the display zones (Wi-Fi reconnect, OTA safety, RTC/NTP retry logic, MQTT robustness, HA discovery device grouping) are kept; see git history for the full multi-zone iteration if revisiting this feature later.

### Fixed (post-revert)
- Diagnosed via a real device's Serial Monitor output that the DS3231 RTC was never actually being detected (`RTC not found!` printed on every boot) — meanwhile `syncNtpToRtc()` unconditionally refused to run without an RTC present (`if (!rtcPresent) return;`), so the clock had no fallback and just stayed blank even though Wi-Fi/MQTT/NTP were all working fine. The RTC is now fully optional: `syncNtpToRtc()` runs off Wi-Fi alone and tracks time via NTP independent of RTC presence (`ntpTimeValid`), the RTC is used opportunistically as a bonus source when present and valid (and kept in sync when it is), and `getTimeString()`/`getDateString()` fall back to NTP-derived time (via `DateTime` calendar math on the raw NTP epoch — no physical RTC required) whenever the RTC isn't available. `RTC Status` now reports `OK` / `NTP_ONLY` / `INVALID` instead of a plain OK/INVALID so it's clear which time source is active. If the RTC is fixed later while running, it's picked up automatically on its next successful sync, no reboot needed.

## [1.0.0] — 2025-01-03

### Added
- ESP8266 + DS3231 RTC real-time clock display on 6-panel MAX7219 LED matrix
- NTP synchronisation (IST UTC+5:30) with hourly refresh
- MQTT integration (PubSubClient) with publish / subscribe support
- Home Assistant MQTT auto-discovery for 4 sensors
- ArduinoOTA over-the-air firmware updates
- ESPAsyncWebServer (port 80) scaffold
- Non-blocking display state machine: Welcome → Brand → IP → Clock → Date
- Colon blink effect (500 ms interval)
- Periodic date scroll every 5 minutes
- `secrets.h` credential separation with `.gitignore` protection
