#include <ESP8266WiFi.h>
#include <WiFiUdp.h>
#include <NTPClient.h>
#include <MD_Parola.h>
#include <MD_MAX72xx.h>
#include <SPI.h>
#include <ArduinoJson.h>
#include <PubSubClient.h>
#include <Wire.h>
#include <RTClib.h>
#include "secrets.h"  // Include credentials file
#include <ArduinoOTA.h>


/* =====================================================
   ---------------- CONFIGURATION ----------------------
   ===================================================== */

const char* mqtt_client_id = "smart_clock";

/* ================= MQTT TOPICS ================= */
#define TOPIC_AVAILABILITY "smart_clock/status"
#define TOPIC_RTC_STATUS "smart_clock/rtc/status"
#define TOPIC_RTC_TIME "smart_clock/rtc/time"
#define TOPIC_RTC_DATE "smart_clock/rtc/date"
#define TOPIC_RTC_TEMPERATURE "smart_clock/rtc/temperature"

#define TOPIC_CMD_MESSAGE "smart_clock/cmd/message"
#define TOPIC_CMD_PRESET "smart_clock/cmd/preset"
#define TOPIC_CMD_BRIGHTNESS "smart_clock/cmd/brightness"
#define TOPIC_CMD_SYNC_RTC "smart_clock/cmd/sync_rtc"
#define TOPIC_CMD_RESET "smart_clock/cmd/reset"

#define HARDWARE_TYPE MD_MAX72XX::FC16_HW
#define MAX_DEVICES 6
#define CLK_PIN 14   // D5
#define DATA_PIN 13  // D7
#define CS_PIN 15    // D8

#define I2C_SDA 4  // GPIO4
#define I2C_SCL 5  // GPIO5

#define MIN_VALID_EPOCH 1700000000UL
#define NTP_SYNC_INTERVAL 3600000UL     // 1 hour, once RTC is valid
#define NTP_RETRY_INTERVAL 30000UL      // 30 seconds, while RTC is invalid
#define WIFI_TIMEOUT 20000UL             // 20 seconds
#define WIFI_RECONNECT_INTERVAL 30000UL  // 30 seconds between reconnect attempts
#define RTC_RETRY_INTERVAL 30000UL       // 30 seconds between RTC re-init attempts

/* =====================================================
   ---------------- OBJECTS -----------------------------
   ===================================================== */

WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", 19800, 60000);

MD_Parola display(HARDWARE_TYPE, DATA_PIN, CLK_PIN, CS_PIN, MAX_DEVICES);
RTC_DS3231 rtc;
WiFiClient espClient;
PubSubClient mqttClient(espClient);

/* =====================================================
   ---------------- STATE -------------------------------
   ===================================================== */

String customMessage;

enum DisplayState {
  SHOW_WELCOME,
  SHOW_BRAND,
  SHOW_IP,
  SHOW_MESSAGE,
  SHOW_CLOCK_ENTRY,
  SHOW_CLOCK_RUN,
  SHOW_DATE
};

DisplayState state = SHOW_WELCOME;
bool scrollComplete = false;

bool rtcPresent = false;
bool rtcValid = false;
bool ntpTimeValid = false;  // true once NTP has synced at least once, independent of the RTC
bool colonState = false;
bool use12HourFormat = false;
volatile bool otaActive = false;

unsigned long lastBlink = 0;
unsigned long lastStateChange = 0;
unsigned long lastNtpAttempt = 0 - NTP_SYNC_INTERVAL;  // forces an immediate sync attempt on first boot
unsigned long lastDateScroll = 0;

const unsigned long DATE_SCROLL_INTERVAL = 300000UL;  // 5 minutes

/* =====================================================
   ---------------- FORWARD DECLARATIONS ----------------
   ===================================================== */

void startScrollingText(const char* text, uint16_t speed, uint16_t pause);
void showStatus(const char* text);
void getTimeString(char* buf, size_t len);
void getDateString(char* buf, size_t len);
bool haveValidTime();
DateTime getCurrentDateTime();
void syncNtpToRtc();
void mqttReconnect();
void publishHADiscovery();
void publishRtcTelemetry();

/* =====================================================
   ---------------- WIFI CONNECTION ---------------------
   ===================================================== */

bool connectWiFi() {
  Serial.println("Connecting to WiFi...");
  display.displayClear();
  showStatus("WiFi..");

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  unsigned long startAttempt = millis();

  while (WiFi.status() != WL_CONNECTED &&
         millis() - startAttempt < WIFI_TIMEOUT) {
    delay(500);
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi connected!");
    Serial.print("IP: ");
    Serial.println(WiFi.localIP());
    return true;
  } else {
    Serial.println("\nWiFi connection failed!");
    display.displayClear();
    showStatus("WiFi FAIL");
    delay(3000);
    return false;
  }
}

