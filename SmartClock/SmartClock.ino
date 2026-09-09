/* =====================================================================
   SMART CLOCK  -  ESP8266 + 6x MAX7219 (FC-16)  -  MQTT / Home Assistant
   =====================================================================
   v2 - rebuilt to StudyClock level:
     - DS3231 removed; time is NTP only (ESP8266 configTime / <time.h>)
     - static IP, self-contained MQTT auto-discovery ("Smart Clock" device)
     - EEPROM-persisted brightness / night brightness / night dimming /
       12-24h / message repeat count
     - night dimming, diagnostics, Restart button, OTA progress bar
     - DHT22 support is present but compiled out (ENABLE_DHT 0) - flip it on
       when a sensor is wired to DHT_PIN and the Adafruit DHT library is added

   Single-zone MD_Parola: HH:MM with a blinking colon, the date auto-scrolls
   every 5 min, messages scroll on demand.

   Arduino IDE, ESP8266 core. Board: "NodeMCU 1.0 (ESP-12E Module)".
   Libraries: MD_Parola, MD_MAX72xx, ArduinoJson (v6), PubSubClient
              (+ "DHT sensor library" + "Adafruit Unified Sensor" if ENABLE_DHT)
   ===================================================================== */

#include <ESP8266WiFi.h>
#include <SPI.h>
#include <MD_Parola.h>
#include <MD_MAX72xx.h>
#include <ArduinoJson.h>
#include <PubSubClient.h>
#include <ArduinoOTA.h>
#include <EEPROM.h>
#include <time.h>
#include "secrets.h"

/* ---------------- HARDWARE ---------------- */
#define HARDWARE_TYPE MD_MAX72XX::FC16_HW
#define MAX_DEVICES   6
#define CLK_PIN       14   // D5
#define DATA_PIN      13   // D7
#define CS_PIN        15   // D8
#define PANEL_COLS    (MAX_DEVICES * 8)

/* ---------------- OPTIONAL DHT22 ---------------- */
// Not fitted. Set to 1 once a DHT22 is wired to DHT_PIN and the Adafruit
// "DHT sensor library" (+ "Adafruit Unified Sensor") is installed. When 0,
// none of the temperature/humidity entities or code are compiled in.
#define ENABLE_DHT 0
#define DHT_PIN    4          // D2  (free; GPIO4)
#define DHT_TYPE   DHT22
#define DHT_READ_INTERVAL 30000UL
#define DHT_STALE_AFTER   180000UL
#if ENABLE_DHT
  #include <DHT.h>
  DHT dht(DHT_PIN, DHT_TYPE);
#endif

/* ---------------- STATIC IP ---------------- */
// Set USE_STATIC_IP to 0 to fall back to DHCP.
#define USE_STATIC_IP 1
static const IPAddress STATIC_IP (192, 168, 0, 171);
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

/* ---------------- NIGHT DIMMING ---------------- */
#define NIGHT_START 22    // dim from 22:00 ...
#define NIGHT_END   7     // ... until 07:00

/* ---------------- DEFAULTS / PREFS ---------------- */
#define PREF_MAGIC               0x5D    // bump to force a defaults reset
#define DEFAULT_BRIGHTNESS       5
#define DEFAULT_NIGHT_BRIGHTNESS 1
#define DEFAULT_MSG_REPEATS      1

#define GREETING "Ragshome"              // scrolled once at boot

/* ---------------- IDENTITY / MQTT ---------------- */
static const char* MQTT_CLIENT_ID = "smart_clock";
#define DISCOVERY_PREFIX "homeassistant"

