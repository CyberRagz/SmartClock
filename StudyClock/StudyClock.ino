/* =====================================================================
   STUDY CLOCK  -  ESP8266 + 4x MAX7219 (FC-16)  -  MQTT / Home Assistant
   =====================================================================
   Tailored from SmartClock/SmartClock.ino for the second unit:
     - 4 LED panels (not 6),  CS on D4
     - NO DS3231 / no I2C  ->  time comes straight from NTP (configTime)
     - single-zone MD_Parola display: HH:MM with a blinking colon,
       scrolling messages, periodic date scroll
     - MQTT auto-discovery: sensors + a Message text box, Brightness
       slider and Show Clock / Show Date buttons, all under one HA device

   Upload from the Arduino IDE (ESP8266 core). Libraries (Library Manager):
     MD_Parola, MD_MAX72xx, ArduinoJson (v6), PubSubClient
   Board: "NodeMCU 1.0 (ESP-12E Module)"  (or your ESP8266 board)
   ===================================================================== */

#include <ESP8266WiFi.h>
#include <SPI.h>
#include <MD_Parola.h>
#include <MD_MAX72xx.h>
#include <ArduinoJson.h>
#include <PubSubClient.h>
#include <ArduinoOTA.h>
#include <time.h>
#include "secrets.h"

/* ---------------- HARDWARE ---------------- */
#define HARDWARE_TYPE MD_MAX72XX::FC16_HW
#define MAX_DEVICES   4
#define CLK_PIN       14   // D5
#define DATA_PIN      13   // D7
#define CS_PIN        2    // D4

/* ---------------- STATIC IP ---------------- */
// Set USE_STATIC_IP to 0 to fall back to DHCP.
#define USE_STATIC_IP 1
static const IPAddress STATIC_IP (192, 168, 0, 170);
static const IPAddress GATEWAY   (192, 168, 0, 1);
static const IPAddress SUBNET    (255, 255, 255, 0);
static const IPAddress DNS_1     (192, 168, 0, 1);
static const IPAddress DNS_2     (8, 8, 8, 8);

/* ---------------- TIME ---------------- */
#define TZ_INFO         "IST-5:30"          // India Standard Time, no DST
#define NTP_SERVER_1    "pool.ntp.org"
#define NTP_SERVER_2    "time.google.com"
#define NTP_SERVER_3    "time.cloudflare.com"
#define MIN_VALID_EPOCH 1700000000UL        // ~2023-11, sanity floor

/* ---------------- IDENTITY / MQTT ---------------- */
static const char* MQTT_CLIENT_ID = "study_clock";
#define DISCOVERY_PREFIX "homeassistant"

#define T_AVAIL       "study_clock/status"
#define T_TIME        "study_clock/time"
#define T_DATE        "study_clock/date"
#define T_IP          "study_clock/ip"
#define T_RSSI        "study_clock/rssi"
#define T_DISP        "study_clock/display"        // CLOCK / MESSAGE / DATE / BOOT
#define T_BRIGHT_ST   "study_clock/brightness"
#define T_MSG_ST      "study_clock/message"

#define T_CMD_MESSAGE    "study_clock/cmd/message"
#define T_CMD_BRIGHTNESS "study_clock/cmd/brightness"
#define T_CMD_RESET      "study_clock/cmd/reset"       // -> back to clock
#define T_CMD_SHOW_DATE  "study_clock/cmd/show_date"

/* ---------------- INTERVALS ---------------- */
#define WIFI_TIMEOUT            20000UL
#define WIFI_RECONNECT_INTERVAL 30000UL
#define MQTT_RETRY_INTERVAL     5000UL
#define NTP_RESYNC_INTERVAL     3600000UL   // 1 h once we have time
#define NTP_RETRY_INTERVAL      30000UL     // while we don't
#define TELEMETRY_FAST_INTERVAL 10000UL     // time / date / display state
#define TELEMETRY_SLOW_INTERVAL 60000UL     // rssi / ip
#define DATE_SCROLL_INTERVAL    300000UL    // auto-scroll the date every 5 min

