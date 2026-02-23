#include <ESP8266WiFi.h>
#include <WiFiUdp.h>
#include <NTPClient.h>
#include <MD_Parola.h>
#include <MD_MAX72xx.h>
#include <SPI.h>
#include <ESPAsyncTCP.h>
#include <ESPAsyncWebServer.h>
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
#define NTP_SYNC_INTERVAL 3600000UL  // 1 hour
#define WIFI_TIMEOUT 20000UL         // 20 seconds

/* =====================================================
   ---------------- OBJECTS -----------------------------
   ===================================================== */

WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", 19800, 60000);

AsyncWebServer server(80);
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

bool rtcValid = false;
bool colonState = false;
bool use12HourFormat = false;

unsigned long lastBlink = 0;
unsigned long lastStateChange = 0;
unsigned long lastNtpSync = 0;
unsigned long lastDateScroll = 0;

const unsigned long DATE_SCROLL_INTERVAL = 300000UL;  // 5 minutes

/* =====================================================
   ---------------- FORWARD DECLARATIONS ----------------
   ===================================================== */

void startScrollingText(const char* text, uint16_t speed, uint16_t pause);
void getTimeString(char* buf, size_t len);
void getDateString(char* buf, size_t len);
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
  display.print("WiFi..");
  
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
    display.print("WiFi FAIL");
    delay(3000);
    return false;
  }
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
        "smart_clock/status",
        0,
        true,
        "offline")) {

    Serial.println("connected");
    mqttClient.publish("smart_clock/status", "online", true);

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

void publishHADiscovery() {
  StaticJsonDocument<256> doc;
  char payload[256];

  // RTC STATUS
  doc.clear();
  doc["name"] = "RTC Status";
  doc["unique_id"] = "smart_clock_rtc_status";
  doc["state_topic"] = TOPIC_RTC_STATUS;
  doc["availability_topic"] = TOPIC_AVAILABILITY;
  doc["icon"] = "mdi:clock-check";
  serializeJson(doc, payload);
  mqttClient.publish(
    "homeassistant/sensor/smart_clock/rtc_status/config",
    payload,
    true);

  // RTC TIME
  doc.clear();
  doc["name"] = "RTC Time";
  doc["unique_id"] = "smart_clock_rtc_time";
  doc["state_topic"] = TOPIC_RTC_TIME;
  doc["availability_topic"] = TOPIC_AVAILABILITY;
  doc["icon"] = "mdi:clock-digital";
  serializeJson(doc, payload);
  mqttClient.publish(
    "homeassistant/sensor/smart_clock/rtc_time/config",
    payload,
    true);

  // RTC DATE
  doc.clear();
  doc["name"] = "RTC Date";
  doc["unique_id"] = "smart_clock_rtc_date";
  doc["state_topic"] = TOPIC_RTC_DATE;
  doc["availability_topic"] = TOPIC_AVAILABILITY;
  doc["icon"] = "mdi:calendar";
  serializeJson(doc, payload);
  mqttClient.publish(
    "homeassistant/sensor/smart_clock/rtc_date/config",
    payload,
    true);

  // RTC TEMPERATURE
  doc.clear();
  doc["name"] = "RTC Temperature";
  doc["unique_id"] = "smart_clock_rtc_temperature";
  doc["state_topic"] = TOPIC_RTC_TEMPERATURE;
  doc["availability_topic"] = TOPIC_AVAILABILITY;
  doc["unit_of_measurement"] = "°C";
  doc["device_class"] = "temperature";
  doc["icon"] = "mdi:thermometer";
  serializeJson(doc, payload);
  mqttClient.publish(
    "homeassistant/sensor/smart_clock/rtc_temperature/config",
    payload,
    true);

  Serial.println("HA discovery published");
}

void publishRtcTelemetry() {
  static unsigned long lastPub = 0;
  if (millis() - lastPub < 60000) return;
  lastPub = millis();

  if (!mqttClient.connected()) return;

  mqttClient.publish(
    TOPIC_RTC_STATUS,
    rtcValid ? "OK" : "INVALID",
    true);

  if (rtcValid) {
    static char buf[16];

    getTimeString(buf, sizeof(buf));
    mqttClient.publish(TOPIC_RTC_TIME, buf, true);

    getDateString(buf, sizeof(buf));
    mqttClient.publish(TOPIC_RTC_DATE, buf, true);

    float t = rtc.getTemperature();
    snprintf(buf, sizeof(buf), "%.1f", t);
    mqttClient.publish(TOPIC_RTC_TEMPERATURE, buf, true);
  }
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  if (length > 100) return;  // Safety check
  
  String msg;
  for (unsigned int i = 0; i < length; i++) {
    msg += (char)payload[i];
  }

  if (String(topic) == TOPIC_CMD_MESSAGE || String(topic) == TOPIC_CMD_PRESET) {
    customMessage = msg;
    state = SHOW_MESSAGE;
  }
  else if (String(topic) == TOPIC_CMD_BRIGHTNESS) {
    display.setIntensity(constrain(msg.toInt(), 0, 15));
  }
  else if (String(topic) == TOPIC_CMD_SYNC_RTC) {
    syncNtpToRtc();
  }
  else if (String(topic) == TOPIC_CMD_RESET) {
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
      rtc.adjust(DateTime(epoch));
      rtcValid = true;
      lastNtpSync = millis();
      Serial.println("RTC synced with NTP");
    }
  }
}

void getTimeString(char* buf, size_t len) {
  if (!rtcValid) {
    strncpy(buf, "-- --", len);
    return;
  }

  DateTime now = rtc.now();
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
  if (!rtcValid) {
    strncpy(buf, "--/---/--", len);
    return;
  }

  static const char* months[] = {
    "JAN", "FEB", "MAR", "APR", "MAY", "JUN",
    "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"
  };

  DateTime now = rtc.now();
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
    display.displayClear();
    display.print("UPDATE");
  });
  
  ArduinoOTA.onEnd([]() {
    display.displayClear();
    display.print("DONE");
  });
  
  ArduinoOTA.onError([](ota_error_t error) {
    display.displayClear();
    display.print("ERROR");
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
  
  // Start NTP client
  timeClient.begin();

  // Initialize I2C and RTC
  Wire.begin(I2C_SDA, I2C_SCL);
  
  if (!rtc.begin()) {
    Serial.println("RTC not found!");
  } else if (rtc.lostPower()) {
    Serial.println("RTC lost power, needs sync");
  } else if (rtc.now().unixtime() <= MIN_VALID_EPOCH) {
    Serial.println("RTC time invalid");
  } else {
    rtcValid = true;
    Serial.println("RTC initialized OK");
  }

  Serial.println("Setup complete!");
}

/* =====================================================
   ---------------- LOOP --------------------------------
   ===================================================== */

void loop() {
  
  ArduinoOTA.handle();  // <--- ADD THIS
  mqttReconnect();
  mqttClient.loop();
  publishRtcTelemetry();

  // Try to sync RTC if needed
  if (!rtcValid) syncNtpToRtc();
  if (millis() - lastNtpSync > NTP_SYNC_INTERVAL) syncNtpToRtc();

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
