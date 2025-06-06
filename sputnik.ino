/*
  ==============================================================================================================================================
  Project: sputnik IoT Sensor Module – WiFi‑Manager Soft‑AP Fallback PLUS 15‑Minute Averaged Google Sheets POST with NTP Fallback (GMT+8)

  Author: Fathurrahman Lananan (Aman)
  Last Modified: 2025‑06‑05

  ==============================================================================================================================================
*/

// --- Conditional includes for ESP32 (Primary target) ---
#if defined(ESP32)
  #include <WiFi.h>
  #include <HTTPClient.h>           // For Google Sheets POST
  #include <WiFiClientSecure.h>     // For HTTPS
#else
  #error "Unsupported board: This sketch is optimized for ESP32."
#endif

#include <WiFiManager.h>             // Soft‑AP captive portal library
#include "DHT.h"                     // Adafruit DHT sensor library
#include <time.h>                     // NTP time functions
#include "thingProperties.h"         // Arduino IoT Cloud: binds dhtTemp/dhtHumi & device credentials

// --- Constants ---
// Hardware Pins
const int DHT_PIN             = 15;
const int DHT_TYPE            = DHT21; // AM2301 sensor type

// WiFiManager SoftAP credentials
const char* AP_CONFIG_SSID    = "Sputnik-Setup";
const char* AP_CONFIG_PASS    = "sputnik123";

// NTP configuration
const char* NTP_SERVER_URL           = "pool.ntp.org";
const long  GMT_OFFSET_SECONDS       = 8 * 3600;    // GMT+8
const int   DAYLIGHT_OFFSET_SECONDS  = 0;
const int   NTP_SYNC_TIMEOUT_MS      = 7000;        // 7 s max wait for initial sync
const int   NTP_RETRY_DELAY_MS       = 500;         // 0.5 s between NTP attempts

// Google Sheets POST configuration
const char* GOOGLE_SHEET_POST_URL    = "https://script.google.com/macros/s/AKfycbzcdQJjV7odm73Z5B0zyyVLhqCZZcaRRbypB4TE5a2vXtaNR5nmld4wX6vs4q3KisRfUw/exec";
const int   HTTP_POST_TIMEOUT_MS     = 10000;       // 10 s timeout for HTTP POST

// Data sampling & buffering
const int DATA_BUFFER_SIZE            = 15;         // 15 samples = 15 minutes
const int SAMPLING_INTERVAL_SEC       = 60;         // 60 s between samples
const int REPORTING_INTERVAL_MIN      = 15;         // Post every 15 minutes

// --- Globals ---
DHT dht(DHT_PIN, DHT_TYPE);
WiFiManager wifiManager;
WiFiClientSecure httpsClient;            // Secure HTTP client for Google Sheets POST

unsigned long previousSampleMillis = 0;  // For non‑blocking sampling using millis()
float temperatureBuffer[DATA_BUFFER_SIZE];
float humidityBuffer[DATA_BUFFER_SIZE];
int   bufferIndex            = 0;

// --- Forward declarations ---
void initializeDHTSensor();
void initializeWiFi();
void initializeNTPTime();
void initializeArduinoCloud();
void collectSensorData();
void postAveragedDataToGoogleSheet();
void printLocalTime(const char* prefix = "");

// ==============================================================================================================================================
// SETUP FUNCTION
// ==============================================================================================================================================
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n\n===== Sputnik IoT Module Initializing =====");

  initializeDHTSensor();        
  initializeWiFi();             
  initializeNTPTime();          
  initializeArduinoCloud();    

  // Initialize data buffers to NAN
  for (int i = 0; i < DATA_BUFFER_SIZE; ++i) {
    temperatureBuffer[i] = NAN;
    humidityBuffer[i]    = NAN;
  }

  Serial.println("===== Setup Complete. Starting Main Loop. =====");
  printLocalTime("[Time Check Post-Setup] ");
  Serial.printf("Free Heap Post-Setup: %u bytes\n", ESP.getFreeHeap());
}

// ==============================================================================================================================================
// MAIN LOOP
// ==============================================================================================================================================
void loop() {
  ArduinoCloud.update(); 
  yield();  // Feed the watchdog

  struct tm currentTime;
  bool timeOK = getLocalTime(&currentTime, 200);
  static int lastSecond = -1;

  if (!timeOK) {
    // If NTP not available, fall back to millis-based sampling once per interval
    if (millis() - previousSampleMillis >= (unsigned long)SAMPLING_INTERVAL_SEC * 1000) {
      previousSampleMillis = millis();
      Serial.println("[Timing] NTP time not available, using millis() for sampling.");
      collectSensorData();
      if (bufferIndex >= DATA_BUFFER_SIZE) {
        Serial.println("[Timing] Buffer full via millis-based sampling, attempting POST.");
        postAveragedDataToGoogleSheet();
      }
    }
    // Attempt quick re-sync without blocking too long
    if (WiFi.status() == WL_CONNECTED && !getLocalTime(&currentTime, 0)) {
      delay(200);
    }
    return;
  }

  // Only sample once per NTP‑aligned minute: when tm_sec==0 AND lastSecond != 0
  if (currentTime.tm_sec == 0 && lastSecond != 0) {
    previousSampleMillis = millis();
    collectSensorData();
    // When minute divisible by 15, POST averaged data
    if (currentTime.tm_min % REPORTING_INTERVAL_MIN == 0) {
      Serial.print("[Timing] NTP-Aligned: Triggering ");
      Serial.print(REPORTING_INTERVAL_MIN);
      Serial.print("-minute report. Current minute: ");
      Serial.println(currentTime.tm_min);
      postAveragedDataToGoogleSheet();
    }
  }

  lastSecond = currentTime.tm_sec;
}

