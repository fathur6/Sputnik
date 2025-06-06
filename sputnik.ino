/*
  ==============================================================================================================================================
  Project: sputnik IoT Sensor Module – WiFi-Manager Soft-AP Fallback PLUS 15-Minute Averaged Google Sheets POST with NTP Fallback (GMT+8)

  Author: Fathurrahman Lananan (Aman)
  Last Modified: 2025-06-05

  Description:
  This sketch implements a robust ESP32-based IoT data logger system. It includes:
  - Soft-AP fallback using WiFiManager for WiFi configuration when no known network is available.
  - Arduino IoT Cloud integration for real-time temperature and humidity updates every minute.
  - A 15-minute data averaging routine that samples sensor data every minute, stores up to 15 samples,
    and at every quarter-hour mark (HH:00, HH:15, HH:30, HH:45), calculates the average of valid readings
    and posts them to a designated Google Sheets endpoint via HTTPS.
  - NTP time synchronization for precise scheduling of Google Sheets posts.

  The sketch uses error-checked sampling (ignoring NaN or invalid values) and only transmits valid averaged data.
  Data is buffered in arrays for temperature and humidity with size 15, and cleared after each successful POST.
  This ensures robustness in unstable network or sensor conditions.
  ==============================================================================================================================================
*/

#if defined(ESP32)
  #include <WiFi.h>
  #include <HTTPClient.h>           // For Google Sheets POST
  #include <WiFiClientSecure.h>     // For HTTPS
#else
  #error "Unsupported board: This sketch is optimized for ESP32."
#endif

#include <WiFiManager.h>             // Soft-AP captive portal library
#include "DHT.h"                     // Adafruit DHT sensor library
#include <time.h>                     // NTP time functions
#include "thingProperties.h"         // Arduino IoT Cloud: binds dhtTemp/dhtHumi & device credentials

// --- Constants ---
const int DHT_PIN             = 15;
const int DHT_TYPE            = DHT21;
const char* AP_CONFIG_SSID    = "Sputnik-Setup";
const char* AP_CONFIG_PASS    = "sputnik123";
const char* NTP_SERVER_URL    = "pool.ntp.org";
const long  GMT_OFFSET_SECONDS= 8 * 3600;
const int   DAYLIGHT_OFFSET   = 0;
const int   NTP_TIMEOUT_MS    = 7000;
const int   NTP_RETRY_DELAY   = 500;
const char* GOOGLE_SHEET_POST_URL = "https://script.google.com/macros/s/AKfycbzcdQJjV7odm73Z5B0zyyVLhqCZZcaRRbypB4TE5a2vXtaNR5nmld4wX6vs4q3KisRfUw/exec";
const int   HTTP_POST_TIMEOUT_MS  = 10000;
const int   DATA_BUFFER_SIZE      = 15;
const int   SAMPLING_INTERVAL_SEC = 60;
const int   REPORT_INTERVAL_MIN   = 15;

DHT dht(DHT_PIN, DHT_TYPE);
WiFiManager wifiManager;
WiFiClientSecure httpsClient;
unsigned long previousSampleMillis = 0;
float temperatureBuffer[DATA_BUFFER_SIZE];
float humidityBuffer[DATA_BUFFER_SIZE];
int bufferIndex = 0;

void initializeDHTSensor();
void initializeWiFi();
void initializeNTPTime();
void initializeArduinoCloud();
void collectSensorData();
void postAveragedDataToGoogleSheet();
void printLocalTime(const char* prefix = "");

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n\n===== Sputnik IoT Module Initializing =====");

  initializeDHTSensor();
  initializeWiFi();
  initializeNTPTime();
  initializeArduinoCloud();

  for (int i = 0; i < DATA_BUFFER_SIZE; ++i) {
    temperatureBuffer[i] = NAN;
    humidityBuffer[i]    = NAN;
  }

  Serial.println("===== Setup Complete. Starting Main Loop. =====");
  printLocalTime("[Time Check Post-Setup] ");
  Serial.printf("Free Heap Post-Setup: %u bytes\n", ESP.getFreeHeap());
}