#define T_AVAIL        "smart_clock/status"
#define T_TIME         "smart_clock/time"
#define T_DATE         "smart_clock/date"
#define T_IP           "smart_clock/ip"
#define T_MAC          "smart_clock/mac"
#define T_RSSI         "smart_clock/rssi"
#define T_HEAP         "smart_clock/heap"
#define T_UPTIME       "smart_clock/uptime"
#define T_RECONNECTS   "smart_clock/wifi_reconnects"
#define T_DISP         "smart_clock/display"        // CLOCK / MESSAGE / DATE / BOOT
#define T_BRIGHT_ST    "smart_clock/brightness"
#define T_NIGHT_BR_ST  "smart_clock/night_brightness"
#define T_NIGHT_DIM_ST "smart_clock/night_dimming"
#define T_12H_ST       "smart_clock/format_12h"
#define T_REPEATS_ST   "smart_clock/message_repeats"
#define T_MSG_ST       "smart_clock/message"
#define T_TEMP         "smart_clock/temperature"
#define T_HUM          "smart_clock/humidity"

#define T_CMD_MESSAGE      "smart_clock/cmd/message"
#define T_CMD_BRIGHTNESS   "smart_clock/cmd/brightness"
#define T_CMD_NIGHT_BR     "smart_clock/cmd/night_brightness"
#define T_CMD_NIGHT_DIM    "smart_clock/cmd/night_dimming"
#define T_CMD_12H          "smart_clock/cmd/format_12h"
#define T_CMD_REPEATS      "smart_clock/cmd/message_repeats"
#define T_CMD_RESET        "smart_clock/cmd/reset"       // -> back to clock
#define T_CMD_SHOW_DATE    "smart_clock/cmd/show_date"
#define T_CMD_SHOW_CLIMATE "smart_clock/cmd/show_climate"
#define T_CMD_RESTART      "smart_clock/cmd/restart"

/* ---------------- INTERVALS ---------------- */
#define WIFI_TIMEOUT            20000UL
#define WIFI_RECONNECT_INTERVAL 30000UL
#define MQTT_RETRY_INTERVAL     5000UL
#define NTP_RESYNC_INTERVAL     3600000UL   // 1 h once we have time
#define NTP_RETRY_INTERVAL      30000UL     // while we don't
#define TELEMETRY_FAST_INTERVAL 10000UL     // time / date / display state
#define TELEMETRY_SLOW_INTERVAL 60000UL     // rssi / ip / diagnostics
#define BRIGHTNESS_CHECK_INTERVAL 5000UL
#define DATE_SCROLL_INTERVAL    300000UL    // auto-scroll the date every 5 min

#define SCROLL_SPEED 45                     // ms per column

/* ---------------- OBJECTS ---------------- */
MD_Parola    display = MD_Parola(HARDWARE_TYPE, DATA_PIN, CLK_PIN, CS_PIN, MAX_DEVICES);
WiFiClient   espClient;
PubSubClient mqtt(espClient);

/* ---------------- STATE ---------------- */
enum DisplayState { SHOW_BOOT, SHOW_IP, SHOW_MESSAGE, SHOW_CLOCK_ENTRY, SHOW_CLOCK_RUN, SHOW_DATE };
DisplayState state = SHOW_BOOT;

String   customMessage;
char     scrollBuf[110]       = "";
int      msgRepeatsLeft       = 0;

// persisted settings
int      brightness           = DEFAULT_BRIGHTNESS;
int      nightBrightness      = DEFAULT_NIGHT_BRIGHTNESS;
bool     nightDimming         = true;
bool     use12h               = false;
int      msgRepeats           = DEFAULT_MSG_REPEATS;

bool     colonOn              = false;
volatile bool otaActive       = false;
bool     pendingMessage       = false;
bool     pendingDate          = false;
bool     pendingClimate       = false;
bool     pendingRestart       = false;
unsigned long wifiReconnects  = 0;

// DHT22 (unused unless ENABLE_DHT)
float    dhtTemp              = NAN;
float    dhtHum               = NAN;
bool     dhtValid             = false;
unsigned long lastDhtRead     = 0 - DHT_READ_INTERVAL;
unsigned long lastDhtOk       = 0;

unsigned long lastBlink       = 0;
unsigned long lastNtpAttempt  = 0 - NTP_RESYNC_INTERVAL;
unsigned long lastDateScroll  = 0;
char     clockShown[8]        = "";