// ==============================================================================================================================================
// INITIALIZATION HELPERS
// ==============================================================================================================================================
void initializeDHTSensor() {
  dht.begin();
  Serial.println("[DHT] Sensor initialized.");
  float initTemp = dht.readTemperature();
  float initHumi = dht.readHumidity();
  if (isnan(initTemp) || isnan(initHumi)) {
    Serial.println("[DHT] Warning: Failed to read from DHT sensor during initialization.");
  } else {
    Serial.printf("[DHT] Initial test: T: %.2f C, H: %.2f %%\n", initTemp, initHumi);
  }
}

void initializeWiFi() {
  Serial.println("[WiFi] Starting WiFiManager...");
  wifiManager.setConnectTimeout(20);          // 20 s to connect to saved creds
  wifiManager.setConfigPortalTimeout(180);     // 3 min portal timeout

  if (!wifiManager.autoConnect(AP_CONFIG_SSID, AP_CONFIG_PASS)) {
    Serial.println("[WiFi] WiFiManager failed or portal timed out. Restarting...");
    delay(3000);
    ESP.restart();
  }
  // Wait until connected
  while (WiFi.status() != WL_CONNECTED) {
    delay(100);
    yield();
  }
  delay(2000); // Extra stabilization delay
  Serial.print("[WiFi] Connected via WiFiManager to: "); Serial.println(WiFi.SSID());
  Serial.print("[WiFi] IP Address: "); Serial.println(WiFi.localIP());
}

void initializeNTPTime() {
  Serial.print("[NTP] Configuring time with server: "); Serial.println(NTP_SERVER_URL);
  configTime(GMT_OFFSET_SECONDS, DAYLIGHT_OFFSET_SECONDS, NTP_SERVER_URL);

  Serial.print("[NTP] Waiting for initial time sync...");
  struct tm timeinfo;
  unsigned long startAttemptTime = millis();
  while (!getLocalTime(&timeinfo, 100)) {
    if (millis() - startAttemptTime > NTP_SYNC_TIMEOUT_MS) {
      Serial.println("\n[NTP] Failed to sync within timeout. Continuing without NTP.");
      return;
    }
    Serial.print(".");
    delay(NTP_RETRY_DELAY_MS);
    yield();
  }
  Serial.println("\n[NTP] Time synchronized.");
  printLocalTime("[NTP] Current time: ");
}

void initializeArduinoCloud() {
  Serial.println("[Cloud] Initializing Arduino IoT Cloud...");
  Serial.println("[Cloud] Ensure SECRET_SSID and SECRET_PASS in arduino_secrets.h match current Wi-Fi.");

  initProperties();                   // Auto-generated: binds dhtTemp/dhtHumi
  ArduinoCloud.begin(ArduinoIoTPreferredConnection);
  setDebugMessageLevel(3);
  ArduinoCloud.printDebugInfo();
  Serial.println("[Cloud] Arduino IoT Cloud connection attempt underway.");
  // Allow some time for MQTT handshake
  unsigned long cloudStart = millis();
  while (millis() - cloudStart < 2000) {
    ArduinoCloud.update();
    yield();
  }
}

// ==============================================================================================================================================
// DATA COLLECTION & POSTING
// ==============================================================================================================================================
void collectSensorData() {
  float humidity    = dht.readHumidity();
  float temperature = dht.readTemperature();

  float storedTemp = NAN;
  float storedHumi = NAN;

  // Validate temperature
  if (!isnan(temperature) && temperature >= -40.0 && temperature <= 85.0) {
    storedTemp = temperature;
  } else {
    Serial.println("[Sensor] Invalid temperature reading, storing NAN.");
  }

  // Validate humidity
  if (!isnan(humidity) && humidity >= 0.0 && humidity <= 100.0) {
    storedHumi = humidity;
  } else {
    Serial.println("[Sensor] Invalid humidity reading, storing NAN.");
  }

  // Update IoT Cloud variables only if valid
  if (!isnan(storedHumi)) { dhtHumi = storedHumi; }
  if (!isnan(storedTemp)) { dhtTemp = storedTemp; }

  temperatureBuffer[bufferIndex] = storedTemp;
  humidityBuffer[bufferIndex]    = storedHumi;

  printLocalTime("[Sensor] ");
  Serial.printf("Sample [%2d/%2d] | Raw T: %6.2f C, H: %5.2f %% | Stored T: %6.2f, H: %5.2f\n",
                bufferIndex + 1, DATA_BUFFER_SIZE,
                temperature, humidity,
                storedTemp, storedHumi);
  yield();
  bufferIndex++;
}

