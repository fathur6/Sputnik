/*
  ==============================================================================================================================================
  Project: Sputnik IoT Sensor Module – WiFi‑Manager, Google Sheets POST, NTP (MYT), Arduino Cloud & OLED Dashboard

  Author   : Fathurrahman Lananan (Aman)
  Revision : 2025‑06‑07 (MYT Time‑Fix Stable)

  Highlights
  ----------
  • Soft‑AP fallback via WiFiManager for hassle‑free Wi‑Fi setup.
  • OLED shows temperature, humidity, Wi‑Fi, Cloud status and Malaysia time (MYT, UTC+8).
  • Arduino IoT Cloud integration (1 min refresh).
  • 15‑min averaged data posted securely to Google Sheets.
  • Reliable NTP sync using configTime (GMT_OFFSET = +8 h) – no TZ overrides.
  • Clean utility functions and circular buffer management.

  Libraries
  ---------
  Adafruit_SSD1306 · Adafruit_GFX · DHT sensor · WiFiManager · ArduinoIoTCloud
  ==============================================================================================================================================
*/

// ─────────────────────────── Core / Platform ────────────────────────────────
#if defined(ESP32)
  #include <WiFi.h>
  #include <HTTPClient.h>
  #include <WiFiClientSecure.h>
#else
  #error "Unsupported board – compile for ESP32." 
#endif

#include <WiFiManager.h>
#include "DHT.h"
#include <time.h>
#include "thingProperties.h"

// ─────────────────────────── Display ────────────────────────────────────────
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// ─────────────────────────── Constants ──────────────────────────────────────
// Hardware
const int DHT_PIN        = 15;
const int DHT_TYPE       = DHT21;      // AM2301
const int OLED_WIDTH     = 128;
const int OLED_HEIGHT    = 64;
const int OLED_ADDR      = 0x3C;
const int OLED_RST       = -1;

// Wi‑Fi Manager
const char* AP_SSID      = "Sputnik-Setup";
const char* AP_PASS      = "sputnik123";

// Time / NTP
const char* NTP_SERVER   = "pool.ntp.org";
const long  GMT_OFFSET   = 8 * 3600;   // +8 h for Malaysia
const int   DST_OFFSET   = 0;          // no DST in MY

// Google Sheets
const char* GSCRIPT_URL  = "https://script.google.com/macros/s/AKfycbzcdQJjV7odm73Z5B0zyyVLhqCZZcaRRbypB4TE5a2vXtaNR5nmld4wX6vs4q3KisRfUw/exec";
const int   HTTP_TIMEOUT = 10000;      // 10 s

// Sampling / Reporting
const int BUF_SIZE       = 15;         // 15 samples ≈ 15 min
const int SAMPLE_SEC     = 60;         // 1‑min sample
const int REPORT_MIN     = 15;         // quarter‑hour post

// ─────────────────────────── Globals ────────────────────────────────────────
Adafruit_SSD1306 display(OLED_WIDTH, OLED_HEIGHT, &Wire, OLED_RST);
DHT               dht(DHT_PIN, DHT_TYPE);
WiFiManager       wifiManager;
WiFiClientSecure  httpsClient;

float tBuf[BUF_SIZE];
float hBuf[BUF_SIZE];
int   bufIdx = 0;

// ─────────────────────────── Declarations ───────────────────────────────────
void setupWiFiAndTime();
void resetBuffers();
void collectSample();
void postAverage();
void updateOLED(float t, float h);
void printTime(const char* prefix="");
char wifiIcon();
char cloudIcon();

// ─────────────────────────── Setup ──────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n===== Sputnik IoT Module Initializing =====");

  // OLED splash
  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    Serial.println("[OLED] init failed");
  } else {
    display.clearDisplay();
    display.setTextSize(2);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(10, 20);
    display.println(F("Sputnik"));
    display.setTextSize(1);
    display.println(F("     IoT Monitor"));
    display.display();
    delay(1200);
  }

  dht.begin();
  setupWiFiAndTime();     // handles Wi‑Fi & NTP (UTC+8)
  resetBuffers();

  Serial.println("===== Setup Complete. Starting Main Loop. =====");
  printTime("[Time Check Post-Setup] ");
  Serial.printf("Free heap: %u bytes\n", ESP.getFreeHeap());
}

// ─────────────────────────── Main Loop ─────────────────────────────────────
void loop() {
  ArduinoCloud.update();

  static uint32_t lastSample = 0;
  if (millis() - lastSample >= SAMPLE_SEC * 1000UL) {
    lastSample = millis();
    collectSample();

    struct tm now;
    if (getLocalTime(&now, 50) && now.tm_min % REPORT_MIN == 0) {
      static int lastPostMin = -1;
      if (now.tm_min != lastPostMin) {
        printTime("[Timing] Quarter-hour → post ");
        postAverage();
        lastPostMin = now.tm_min;
      }
    } else if (bufIdx >= BUF_SIZE) {
      Serial.println("[Timing] Buffer full – fallback post");
      postAverage();
    }
  }
}

// ─────────────────────────── Wi‑Fi + Time ──────────────────────────────────
void setupWiFiAndTime() {
  // Wi‑Fi via WiFiManager
  wifiManager.setConnectTimeout(20);
  wifiManager.setConfigPortalTimeout(180);
  if (!wifiManager.autoConnect(AP_SSID, AP_PASS)) ESP.restart();
  Serial.printf("[WiFi] Connected to: %s\n", WiFi.SSID().c_str());

  // NTP sync using configTime with UTC+8 offset
  Serial.println("[NTP] Syncing time (MYT, UTC+8)…");
  configTime(GMT_OFFSET, DST_OFFSET, NTP_SERVER);

  // wait (max 5 s) for sync
  for (int i = 0; i < 50; ++i) {
    struct tm t;
    if (getLocalTime(&t, 0) && t.tm_year > 70) break;
    delay(100);
  }

  // Arduino IoT Cloud
  initProperties();
  ArduinoCloud.begin(ArduinoIoTPreferredConnection);
  setDebugMessageLevel(1);

  // Update OLED once after time sync
  updateOLED(NAN, NAN);
}