/* ---------------- FORWARD DECLS ---------------- */
void startWiFi();
bool haveValidTime();
void getClockText(char* buf, size_t len);
void getTimeText(char* buf, size_t len);
void getDateText(char* buf, size_t len);
void getClimateText(char* buf, size_t len);
void readDht();
void startScroll(const char* text, uint16_t pause);
bool scrollDone();
void showStatic(const char* text);
int  effectiveBrightness();
void applyBrightness(bool force);
void loadPrefs();
void savePrefs();
void mqttReconnect();
void publishDiscovery();
void publishStates();
void publishTelemetry(bool force);
const char* displayName();

/* =====================================================================
   PREFS (EEPROM)
   ===================================================================== */
void loadPrefs() {
  EEPROM.begin(16);
  if (EEPROM.read(0) == PREF_MAGIC) {
    brightness      = constrain((int)EEPROM.read(1), 0, 15);
    nightBrightness = constrain((int)EEPROM.read(2), 0, 15);
    uint8_t f       = EEPROM.read(3);
    nightDimming    = f & 0x01;
    use12h          = f & 0x02;
    msgRepeats      = constrain((int)EEPROM.read(4), 1, 5);
  } else {
    savePrefs();
  }
}

void savePrefs() {
  EEPROM.write(0, PREF_MAGIC);
  EEPROM.write(1, (uint8_t)brightness);
  EEPROM.write(2, (uint8_t)nightBrightness);
  EEPROM.write(3, (uint8_t)((nightDimming ? 0x01 : 0) | (use12h ? 0x02 : 0)));
  EEPROM.write(4, (uint8_t)msgRepeats);
  EEPROM.commit();
}

/* =====================================================================
   WIFI
   ===================================================================== */
void startWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.hostname("smartclock");
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
    Serial.printf("\nWiFi OK  ip=%s  mac=%s\n",
                  WiFi.localIP().toString().c_str(), WiFi.macAddress().c_str());
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
  wifiReconnects++;
  Serial.println("WiFi reconnect...");
  WiFi.disconnect();
  startWiFi();
}

/* =====================================================================
   TIME  (NTP only)
   ===================================================================== */
bool haveValidTime() {
  return time(nullptr) > (time_t)MIN_VALID_EPOCH;
}

void tryNtpSync() {
  if (WiFi.status() != WL_CONNECTED) return;
  configTime(TZ_INFO, NTP_SERVER_1, NTP_SERVER_2, NTP_SERVER_3);
}

void getClockText(char* buf, size_t len) {
  if (!haveValidTime()) { strncpy(buf, "--:--", len); return; }
  time_t now = time(nullptr);
  struct tm t;
  localtime_r(&now, &t);

  if (millis() - lastBlink >= 500) { colonOn = !colonOn; lastBlink = millis(); }

  int h = t.tm_hour;
  if (use12h) { h %= 12; if (h == 0) h = 12; }
  snprintf(buf, len, use12h ? "%2d%c%02d" : "%02d%c%02d",
           h, colonOn ? ':' : ' ', t.tm_min);
}

void getTimeText(char* buf, size_t len) {   // MQTT: always 24h, unambiguous
  if (!haveValidTime()) { strncpy(buf, "--:--:--", len); return; }
  time_t now = time(nullptr);
  struct tm t;
  localtime_r(&now, &t);
  snprintf(buf, len, "%02d:%02d:%02d", t.tm_hour, t.tm_min, t.tm_sec);
}

void getDateText(char* buf, size_t len) {
  static const char* WD[] = { "SUN","MON","TUE","WED","THU","FRI","SAT" };
  static const char* M[]  = { "JAN","FEB","MAR","APR","MAY","JUN",
                              "JUL","AUG","SEP","OCT","NOV","DEC" };
  if (!haveValidTime()) { strncpy(buf, "--/---/--", len); return; }
  time_t now = time(nullptr);
  struct tm t;
  localtime_r(&now, &t);
  snprintf(buf, len, "%s %02d/%s/%02d",
           WD[t.tm_wday], t.tm_mday, M[t.tm_mon], (t.tm_year + 1900) % 100);
}