#define SCROLL_SPEED 45                     // ms per column
#define DEFAULT_BRIGHTNESS 5

/* ---------------- OBJECTS ---------------- */
MD_Parola  display = MD_Parola(HARDWARE_TYPE, DATA_PIN, CLK_PIN, CS_PIN, MAX_DEVICES);
WiFiClient       espClient;
PubSubClient     mqtt(espClient);

/* ---------------- STATE ---------------- */
enum DisplayState { SHOW_BOOT, SHOW_IP, SHOW_MESSAGE, SHOW_CLOCK_ENTRY, SHOW_CLOCK_RUN, SHOW_DATE };
DisplayState state = SHOW_BOOT;

String   customMessage;
int      brightness      = DEFAULT_BRIGHTNESS;
bool     colonOn         = false;
volatile bool otaActive  = false;

bool     pendingMessage  = false;
bool     pendingDate     = false;

unsigned long lastBlink       = 0;
unsigned long lastNtpAttempt  = 0 - NTP_RESYNC_INTERVAL;
unsigned long lastDateScroll  = 0;
char     clockShown[8]        = "";

/* ---------------- FORWARD DECLS ---------------- */
bool haveValidTime();
void getClockText(char* buf, size_t len);   // HH<blink>MM for the panel
void getTimeText(char* buf, size_t len);    // HH:MM:SS for MQTT
void getDateText(char* buf, size_t len);    // DD/MMM/YY
void startScroll(const char* text, uint16_t pause);
bool scrollDone();
void showStatic(const char* text);
void mqttReconnect();
void publishDiscovery();
void publishTelemetry(bool force);
const char* displayName();

/* =====================================================================
   WIFI
   ===================================================================== */
void startWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.hostname("studyclock");
#if USE_STATIC_IP
  if (!WiFi.config(STATIC_IP, GATEWAY, SUBNET, DNS_1, DNS_2)) {
    Serial.println("WiFi.config() rejected - check the addresses");
  }
#endif
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
}

bool connectWiFi() {
  showStatic("wifi");
  startWiFi();

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_TIMEOUT) {
    delay(300);
    Serial.print('.');
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\nWiFi OK  %s\n", WiFi.localIP().toString().c_str());
    return true;
  }
  Serial.println("\nWiFi FAILED");
  showStatic("no wifi");
  delay(2000);
  return false;
}

void ensureWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;
  static unsigned long last = 0;
  if (millis() - last < WIFI_RECONNECT_INTERVAL) return;
  last = millis();
  Serial.println("WiFi reconnect...");
  WiFi.disconnect();
  startWiFi();
}

/* =====================================================================
   TIME  (NTP only - no RTC on this unit)
   ===================================================================== */
bool haveValidTime() {
  return time(nullptr) > (time_t)MIN_VALID_EPOCH;
}

void tryNtpSync() {
  if (WiFi.status() != WL_CONNECTED) return;
  // configTime() kicks off SNTP; calling it again just refreshes the servers.
  configTime(TZ_INFO, NTP_SERVER_1, NTP_SERVER_2, NTP_SERVER_3);
}

void getClockText(char* buf, size_t len) {
  if (!haveValidTime()) { strncpy(buf, "--:--", len); return; }
  time_t now = time(nullptr);
  struct tm t;
  localtime_r(&now, &t);

  if (millis() - lastBlink >= 500) { colonOn = !colonOn; lastBlink = millis(); }
  snprintf(buf, len, "%02d%c%02d", t.tm_hour, colonOn ? ':' : ' ', t.tm_min);
}

void getTimeText(char* buf, size_t len) {
  if (!haveValidTime()) { strncpy(buf, "--:--:--", len); return; }
  time_t now = time(nullptr);
  struct tm t;
  localtime_r(&now, &t);
  snprintf(buf, len, "%02d:%02d:%02d", t.tm_hour, t.tm_min, t.tm_sec);
}