void ensureWiFiConnected() {
  if (WiFi.status() == WL_CONNECTED) return;

  static unsigned long lastAttempt = 0;
  if (millis() - lastAttempt < WIFI_RECONNECT_INTERVAL) return;
  lastAttempt = millis();

  Serial.println("WiFi disconnected, attempting reconnect...");
  WiFi.disconnect();
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
}

/* =====================================================
   ---------------- MQTT --------------------------------
   ===================================================== */

void mqttReconnect() {
  if (mqttClient.connected()) return;

  static unsigned long lastAttempt = 0;
  if (millis() - lastAttempt < 5000) return;
  lastAttempt = millis();

  Serial.print("MQTT connecting... ");

  if (mqttClient.connect(
        mqtt_client_id,
        MQTT_USER,
        MQTT_PASS,
        TOPIC_AVAILABILITY,
        0,
        true,
        "offline")) {

    Serial.println("connected");
    mqttClient.publish(TOPIC_AVAILABILITY, "online", true);

    publishHADiscovery();

    mqttClient.subscribe(TOPIC_CMD_MESSAGE);
    mqttClient.subscribe(TOPIC_CMD_PRESET);
    mqttClient.subscribe(TOPIC_CMD_BRIGHTNESS);
    mqttClient.subscribe(TOPIC_CMD_SYNC_RTC);
    mqttClient.subscribe(TOPIC_CMD_RESET);

  } else {
    Serial.print("failed, rc=");
    Serial.println(mqttClient.state());
  }
}

void publishDiscoverySensor(const char* objectId, const char* name, const char* icon,
                             const char* stateTopic, const char* deviceClass,
                             const char* unit) {
  StaticJsonDocument<384> doc;
  char payload[384];
  char uniqueId[40];
  char configTopic[64];

  snprintf(uniqueId, sizeof(uniqueId), "smart_clock_%s", objectId);
  snprintf(configTopic, sizeof(configTopic),
           "homeassistant/sensor/smart_clock/%s/config", objectId);

  doc["name"] = name;
  doc["unique_id"] = uniqueId;
  doc["state_topic"] = stateTopic;
  doc["availability_topic"] = TOPIC_AVAILABILITY;
  doc["icon"] = icon;
  if (deviceClass) doc["device_class"] = deviceClass;
  if (unit) doc["unit_of_measurement"] = unit;

  // Shared device block so all sensors group under one "Smart Clock" device in HA
  JsonObject device = doc.createNestedObject("device");
  device["identifiers"][0] = "smart_clock";
  device["name"] = "Smart Clock";
  device["manufacturer"] = "DIY";
  device["model"] = "ESP8266 + MAX7219 + DS3231";

  serializeJson(doc, payload);
  mqttClient.publish(configTopic, payload, true);
}

void publishHADiscovery() {
  publishDiscoverySensor("rtc_status", "RTC Status", "mdi:clock-check",
                          TOPIC_RTC_STATUS, nullptr, nullptr);
  publishDiscoverySensor("rtc_time", "RTC Time", "mdi:clock-digital",
                          TOPIC_RTC_TIME, nullptr, nullptr);
  publishDiscoverySensor("rtc_date", "RTC Date", "mdi:calendar",
                          TOPIC_RTC_DATE, nullptr, nullptr);
  publishDiscoverySensor("rtc_temperature", "RTC Temperature", "mdi:thermometer",
                          TOPIC_RTC_TEMPERATURE, "temperature", "°C");

  Serial.println("HA discovery published");
}

void publishRtcTelemetry() {
  static unsigned long lastPub = 0;
  if (millis() - lastPub < 60000) return;
  lastPub = millis();

  if (!mqttClient.connected()) return;

  const char* status;
  if (rtcPresent && rtcValid) status = "OK";
  else if (ntpTimeValid) status = "NTP_ONLY";
  else status = "INVALID";
  mqttClient.publish(TOPIC_RTC_STATUS, status, true);

  if (haveValidTime()) {
    static char buf[16];

    getTimeString(buf, sizeof(buf));
    mqttClient.publish(TOPIC_RTC_TIME, buf, true);

    getDateString(buf, sizeof(buf));
    mqttClient.publish(TOPIC_RTC_DATE, buf, true);
  }

  if (rtcPresent) {  // temperature needs the physical chip, independent of time validity
    static char tempBuf[16];
    float t = rtc.getTemperature();
    snprintf(tempBuf, sizeof(tempBuf), "%.1f", t);
    mqttClient.publish(TOPIC_RTC_TEMPERATURE, tempBuf, true);
  }
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  if (length > 100) return;  // Safety check

  static char msg[101];
  memcpy(msg, payload, length);
  msg[length] = '\0';

  if (strcmp(topic, TOPIC_CMD_MESSAGE) == 0 || strcmp(topic, TOPIC_CMD_PRESET) == 0) {
    customMessage = msg;
    state = SHOW_MESSAGE;
  }
  else if (strcmp(topic, TOPIC_CMD_BRIGHTNESS) == 0) {
    char* endPtr;
    long level = strtol(msg, &endPtr, 10);
    if (endPtr != msg) {  // only apply if the payload actually parsed as a number
      display.setIntensity(constrain((int)level, 0, 15));
    }
  }
  else if (strcmp(topic, TOPIC_CMD_SYNC_RTC) == 0) {
    syncNtpToRtc();
  }
  else if (strcmp(topic, TOPIC_CMD_RESET) == 0) {
    state = SHOW_CLOCK_ENTRY;
  }
}