/* =====================================================================
   DHT22  (compiled out unless ENABLE_DHT)
   ===================================================================== */
void readDht() {
#if ENABLE_DHT
  if (millis() - lastDhtRead < DHT_READ_INTERVAL) return;
  lastDhtRead = millis();

  float h = dht.readHumidity();
  float t = dht.readTemperature();          // Celsius

  if (!isnan(h) && !isnan(t) && t > -40 && t < 85) {
    dhtHum = h;
    dhtTemp = t;
    dhtValid = true;
    lastDhtOk = millis();
  } else if (millis() - lastDhtOk > DHT_STALE_AFTER) {
    dhtValid = false;
  }
#endif
}

void getClimateText(char* buf, size_t len) {
  if (!dhtValid) { strncpy(buf, "CLIMATE N/A", len); return; }
  snprintf(buf, len, "%.1fC  %.0f%%", dhtTemp, dhtHum);
}

/* =====================================================================
   BRIGHTNESS  (with night dimming)
   ===================================================================== */
int effectiveBrightness() {
  if (nightDimming && haveValidTime()) {
    time_t now = time(nullptr);
    struct tm t;
    localtime_r(&now, &t);
    int h = t.tm_hour;
    bool night = (NIGHT_START > NIGHT_END)
                 ? (h >= NIGHT_START || h < NIGHT_END)
                 : (h >= NIGHT_START && h < NIGHT_END);
    if (night) return nightBrightness;
  }
  return brightness;
}

void applyBrightness(bool force) {
  static int last = -1;
  int b = effectiveBrightness();
  if (force || b != last) { last = b; display.setIntensity(b); }
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
    char* end; long v = strtol(msg, &end, 10);
    if (end != msg) {
      int nv = constrain((int)v, 0, 15);
      if (nv != brightness) { brightness = nv; savePrefs(); }
      applyBrightness(true);
      publishStates();
    }
  }
  else if (!strcmp(topic, T_CMD_NIGHT_BR)) {
    char* end; long v = strtol(msg, &end, 10);
    if (end != msg) {
      int nv = constrain((int)v, 0, 15);
      if (nv != nightBrightness) { nightBrightness = nv; savePrefs(); }
      applyBrightness(true);
      publishStates();
    }
  }
  else if (!strcmp(topic, T_CMD_NIGHT_DIM)) {
    bool nv = !strcmp(msg, "ON");
    if (nv != nightDimming) { nightDimming = nv; savePrefs(); }
    applyBrightness(true);
    publishStates();
  }
  else if (!strcmp(topic, T_CMD_12H)) {
    bool nv = !strcmp(msg, "ON");
    if (nv != use12h) { use12h = nv; savePrefs(); }
    clockShown[0] = '\0';
    publishStates();
  }
  else if (!strcmp(topic, T_CMD_REPEATS)) {
    char* end; long v = strtol(msg, &end, 10);
    if (end != msg) {
      int nv = constrain((int)v, 1, 5);
      if (nv != msgRepeats) { msgRepeats = nv; savePrefs(); }
      publishStates();
    }
  }
  else if (!strcmp(topic, T_CMD_RESET)) {
    state = SHOW_CLOCK_ENTRY;
  }
  else if (!strcmp(topic, T_CMD_SHOW_DATE)) {
    pendingDate = true;
  }
#if ENABLE_DHT
  else if (!strcmp(topic, T_CMD_SHOW_CLIMATE)) {
    pendingClimate = true;
  }
#endif
  else if (!strcmp(topic, T_CMD_RESTART)) {
    pendingRestart = true;
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
    mqtt.subscribe(T_CMD_NIGHT_BR);
    mqtt.subscribe(T_CMD_NIGHT_DIM);
    mqtt.subscribe(T_CMD_12H);
    mqtt.subscribe(T_CMD_REPEATS);
    mqtt.subscribe(T_CMD_RESET);
    mqtt.subscribe(T_CMD_SHOW_DATE);
#if ENABLE_DHT
    mqtt.subscribe(T_CMD_SHOW_CLIMATE);
#endif
    mqtt.subscribe(T_CMD_RESTART);

    mqtt.publish(T_MAC, WiFi.macAddress().c_str(), true);
    publishStates();
    publishTelemetry(true);
  } else {
    Serial.printf("failed rc=%d\n", mqtt.state());
  }
}