void getDateText(char* buf, size_t len) {
  static const char* M[] = { "JAN","FEB","MAR","APR","MAY","JUN",
                             "JUL","AUG","SEP","OCT","NOV","DEC" };
  if (!haveValidTime()) { strncpy(buf, "--/---/--", len); return; }
  time_t now = time(nullptr);
  struct tm t;
  localtime_r(&now, &t);
  snprintf(buf, len, "%02d/%s/%02d", t.tm_mday, M[t.tm_mon], (t.tm_year + 1900) % 100);
}

/* =====================================================================
   DISPLAY  (single zone - HH:MM print + scroll effects)
   ===================================================================== */
void startScroll(const char* text, uint16_t pause) {
  display.displayClear();
  display.displayText(text, PA_CENTER, SCROLL_SPEED, pause, PA_SCROLL_LEFT, PA_SCROLL_LEFT);
}

bool scrollDone() {
  if (display.displayAnimate()) { display.displayClear(); return true; }
  return false;
}

void showStatic(const char* text) {
  display.displayClear();
  display.displayText(text, PA_CENTER, 0, 0, PA_PRINT, PA_NO_EFFECT);
  display.displayAnimate();
}

/* =====================================================================
   MQTT
   ===================================================================== */
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  if (length > 100) length = 100;
  char msg[101];
  memcpy(msg, payload, length);
  msg[length] = '\0';

  if (!strcmp(topic, T_CMD_MESSAGE)) {
    customMessage = msg;
    mqtt.publish(T_MSG_ST, msg, true);
    if (customMessage.length() > 0) pendingMessage = true;
    else                            state = SHOW_CLOCK_ENTRY;
  }
  else if (!strcmp(topic, T_CMD_BRIGHTNESS)) {
    char* end;
    long v = strtol(msg, &end, 10);
    if (end != msg) {
      brightness = constrain((int)v, 0, 15);
      display.setIntensity(brightness);
      char b[8]; snprintf(b, sizeof(b), "%d", brightness);
      mqtt.publish(T_BRIGHT_ST, b, true);
    }
  }
  else if (!strcmp(topic, T_CMD_RESET)) {
    state = SHOW_CLOCK_ENTRY;
  }
  else if (!strcmp(topic, T_CMD_SHOW_DATE)) {
    pendingDate = true;
  }
}

void mqttReconnect() {
  if (mqtt.connected()) return;
  static unsigned long last = 0;
  if (millis() - last < MQTT_RETRY_INTERVAL) return;
  last = millis();

  Serial.print("MQTT... ");
  if (mqtt.connect(MQTT_CLIENT_ID, MQTT_USER, MQTT_PASS,
                   T_AVAIL, 0, true, "offline")) {
    Serial.println("connected");
    mqtt.publish(T_AVAIL, "online", true);
    publishDiscovery();

    mqtt.subscribe(T_CMD_MESSAGE);
    mqtt.subscribe(T_CMD_BRIGHTNESS);
    mqtt.subscribe(T_CMD_RESET);
    mqtt.subscribe(T_CMD_SHOW_DATE);

    char b[8]; snprintf(b, sizeof(b), "%d", brightness);
    mqtt.publish(T_BRIGHT_ST, b, true);
    mqtt.publish(T_MSG_ST, customMessage.c_str(), true);
    publishTelemetry(true);
  } else {
    Serial.printf("failed rc=%d\n", mqtt.state());
  }
}

// One helper for every discovery entity. `extra` is a lambda that fills the
// component-specific keys.
template <typename F>
void discovery(const char* component, const char* objectId, const char* name, F extra) {
  StaticJsonDocument<512> doc;
  doc["name"] = name;

  char uid[48];
  snprintf(uid, sizeof(uid), "study_clock_%s", objectId);
  doc["unique_id"] = uid;
  doc["availability_topic"] = T_AVAIL;

  JsonObject dev = doc.createNestedObject("device");
  dev["identifiers"][0] = "study_clock";
  dev["name"]           = "Study Clock";
  dev["manufacturer"]   = "DIY";
  dev["model"]          = "ESP8266 + 4x MAX7219";

  extra(doc);

  char topic[96];
  snprintf(topic, sizeof(topic), "%s/%s/study_clock/%s/config",
           DISCOVERY_PREFIX, component, objectId);

  char payload[512];
  size_t n = serializeJson(doc, payload, sizeof(payload));
  mqtt.publish(topic, (const uint8_t*)payload, n, true);
}