void loop() {
  ArduinoCloud.update();
  yield();

  struct tm currentTime;
  bool timeOK = getLocalTime(&currentTime, 200);
  static int lastSecond = -1;

  if (!timeOK) {
    if (millis() - previousSampleMillis >= (unsigned long)SAMPLING_INTERVAL_SEC * 1000) {
      previousSampleMillis = millis();
      Serial.println("[Timing] NTP unavailable, using millis() for sampling.");
      collectSensorData();
      if (bufferIndex >= DATA_BUFFER_SIZE) {
        Serial.println("[Timing] Buffer full via millis-based sampling, attempting POST.");
        postAveragedDataToGoogleSheet();
      }
    }
    if (WiFi.status() == WL_CONNECTED && !getLocalTime(&currentTime, 0)) {
      delay(200);
    }
    return;
  }

  if (currentTime.tm_sec == 0 && lastSecond != 0) {
    // At exact minute boundary, check if start of new 15-min window
    if (currentTime.tm_min % REPORT_INTERVAL_MIN == 1) {
      bufferIndex = 0; // reset buffer at minute 1 past quarter
    }
    collectSensorData();
    // Post only at quarter boundaries (minutes 0,15,30,45)
    if (currentTime.tm_min % REPORT_INTERVAL_MIN == 0) {
      Serial.print("[Timing] Quarter-hour reached: posting averaged data. Minute: ");
      Serial.println(currentTime.tm_min);
      postAveragedDataToGoogleSheet();
    }
  }
  lastSecond = currentTime.tm_sec;
}

void initializeDHTSensor() {
  dht.begin();
  Serial.println("[DHT] Sensor initialized.");
  float t = dht.readTemperature(); float h = dht.readHumidity();
  if (isnan(t) || isnan(h)) Serial.println("[DHT] Warning: Failed initial read.");
  else Serial.printf("[DHT] Initial test: T: %.2f C, H: %.2f %%\n", t, h);
}

void initializeWiFi() {
  Serial.println("[WiFi] Starting WiFiManager...");
  wifiManager.setConnectTimeout(20);
  wifiManager.setConfigPortalTimeout(180);
  if (!wifiManager.autoConnect(AP_CONFIG_SSID, AP_CONFIG_PASS)) {
    Serial.println("[WiFi] WiFiManager failed or portal timed out. Restarting...");
    delay(3000); ESP.restart();
  }
  while (WiFi.status() != WL_CONNECTED) { delay(100); yield(); }
  delay(2000);
  Serial.print("[WiFi] Connected to: "); Serial.println(WiFi.SSID());
  Serial.print("[WiFi] IP Address: "); Serial.println(WiFi.localIP());
}

void initializeNTPTime() {
  Serial.print("[NTP] Configuring time: "); Serial.println(NTP_SERVER_URL);
  configTime(GMT_OFFSET_SECONDS, DAYLIGHT_OFFSET, NTP_SERVER_URL);
  Serial.print("[NTP] Waiting for sync...");
  struct tm timeinfo;
  unsigned long start = millis();
  while (!getLocalTime(&timeinfo, 100)) {
    if (millis() - start > NTP_TIMEOUT_MS) { Serial.println("\n[NTP] Timeout, proceeding without NTP."); return; }
    Serial.print("."); delay(NTP_RETRY_DELAY); yield();
  }
  Serial.println("\n[NTP] Time synchronized.");
  printLocalTime("[NTP] Current time: ");
}

void initializeArduinoCloud() {
  Serial.println("[Cloud] Initializing Arduino IoT Cloud...");
  Serial.println("[Cloud] Ensure SECRET_SSID/PASS match WiFiManager network.");
  initProperties();
  ArduinoCloud.begin(ArduinoIoTPreferredConnection);
  setDebugMessageLevel(3);
  ArduinoCloud.printDebugInfo();
  Serial.println("[Cloud] Connection attempt underway.");
  unsigned long start = millis();
  while (millis() - start < 2000) { ArduinoCloud.update(); yield(); }
}