// One helper for every discovery entity. `extra` fills the component-specific keys.
template <typename F>
void discovery(const char* component, const char* objectId, const char* name, F extra) {
  StaticJsonDocument<640> doc;
  doc["name"] = name;

  char uid[48];
  snprintf(uid, sizeof(uid), "smart_clock_%s", objectId);
  doc["unique_id"] = uid;
  doc["availability_topic"] = T_AVAIL;

  JsonObject dev = doc.createNestedObject("device");
  dev["identifiers"][0] = "smart_clock";
  dev["name"]           = "Smart Clock";
  dev["manufacturer"]   = "DIY";
  dev["model"]          = "ESP8266 + 6x MAX7219";

  extra(doc);

  char topic[96];
  snprintf(topic, sizeof(topic), "%s/%s/smart_clock/%s/config",
           DISCOVERY_PREFIX, component, objectId);

  char payload[640];
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

#if ENABLE_DHT
  discovery("sensor", "temperature", "Temperature", [](JsonDocument& d){
    d["state_topic"] = T_TEMP; d["device_class"] = "temperature";
    d["unit_of_measurement"] = "\xC2\xB0" "C";   // UTF-8 degree sign, ASCII source
    d["state_class"] = "measurement"; });

  discovery("sensor", "humidity", "Humidity", [](JsonDocument& d){
    d["state_topic"] = T_HUM; d["device_class"] = "humidity";
    d["unit_of_measurement"] = "%"; d["state_class"] = "measurement"; });
#endif

  discovery("sensor", "rssi", "Wi-Fi Signal", [](JsonDocument& d){
    d["state_topic"] = T_RSSI; d["device_class"] = "signal_strength";
    d["unit_of_measurement"] = "dBm"; d["entity_category"] = "diagnostic";
    d["state_class"] = "measurement"; });

  discovery("sensor", "ip", "IP Address", [](JsonDocument& d){
    d["state_topic"] = T_IP; d["icon"] = "mdi:ip-network";
    d["entity_category"] = "diagnostic"; });

  discovery("sensor", "mac", "MAC Address", [](JsonDocument& d){
    d["state_topic"] = T_MAC; d["icon"] = "mdi:network";
    d["entity_category"] = "diagnostic"; });

  discovery("sensor", "heap", "Free Heap", [](JsonDocument& d){
    d["state_topic"] = T_HEAP; d["unit_of_measurement"] = "B";
    d["icon"] = "mdi:memory"; d["entity_category"] = "diagnostic";
    d["state_class"] = "measurement"; });

  discovery("sensor", "uptime", "Uptime", [](JsonDocument& d){
    d["state_topic"] = T_UPTIME; d["device_class"] = "duration";
    d["unit_of_measurement"] = "s"; d["entity_category"] = "diagnostic";
    d["state_class"] = "total_increasing"; });

  discovery("sensor", "wifi_reconnects", "Wi-Fi Reconnects", [](JsonDocument& d){
    d["state_topic"] = T_RECONNECTS; d["icon"] = "mdi:wifi-sync";
    d["entity_category"] = "diagnostic"; d["state_class"] = "total_increasing"; });

  discovery("text", "message", "Message", [](JsonDocument& d){
    d["command_topic"] = T_CMD_MESSAGE; d["state_topic"] = T_MSG_ST;
    d["icon"] = "mdi:message-text"; d["max"] = 100; });

  discovery("number", "brightness", "Brightness", [](JsonDocument& d){
    d["command_topic"] = T_CMD_BRIGHTNESS; d["state_topic"] = T_BRIGHT_ST;
    d["icon"] = "mdi:brightness-6";
    d["min"] = 0; d["max"] = 15; d["step"] = 1; d["mode"] = "slider"; });

  discovery("number", "night_brightness", "Night Brightness", [](JsonDocument& d){
    d["command_topic"] = T_CMD_NIGHT_BR; d["state_topic"] = T_NIGHT_BR_ST;
    d["icon"] = "mdi:brightness-3";
    d["min"] = 0; d["max"] = 15; d["step"] = 1; d["mode"] = "slider";
    d["entity_category"] = "config"; });

  discovery("number", "message_repeats", "Message Repeats", [](JsonDocument& d){
    d["command_topic"] = T_CMD_REPEATS; d["state_topic"] = T_REPEATS_ST;
    d["icon"] = "mdi:repeat";
    d["min"] = 1; d["max"] = 5; d["step"] = 1;
    d["entity_category"] = "config"; });

  discovery("switch", "night_dimming", "Night Dimming", [](JsonDocument& d){
    d["command_topic"] = T_CMD_NIGHT_DIM; d["state_topic"] = T_NIGHT_DIM_ST;
    d["icon"] = "mdi:weather-night"; d["entity_category"] = "config"; });

  discovery("switch", "format_12h", "12 Hour Format", [](JsonDocument& d){
    d["command_topic"] = T_CMD_12H; d["state_topic"] = T_12H_ST;
    d["icon"] = "mdi:clock-outline"; d["entity_category"] = "config"; });

  discovery("button", "show_clock", "Show Clock", [](JsonDocument& d){
    d["command_topic"] = T_CMD_RESET; d["payload_press"] = "1";
    d["icon"] = "mdi:clock"; });

  discovery("button", "show_date", "Show Date", [](JsonDocument& d){
    d["command_topic"] = T_CMD_SHOW_DATE; d["payload_press"] = "1";
    d["icon"] = "mdi:calendar"; });

#if ENABLE_DHT
  discovery("button", "show_climate", "Show Climate", [](JsonDocument& d){
    d["command_topic"] = T_CMD_SHOW_CLIMATE; d["payload_press"] = "1";
    d["icon"] = "mdi:home-thermometer"; });
#endif

  discovery("button", "restart", "Restart", [](JsonDocument& d){
    d["command_topic"] = T_CMD_RESTART; d["payload_press"] = "1";
    d["device_class"] = "restart"; d["entity_category"] = "config"; });

  Serial.println("HA discovery published");
}