void publishDiscovery() {
  discovery("sensor", "time", "Time", [](JsonDocument& d){
    d["state_topic"] = T_TIME; d["icon"] = "mdi:clock-digital"; });

  discovery("sensor", "date", "Date", [](JsonDocument& d){
    d["state_topic"] = T_DATE; d["icon"] = "mdi:calendar"; });

  discovery("sensor", "display", "Display", [](JsonDocument& d){
    d["state_topic"] = T_DISP; d["icon"] = "mdi:television-guide"; });

  discovery("sensor", "rssi", "Wi-Fi Signal", [](JsonDocument& d){
    d["state_topic"] = T_RSSI; d["device_class"] = "signal_strength";
    d["unit_of_measurement"] = "dBm"; d["entity_category"] = "diagnostic"; });

  discovery("sensor", "ip", "IP Address", [](JsonDocument& d){
    d["state_topic"] = T_IP; d["icon"] = "mdi:ip-network";
    d["entity_category"] = "diagnostic"; });

  discovery("text", "message", "Message", [](JsonDocument& d){
    d["command_topic"] = T_CMD_MESSAGE; d["state_topic"] = T_MSG_ST;
    d["icon"] = "mdi:message-text"; d["max"] = 100; });

  discovery("number", "brightness", "Brightness", [](JsonDocument& d){
    d["command_topic"] = T_CMD_BRIGHTNESS; d["state_topic"] = T_BRIGHT_ST;
    d["icon"] = "mdi:brightness-6";
    d["min"] = 0; d["max"] = 15; d["step"] = 1; d["mode"] = "slider"; });

  discovery("button", "show_clock", "Show Clock", [](JsonDocument& d){
    d["command_topic"] = T_CMD_RESET; d["payload_press"] = "1";
    d["icon"] = "mdi:clock"; });

  discovery("button", "show_date", "Show Date", [](JsonDocument& d){
    d["command_topic"] = T_CMD_SHOW_DATE; d["payload_press"] = "1";
    d["icon"] = "mdi:calendar"; });

  Serial.println("HA discovery published");
}

const char* displayName() {
  switch (state) {
    case SHOW_MESSAGE:                     return "MESSAGE";
    case SHOW_DATE:                        return "DATE";
    case SHOW_CLOCK_RUN:
    case SHOW_CLOCK_ENTRY:                 return "CLOCK";
    default:                               return "BOOT";
  }
}

void publishTelemetry(bool force) {
  if (!mqtt.connected()) return;
  static unsigned long lastFast = 0, lastSlow = 0;
  unsigned long now = millis();
  char buf[16];

  if (force || now - lastFast >= TELEMETRY_FAST_INTERVAL) {
    lastFast = now;
    if (haveValidTime()) {
      getTimeText(buf, sizeof(buf));  mqtt.publish(T_TIME, buf, true);
      getDateText(buf, sizeof(buf));  mqtt.publish(T_DATE, buf, true);
    }
    mqtt.publish(T_DISP, displayName(), true);
  }
  if (force || now - lastSlow >= TELEMETRY_SLOW_INTERVAL) {
    lastSlow = now;
    snprintf(buf, sizeof(buf), "%d", WiFi.RSSI());
    mqtt.publish(T_RSSI, buf, true);
    mqtt.publish(T_IP, WiFi.localIP().toString().c_str(), true);
  }
}

/* =====================================================================
   OTA
   ===================================================================== */