void postAveragedDataToGoogleSheet() {
  if (bufferIndex == 0) {
    Serial.println("[POST] No samples to average.");
    return;
  }

  float sumTemp = 0.0f, sumHumi = 0.0f;
  int validCount = 0;
  Serial.println("[POST] --- Buffer Contents for Averaging ---");

  for (int i = 0; i < bufferIndex; ++i) {
    Serial.printf("  Buffer[%2d]: T=%6.2f, H=%5.2f %s\n",
                  i,
                  temperatureBuffer[i],
                  humidityBuffer[i],
                  (!isnan(temperatureBuffer[i]) && !isnan(humidityBuffer[i])) ? "(Valid)" : "(Invalid)"
                  );
    yield();
    if (!isnan(temperatureBuffer[i]) && !isnan(humidityBuffer[i])) {
      sumTemp += temperatureBuffer[i];
      sumHumi += humidityBuffer[i];
      validCount++;
    }
  }
  Serial.println("[POST] --- End Buffer ---");
  yield();

  if (validCount == 0) {
    Serial.println("[POST] No valid pairs, clearing buffer.");
    for (int i = 0; i < bufferIndex; ++i) {
      temperatureBuffer[i] = NAN;
      humidityBuffer[i]    = NAN;
    }
    bufferIndex = 0;
    return;
  }

  float avgTemp = sumTemp / validCount;
  float avgHumi = sumHumi / validCount;
  printLocalTime("[POST] ");
  Serial.printf("Calculated AVG from %d valid of %d samples -> T: %.2f C | H: %.2f %%\n", validCount, bufferIndex, avgTemp, avgHumi);
  yield();

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[POST] WiFi disconnected, skipping POST.");
    return;
  }

  HTTPClient httpClient;
  httpsClient.setInsecure();  // For simplicity; replace with CA cert in production

  Serial.print("[POST] Connecting to: "); Serial.println(GOOGLE_SHEET_POST_URL);
  bool httpInit = httpClient.begin(httpsClient, GOOGLE_SHEET_POST_URL);
  yield();

  if (!httpInit) {
    Serial.println("[POST] HTTP begin() failed, data remains in buffer.");
    return;
  }

  httpClient.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  httpClient.addHeader("Content-Type", "application/json; charset=utf-8");
  httpClient.setTimeout(HTTP_POST_TIMEOUT_MS);

  char payload[128];
  snprintf(payload, sizeof(payload), "{\"temperature\":%.2f,\"humidity\":%.2f,\"mcu\":\"sputnik1\"}", avgTemp, avgHumi);

  Serial.print("[POST] Sending JSON: "); Serial.println(payload);
  yield();
  int statusCode = httpClient.POST(payload);

  if (statusCode > 0) {
    Serial.printf("[POST] HTTP Status: %d\n", statusCode);
    if (httpClient.getSize() > 0) {
      String resp = httpClient.getString();
      Serial.print("[POST] Response: "); Serial.println(resp);
    }
    if (statusCode == HTTP_CODE_OK || (statusCode >= 300 && statusCode < 400)) {
      Serial.println("[POST] Success, clearing buffer.");
      for (int i = 0; i < bufferIndex; ++i) {
        temperatureBuffer[i] = NAN;
        humidityBuffer[i]    = NAN;
      }
      bufferIndex = 0;
    } else {
      Serial.printf("[POST] Warning: Received code %d, keeping buffer for retry.\n", statusCode);
    }
  } else {
    Serial.printf("[POST] Failed: %s (Code: %d)\n", httpClient.errorToString(statusCode).c_str(), statusCode);
    Serial.println("[POST] Data remains in buffer for retry.");
  }
  httpClient.end();

  Serial.printf("Free Heap after POST attempt: %u bytes\n", ESP.getFreeHeap());
  yield();
}

// ==============================================================================================================================================
// UTILITY FUNCTIONS
// ==============================================================================================================================================
void printLocalTime(const char* prefix) {
  struct tm timeinfo;
  if (getLocalTime(&timeinfo, 0)) {
    Serial.print(prefix);
    char buf[64];
    strftime(buf, sizeof(buf), "%A, %B %d %Y %H:%M:%S (%Z)", &timeinfo);
    Serial.println(buf);
  } else {
    Serial.print(prefix);
    Serial.println("Time not available.");
  }
  yield();
}