/* =====================================================
   ---------------- TIME LOGIC --------------------------
   ===================================================== */

void syncNtpToRtc() {
  if (!WiFi.isConnected()) return;

  if (timeClient.update()) {
    time_t epoch = timeClient.getEpochTime();
    if (epoch > MIN_VALID_EPOCH) {
      ntpTimeValid = true;  // usable time source regardless of whether an RTC is present

      if (rtcPresent) {
        rtc.adjust(DateTime(epoch));
        rtcValid = true;
        Serial.println("RTC synced with NTP");
      } else {
        Serial.println("NTP time acquired (no RTC present, using NTP directly)");
      }
    }
  }
}

// True once there's ANY usable time source — the RTC if present and valid,
// otherwise NTP alone. This is what getTimeString()/getDateString() gate on,
// so the clock still works over the network even with no RTC hardware at all.
bool haveValidTime() {
  return (rtcPresent && rtcValid) || ntpTimeValid;
}

// DateTime is pure calendar math in RTClib — it works standalone from a raw
// epoch, no physical RTC required. Prefer the RTC when it's actually valid
// (keeps ticking correctly between NTP syncs without network jitter);
// otherwise fall back to NTP's live, continuously-extrapolated time.
DateTime getCurrentDateTime() {
  if (rtcPresent && rtcValid) return rtc.now();
  return DateTime((uint32_t)timeClient.getEpochTime());
}

void ensureRtcPresent() {
  if (rtcPresent) return;

  static unsigned long lastAttempt = 0 - RTC_RETRY_INTERVAL;  // don't delay the first retry
  if (millis() - lastAttempt < RTC_RETRY_INTERVAL) return;
  lastAttempt = millis();

  if (rtc.begin()) {
    rtcPresent = true;
    Serial.println("RTC detected");
    if (!rtc.lostPower() && rtc.now().unixtime() > MIN_VALID_EPOCH) {
      rtcValid = true;
    }
  }
}

void getTimeString(char* buf, size_t len) {
  if (!haveValidTime()) {
    strncpy(buf, "-- --", len);
    return;
  }

  DateTime now = getCurrentDateTime();
  int h = now.hour();
  int m = now.minute();

  if (millis() - lastBlink >= 500) {
    colonState = !colonState;
    lastBlink = millis();
  }

  if (use12HourFormat) {
    h %= 12;
    if (h == 0) h = 12;
  }

  snprintf(buf, len, "%02d%c%02d", h, colonState ? ':' : ' ', m);
}

void getDateString(char* buf, size_t len) {
  if (!haveValidTime()) {
    strncpy(buf, "--/---/--", len);
    return;
  }

  static const char* months[] = {
    "JAN", "FEB", "MAR", "APR", "MAY", "JUN",
    "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"
  };

  DateTime now = getCurrentDateTime();
  uint8_t d = now.day();
  uint8_t m = now.month();
  uint8_t y = now.year() % 100;

  if (m < 1 || m > 12) {
    strncpy(buf, "--/ERR/--", len);
    return;
  }

  snprintf(buf, len, "%02d/%s/%02d", d, months[m - 1], y);
}

/* =====================================================
   ---------------- NON-BLOCKING DISPLAY ----------------
   ===================================================== */

void startScrollingText(const char* text, uint16_t speed, uint16_t pause) {
  display.displayClear();
  display.displayText(
    text,
    PA_CENTER,
    speed,
    pause,
    PA_SCROLL_LEFT,
    PA_SCROLL_LEFT);
  scrollComplete = false;
}

// Static status text (WiFi.., UPDATE, etc). Uses displayText()+displayAnimate()
// rather than display.print() purely as a defensive habit — functionally
// equivalent in this single-zone display, but non-blocking by construction.
void showStatus(const char* text) {
  display.displayText(text, PA_CENTER, 0, 0, PA_PRINT, PA_NO_EFFECT);
  display.displayAnimate();
}

bool updateScrollingText() {
  if (display.displayAnimate()) {
    scrollComplete = true;
    display.displayClear();
    return true;  // Animation complete
  }
  return false;  // Still animating
}