void publishStates() {
  if (!mqtt.connected()) return;
  char b[8];
  snprintf(b, sizeof(b), "%d", brightness);       mqtt.publish(T_BRIGHT_ST, b, true);
  snprintf(b, sizeof(b), "%d", nightBrightness);  mqtt.publish(T_NIGHT_BR_ST, b, true);
  snprintf(b, sizeof(b), "%d", msgRepeats);       mqtt.publish(T_REPEATS_ST, b, true);
  mqtt.publish(T_NIGHT_DIM_ST, nightDimming ? "ON" : "OFF", true);
  mqtt.publish(T_12H_ST,       use12h ? "ON" : "OFF", true);
  mqtt.publish(T_MSG_ST,       customMessage.c_str(), true);
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
  char buf[24];

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
    snprintf(buf, sizeof(buf), "%d", WiFi.RSSI());        mqtt.publish(T_RSSI, buf, true);
    snprintf(buf, sizeof(buf), "%u", ESP.getFreeHeap());  mqtt.publish(T_HEAP, buf, true);
    snprintf(buf, sizeof(buf), "%lu", millis() / 1000);   mqtt.publish(T_UPTIME, buf, true);
    snprintf(buf, sizeof(buf), "%lu", wifiReconnects);    mqtt.publish(T_RECONNECTS, buf, true);
    mqtt.publish(T_IP, WiFi.localIP().toString().c_str(), true);

#if ENABLE_DHT
    if (dhtValid) {
      snprintf(buf, sizeof(buf), "%.1f", dhtTemp);  mqtt.publish(T_TEMP, buf, true);
      snprintf(buf, sizeof(buf), "%.1f", dhtHum);   mqtt.publish(T_HUM, buf, true);
    } else {
      mqtt.publish(T_TEMP, "unknown", true);
      mqtt.publish(T_HUM, "unknown", true);
    }
#endif
  }
}