// ─────────────────────────── Data Acquisition ─────────────────────────────
void collectSample() {
  // re-sync NTP clock to GMT+8
  configTime(GMT_OFFSET, DST_OFFSET, NTP_SERVER);
  
  float h = dht.readHumidity();
  float t = dht.readTemperature();
  float tStore = (isnan(t) || t < -40 || t > 85) ? NAN : t;
  float hStore = (isnan(h) || h < 0 || h > 100) ? NAN : h;

  if (bufIdx < BUF_SIZE) {
    tBuf[bufIdx] = tStore;
    hBuf[bufIdx] = hStore;
    ++bufIdx;
  } else {
    memmove(tBuf, tBuf + 1, (BUF_SIZE - 1) * sizeof(float));
    memmove(hBuf, hBuf + 1, (BUF_SIZE - 1) * sizeof(float));
    tBuf[BUF_SIZE - 1] = tStore;
    hBuf[BUF_SIZE - 1] = hStore;
  }

  printTime("[Sensor] ");
  Serial.printf("Sample [%2d/%2d] | T: %.2f C, H: %.2f %%\n", bufIdx, BUF_SIZE, tStore, hStore);

  dhtTemp = tStore;
  dhtHumi = hStore;
  updateOLED(tStore, hStore);
}

// ─────────────────────────── Posting to Google Sheets ──────────────────────
void postAverage() {
  display.clearDisplay();
  display.setTextSize(2);
  display.setCursor(20, 20);
  display.print("Posting");
  display.display();

  if (bufIdx == 0) {
    Serial.println("[POST] buffer empty");
    return;
  }

  float sT = 0, sH = 0;
  int n = 0;
  for (int i = 0; i < bufIdx; ++i) {
    if (!isnan(tBuf[i]) && !isnan(hBuf[i])) {
      sT += tBuf[i];
      sH += hBuf[i];
      ++n;
    }
  }
  if (n == 0) {
    Serial.println("[POST] no valid pairs");
    resetBuffers();
    return;
  }

  float avgT = sT / n;
  float avgH = sH / n;
  printTime("[POST] ");
  Serial.printf("Avg T: %.2f H: %.2f (n=%d)\n", avgT, avgH, n);
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[POST] Wi-Fi down");
    return;
  }

  HTTPClient http;
  httpsClient.setInsecure();
  if (http.begin(httpsClient, GSCRIPT_URL)) {
    http.addHeader("Content-Type", "application/json; charset=utf-8");
    http.setTimeout(HTTP_TIMEOUT);
    char payload[128];
    snprintf(payload, sizeof(payload), "{\"temperature\":%.2f,\"humidity\":%.2f,\"mcu\":\"sputnik1\"}", avgT, avgH);
    Serial.printf("[POST] JSON: %s\n", payload);
    int code = http.POST(payload);

    if (code > 0 && (code == HTTP_CODE_OK || (code >= 300 && code < 400))) {
      Serial.printf("[POST] Success (%d) – buffer reset\n", code);
      resetBuffers();
    } else {
      Serial.printf("[POST] Failed (%d): %s\n", code, http.errorToString(code).c_str());
    }
    http.end();
  } else {
    Serial.println("[POST] HTTP begin failed");
  }
  Serial.printf("Heap after POST: %u bytes\n", ESP.getFreeHeap());
}

// ─────────────────────────── OLED Update ───────────────────────────────────
void updateOLED(float t, float h) {
  struct tm now;
  bool tOK = getLocalTime(&now, 0);
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  display.setTextSize(2);
  display.setCursor(0, 0);
  display.print("T:");
  isnan(t) ? display.print("--") : display.printf("%.1f", t);
  display.print((char)247);
  display.println("C");

  display.setCursor(0, 24);
  display.print("H:");
  isnan(h) ? display.print("--") : display.printf("%.1f", h);
  display.println(" %");

  display.drawFastHLine(0, 48, OLED_WIDTH, SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(0, 52);
  display.printf("WiFi:%c", wifiIcon());
  display.setCursor(44, 52);
  display.printf("Cloud:%c", cloudIcon());
  display.setCursor(90, 52);
  if (tOK) display.printf("%02d:%02d", now.tm_hour, now.tm_min);
  else display.print("--:--");
  display.display();
}

// ─────────────────────────── Utility ───────────────────────────────────────
void resetBuffers() {
  Serial.println("[Buffer] Resetting data buffers.");
  for (int i = 0; i < BUF_SIZE; ++i) {
    tBuf[i] = NAN;
    hBuf[i] = NAN;
  }
  bufIdx = 0;
}

void printTime(const char* prefix) {
  struct tm t;
  if (getLocalTime(&t, 0)) {
    Serial.print(prefix);
    char buf[64];
    strftime(buf, sizeof(buf), "%A, %B %d %Y %H:%M:%S", &t);
    Serial.print(buf);
    Serial.println(" MYT");
  } else {
    Serial.print(prefix);
    Serial.println("Time not available");
  }
}

char wifiIcon() {
  if (WiFi.status() != WL_CONNECTED) return 'X';
  int8_t rssi = WiFi.RSSI();
  if (rssi > -67) return '+';   // strong
  if (rssi > -80) return '/';   // medium
  return '-';                   // weak
}

char cloudIcon() {
  return ArduinoCloud.connected() ? 'C' : (WiFi.status() == WL_CONNECTED ? 'c' : 'x');
}