void collectSensorData() {
  float h = dht.readHumidity();
  float t = dht.readTemperature();
  float sT = NAN, sH = NAN;
  if (!isnan(t) && t >= -40.0 && t <= 85.0) sT = t; else Serial.println("[Sensor] Invalid temp.");
  if (!isnan(h) && h >= 0.0 && h <= 100.0) sH = h; else Serial.println("[Sensor] Invalid hum.");
  if (!isnan(sH)) dhtHumi = sH;
  if (!isnan(sT)) dhtTemp = sT;
  temperatureBuffer[bufferIndex] = sT;
  humidityBuffer[bufferIndex]    = sH;
  printLocalTime("[Sensor] ");
  Serial.printf("Sample [%2d/%2d] | Raw T: %.2f, H: %.2f | Stored T: %.2f, H: %.2f\n",
                bufferIndex+1, DATA_BUFFER_SIZE, t, h, sT, sH);
  yield();
  bufferIndex++;
}

void postAveragedDataToGoogleSheet() {
  if (bufferIndex == 0) { Serial.println("[POST] No samples."); return; }
  float sumT=0, sumH=0;
  int vCount=0;
  Serial.println("[POST] --- Buffer for Averaging ---");
  for (int i=0; i<bufferIndex; i++) {
    Serial.printf("  Buffer[%2d]: T=%.2f, H=%.2f %s\n", i, temperatureBuffer[i], humidityBuffer[i],
                  (!isnan(temperatureBuffer[i]) && !isnan(humidityBuffer[i])) ? "(Valid)" : "(Invalid)");
    yield();
    if (!isnan(temperatureBuffer[i]) && !isnan(humidityBuffer[i])) { sumT+=temperatureBuffer[i]; sumH+=humidityBuffer[i]; vCount++; }
  }
  Serial.println("[POST] --- End Buffer ---"); yield();
  if (vCount == 0) { Serial.println("[POST] No valid, clearing buffer."); for (int i=0;i<bufferIndex;i++){temperatureBuffer[i]=NAN; humidityBuffer[i]=NAN;} bufferIndex=0; return; }
  float avgT=sumT/vCount, avgH=sumH/vCount;
  printLocalTime("[POST] ");
  Serial.printf("Avg from %d valid of %d -> T: %.2f, H: %.2f\n", vCount, bufferIndex, avgT, avgH);
  yield();
  if (WiFi.status()!=WL_CONNECTED) { Serial.println("[POST] WiFi gone, skip."); return; }
  HTTPClient http;
  httpsClient.setInsecure();
  Serial.print("[POST] Connecting to: "); Serial.println(GOOGLE_SHEET_POST_URL);
  bool initOk = http.begin(httpsClient, GOOGLE_SHEET_POST_URL);
  yield(); if (!initOk) { Serial.println("[POST] HTTP begin failed, keep buffer."); return; }
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.addHeader("Content-Type","application/json; charset=utf-8");
  http.setTimeout(HTTP_POST_TIMEOUT_MS);
  char payload[128]; snprintf(payload, sizeof(payload), "{\"temperature\":%.2f,\"humidity\":%.2f,\"mcu\":\"sputnik1\"}", avgT, avgH);
  Serial.print("[POST] Sending: "); Serial.println(payload); yield();
  int code=http.POST(payload);
  if (code>0) {
    Serial.printf("[POST] HTTP %d\n", code);
    if (http.getSize()>0) { String r=http.getString(); Serial.print("[POST] Resp: "); Serial.println(r); }
    if (code==HTTP_CODE_OK || (code>=300&&code<400)) { Serial.println("[POST] Success, clearing buffer."); for(int i=0;i<bufferIndex;i++){temperatureBuffer[i]=NAN; humidityBuffer[i]=NAN;} bufferIndex=0; }
    else { Serial.printf("[POST] Warn code %d, keep buffer.\n", code); }
  } else { Serial.printf("[POST] Fail: %s (Code: %d)\n", http.errorToString(code).c_str(), code); Serial.println("[POST] Keep buffer."); }
  http.end(); Serial.printf("Free Heap after POST: %u bytes\n", ESP.getFreeHeap()); yield();
}

void printLocalTime(const char* prefix) {
  struct tm ti;
  if (getLocalTime(&ti,0)) {
    Serial.print(prefix); char b[64]; strftime(b,64,"%A, %B %d %Y %H:%M:%S (%Z)", &ti); Serial.println(b);
  } else { Serial.print(prefix); Serial.println("Time N/A"); }
  yield();
}