/* =====================================================================
   OTA  (progress bar on the panel)
   ===================================================================== */
void setupOTA() {
  ArduinoOTA.setHostname("smartclock");
  ArduinoOTA.setPassword(OTA_PASSWORD);

  ArduinoOTA.onStart([]() { otaActive = true; showStatic("ota"); });

  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    if (total == 0) return;
    MD_MAX72XX* mx = display.getGraphicObject();
    int filled = (int)((uint32_t)progress * PANEL_COLS / total);
    mx->clear();
    for (int c = 0; c < filled && c < PANEL_COLS; c++) mx->setColumn(c, 0b00111100);
    mx->update();
  });

  ArduinoOTA.onEnd([]()  { otaActive = false; showStatic("done"); });
  ArduinoOTA.onError([](ota_error_t) { otaActive = false; showStatic("err"); });

  ArduinoOTA.begin();
}

/* =====================================================================
   SETUP
   ===================================================================== */
void setup() {
  Serial.begin(115200);
  Serial.println("\n\nSmart Clock starting...");

  loadPrefs();

  display.begin();
  display.setIntensity(brightness);
  display.displayClear();

#if ENABLE_DHT
  dht.begin();
#endif

  connectWiFi();
  setupOTA();

  mqtt.setServer(MQTT_SERVER, MQTT_PORT);
  mqtt.setCallback(mqttCallback);
  mqtt.setSocketTimeout(2);
  mqtt.setBufferSize(768);            // HA discovery payloads exceed the 256 default

  tryNtpSync();
  applyBrightness(true);

  startScroll(GREETING, 1200);
  state = SHOW_BOOT;
  Serial.printf("Setup done  mac=%s\n", WiFi.macAddress().c_str());
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
  readDht();
  publishTelemetry(false);

  if (pendingRestart) {
    mqtt.publish(T_AVAIL, "offline", true);
    mqtt.loop();
    delay(150);
    ESP.restart();
  }

  // night dimming / brightness upkeep
  static unsigned long lastBr = 0;
  if (millis() - lastBr >= BRIGHTNESS_CHECK_INTERVAL) {
    lastBr = millis();
    applyBrightness(false);
  }

  // NTP: retry fast until valid, then hourly
  unsigned long ntpInterval = haveValidTime() ? NTP_RESYNC_INTERVAL : NTP_RETRY_INTERVAL;
  if (millis() - lastNtpAttempt >= ntpInterval) {
    lastNtpAttempt = millis();
    tryNtpSync();
  }

  // Command-driven transitions (kept out of the callback so display calls
  // don't run re-entrantly inside mqtt.loop())
  if (pendingMessage) {
    pendingMessage = false;
    strncpy(scrollBuf, customMessage.c_str(), sizeof(scrollBuf) - 1);
    scrollBuf[sizeof(scrollBuf) - 1] = '\0';
    msgRepeatsLeft = msgRepeats;
    startScroll(scrollBuf, 1500);
    state = SHOW_MESSAGE;
  } else if (pendingDate && state == SHOW_CLOCK_RUN) {
    pendingDate = false;
    getDateText(scrollBuf, sizeof(scrollBuf));
    startScroll(scrollBuf, 1500);
    state = SHOW_DATE;
  } else if (pendingClimate && state == SHOW_CLOCK_RUN) {
    pendingClimate = false;
    getClimateText(scrollBuf, sizeof(scrollBuf));
    startScroll(scrollBuf, 1500);
    state = SHOW_DATE;              // same "scroll then back to clock" handling
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
      if (scrollDone()) {
        if (--msgRepeatsLeft > 0) startScroll(scrollBuf, 1500);
        else                      state = SHOW_CLOCK_ENTRY;
      }
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