void setupOTA() {
  ArduinoOTA.setHostname("studyclock");
  ArduinoOTA.setPassword(OTA_PASSWORD);
  ArduinoOTA.onStart([]() { otaActive = true;  showStatic("ota"); });
  ArduinoOTA.onEnd  ([]() { otaActive = false; showStatic("done"); });
  ArduinoOTA.onError([](ota_error_t) { otaActive = false; showStatic("err"); });
  ArduinoOTA.begin();
}

/* =====================================================================
   SETUP
   ===================================================================== */
void setup() {
  Serial.begin(115200);
  Serial.println("\n\nStudy Clock starting...");

  display.begin();
  display.setIntensity(brightness);
  display.displayClear();

  connectWiFi();
  setupOTA();

  mqtt.setServer(MQTT_SERVER, MQTT_PORT);
  mqtt.setCallback(mqttCallback);
  mqtt.setSocketTimeout(2);
  mqtt.setBufferSize(768);            // HA discovery payloads exceed the 256 default

  tryNtpSync();

  startScroll("STUDY CLOCK", 1200);
  state = SHOW_BOOT;
  Serial.println("Setup done");
}

/* =====================================================================
   LOOP
   ===================================================================== */
void loop() {
  ArduinoOTA.handle();
  if (otaActive) return;

  ensureWiFi();
  mqttReconnect();
  mqtt.loop();
  publishTelemetry(false);

  // NTP: retry fast until valid, then hourly
  unsigned long ntpInterval = haveValidTime() ? NTP_RESYNC_INTERVAL : NTP_RETRY_INTERVAL;
  if (millis() - lastNtpAttempt >= ntpInterval) {
    lastNtpAttempt = millis();
    tryNtpSync();
  }

  // Command-driven transitions (kept out of the callback so display calls
  // don't run re-entrantly inside mqtt.loop())
  // NOTE: MD_Parola keeps the text pointer for the whole animation, so any
  // buffer handed to startScroll() must outlive the scroll -> use statics.
  static char scrollBuf[110];

  if (pendingMessage) {
    pendingMessage = false;
    strncpy(scrollBuf, customMessage.c_str(), sizeof(scrollBuf) - 1);
    scrollBuf[sizeof(scrollBuf) - 1] = '\0';
    startScroll(scrollBuf, 1500);
    state = SHOW_MESSAGE;
  } else if (pendingDate && state == SHOW_CLOCK_RUN) {
    pendingDate = false;
    getDateText(scrollBuf, sizeof(scrollBuf));
    startScroll(scrollBuf, 1500);
    state = SHOW_DATE;
  }

  switch (state) {

    case SHOW_BOOT:
      if (scrollDone()) {
        if (WiFi.status() == WL_CONNECTED) {
          static char ip[36];
          snprintf(ip, sizeof(ip), "IP %s", WiFi.localIP().toString().c_str());
          startScroll(ip, 2000);
          state = SHOW_IP;
        } else {
          state = SHOW_CLOCK_ENTRY;
        }
      }
      break;

    case SHOW_IP:
      if (scrollDone()) state = SHOW_CLOCK_ENTRY;
      break;

    case SHOW_MESSAGE:
      if (scrollDone()) state = SHOW_CLOCK_ENTRY;
      break;

    case SHOW_CLOCK_ENTRY:
      display.displayClear();
      clockShown[0] = '\0';
      lastDateScroll = millis();
      state = SHOW_CLOCK_RUN;
      break;

    case SHOW_CLOCK_RUN: {
      char now[8];
      getClockText(now, sizeof(now));
      if (strcmp(now, clockShown) != 0) {
        strcpy(clockShown, now);
        display.displayText(clockShown, PA_CENTER, 0, 0, PA_PRINT, PA_NO_EFFECT);
      }
      display.displayAnimate();

      if (millis() - lastDateScroll >= DATE_SCROLL_INTERVAL) {
        lastDateScroll = millis();
        pendingDate = true;
      }
      break;
    }

    case SHOW_DATE:
      if (scrollDone()) state = SHOW_CLOCK_ENTRY;
      break;
  }
}
