# Changelog

All notable changes to this project will be documented here.

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