// OTA configuration

void setupOTA() {
  ArduinoOTA.setHostname("smartclock");
  ArduinoOTA.setPassword(OTA_PASSWORD);

  ArduinoOTA.onStart([]() {
    otaActive = true;
    display.displayClear();
    showStatus("UPDATE");
  });

  ArduinoOTA.onEnd([]() {
    display.displayClear();
    showStatus("DONE");
    otaActive = false;
  });

  ArduinoOTA.onError([](ota_error_t error) {
    display.displayClear();
    showStatus("ERROR");
    otaActive = false;
  });

  ArduinoOTA.begin();
  Serial.println("OTA Ready");
}
/* =====================================================
   ---------------- SETUP -------------------------------
   ===================================================== */

void setup() {
  Serial.begin(115200);
  Serial.println("\n\nSmart Clock Starting...");

  // Initialize display first
  display.begin();
  display.setIntensity(5);
  display.displayClear();

  // Connect WiFi with timeout
  if (!connectWiFi()) {
    Serial.println("Continuing without WiFi...");
  }
  setupOTA();  // <--- ADD THIS
  // Setup MQTT
  mqttClient.setServer(MQTT_SERVER, MQTT_PORT);
  mqttClient.setCallback(mqttCallback);
  mqttClient.setSocketTimeout(2);  // cap worst-case blocking in connect()/loop() at 2s

  // Start NTP client
  timeClient.begin();

  // Initialize I2C and RTC
  Wire.begin(I2C_SDA, I2C_SCL);

  if (!rtc.begin()) {
    Serial.println("RTC not found!");
  } else {
    rtcPresent = true;
    if (rtc.lostPower()) {
      Serial.println("RTC lost power, needs sync");
    } else if (rtc.now().unixtime() <= MIN_VALID_EPOCH) {
      Serial.println("RTC time invalid");
    } else {
      rtcValid = true;
      Serial.println("RTC initialized OK");
    }
  }

  Serial.println("Setup complete!");
}

/* =====================================================
   ---------------- LOOP --------------------------------
   ===================================================== */

void loop() {

  ArduinoOTA.handle();
  if (otaActive) return;  // freeze everything else while a flash write is in progress

  ensureWiFiConnected();
  mqttReconnect();
  mqttClient.loop();
  publishRtcTelemetry();

  ensureRtcPresent();

  // Sync from NTP: retry frequently until we have any valid time source,
  // resync hourly once we do (whether that's from the RTC or NTP alone)
  unsigned long ntpInterval = haveValidTime() ? NTP_SYNC_INTERVAL : NTP_RETRY_INTERVAL;
  if (millis() - lastNtpAttempt >= ntpInterval) {
    lastNtpAttempt = millis();
    syncNtpToRtc();
  }

  // Handle display states
  switch (state) {

    case SHOW_WELCOME:
      startScrollingText("WASSUP", 50, 1000);
      state = SHOW_BRAND;
      break;

    case SHOW_BRAND:
      if (updateScrollingText()) {
        startScrollingText("Ragshome", 50, 2000);
        state = SHOW_IP;
      }
      break;

    case SHOW_IP:
      if (updateScrollingText()) {
        static char ipBuf[48];
        snprintf(ipBuf, sizeof(ipBuf), "IP:%s",
                 WiFi.localIP().toString().c_str());
        startScrollingText(ipBuf, 50, 3000);
        state = SHOW_CLOCK_ENTRY;
      }
      break;

    case SHOW_MESSAGE:
      if (updateScrollingText()) {
        startScrollingText(customMessage.c_str(), 50, 1000);
        state = SHOW_CLOCK_ENTRY;
      }
      break;

    case SHOW_CLOCK_ENTRY:
      // This is the missing state that was causing the bug!
      if (updateScrollingText()) {
        display.displayClear();
        lastDateScroll = millis();
        state = SHOW_CLOCK_RUN;
      }
      break;

    case SHOW_CLOCK_RUN:
      {
        static char timeBuf[16];
        getTimeString(timeBuf, sizeof(timeBuf));

        // Check if it's time to show date
        if (millis() - lastDateScroll >= DATE_SCROLL_INTERVAL) {
          lastDateScroll = millis();
          static char dateBuf[16];
          getDateString(dateBuf, sizeof(dateBuf));
          startScrollingText(dateBuf, 50, 2000);
          state = SHOW_DATE;
        } else {
          // Display time normally
          display.displayText(
            timeBuf,
            PA_CENTER,
            0,
            0,
            PA_PRINT,
            PA_NO_EFFECT);
          display.displayAnimate();
        }
        break;
      }

    case SHOW_DATE:
      if (updateScrollingText()) {
        state = SHOW_CLOCK_ENTRY;
      }
      break;
  }
}
