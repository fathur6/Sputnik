/*
  ==============================================================================================================================================
  Project: sputnik IoT Sensor Module – WiFi-Manager Soft-AP Fallback PLUS 15-Minute Averaged Google Sheets POST (GMT+8)

  Author: Fathurrahman Lananan (Aman)
  Last Modified: 2025-06-05
  Revised by Gemini: 2025-06-05 // Current date is 2025-06-05
  ==============================================================================================================================================
*/

// --- Conditional includes for ESP32 (Primary target for this sketch) ---
#if defined(ESP32)
  #include <WiFi.h>
  #include <HTTPClient.h> // For Google Sheets POST
  #include <WiFiClientSecure.h> // For HTTPS
#else
  #error "Unsupported board: This sketch is optimized for ESP32."
#endif

#include <WiFiManager.h>      // https://github.com/tzapu/WiFiManager
#include "DHT.h"              // Adafruit DHT Library
#include <time.h>             // For NTP time
#include "thingProperties.h"  // Arduino IoT Cloud: binds dhtTemp/dhtHumi & Device Credentials

// --- Constants ---
// Hardware Pins
const int DHT_PIN   = 15;
const int DHT_TYPE  = DHT21; // AM2301

// WiFiManager SoftAP Credentials
const char* AP_CONFIG_SSID = "Sputnik-Setup";
const char* AP_CONFIG_PASS = "sputnik123";

// NTP Configuration
const char* NTP_SERVER_URL    = "pool.ntp.org";
const long  GMT_OFFSET_SECONDS  = 8 * 3600; // GMT+8
const int   DAYLIGHT_OFFSET_SECONDS = 0;
const int   NTP_SYNC_TIMEOUT_MS = 7000; // Increased timeout for initial NTP sync in setup
const int   NTP_RETRY_DELAY_MS  = 1000;

// Google Sheets POST Configuration
const char* GOOGLE_SHEET_POST_URL = "https://script.google.com/macros/s/AKfycbzcdQJjV7odm73Z5B0zyyVLhqCZZcaRRbypB4TE5a2vXtaNR5nmld4wX6vs4q3KisRfUw/exec";
const int   HTTP_POST_TIMEOUT_MS  = 10000; // Max time for HTTP POST (10s)

// Data Sampling & Buffering
const int DATA_BUFFER_SIZE       = 15; // Store 15 samples (for 15 minutes)
const int SAMPLING_INTERVAL_SEC  = 60; // Target 60 seconds between samples
const int REPORTING_INTERVAL_MIN = 15; // Post averaged data every 15 minutes

// --- Global Objects ---
DHT dht(DHT_PIN, DHT_TYPE);
WiFiManager wifiManager;
WiFiClientSecure httpsClient; // Secure client for Google Sheets POST

// --- Global Variables for Data and Timing ---
unsigned long previousSampleMillis = 0; // For non-blocking timed sampling
float temperatureBuffer[DATA_BUFFER_SIZE];
float humidityBuffer[DATA_BUFFER_SIZE];
int   bufferIndex = 0;

// --- Forward declarations ---
void initializeWiFi();
void initializeNTPTime();
void initializeDHTSensor();
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
  Serial.println("Firmware Version: 2025-06-05-RevH"); 
  Serial.printf("ESP32 Chip Revision: %d\n", ESP.getChipRevision());
  Serial.printf("Initial Free Heap: %u bytes\n", ESP.getFreeHeap());

  initializeDHTSensor();      
  initializeWiFi();           
  initializeNTPTime();        
  initializeArduinoCloud();   

  for (int i = 0; i < DATA_BUFFER_SIZE; ++i) {
    temperatureBuffer[i] = NAN;
    humidityBuffer[i] = NAN;
  }

  Serial.println("===== Setup Complete. Starting Main Loop. =====");
  printLocalTime("[Time Check Post-Setup]");
  Serial.printf("Free Heap Post-Setup: %u bytes\n", ESP.getFreeHeap());
}

// ==============================================================================================================================================
// MAIN LOOP
// ==============================================================================================================================================
void loop() {
  ArduinoCloud.update(); 

  struct tm currentTime;
  if (!getLocalTime(&currentTime, 10)) { 
    if (millis() - previousSampleMillis >= (unsigned long)SAMPLING_INTERVAL_SEC * 1000) {
      previousSampleMillis = millis(); 
      Serial.println("[Timing] NTP time not available, using millis() for 1-minute interval.");
      collectSensorData(); 
      if (bufferIndex >= DATA_BUFFER_SIZE) { 
        Serial.println("[Timing] Buffer full via millis-based sampling, attempting post.");
        postAveragedDataToGoogleSheet(); 
      }
    }
    if (WiFi.status() == WL_CONNECTED && !getLocalTime(&currentTime, 0)) { 
        delay(200); 
    }
    return; 
  }

  if (currentTime.tm_sec == 0 && 
      (millis() - previousSampleMillis >= ((unsigned long)SAMPLING_INTERVAL_SEC * 1000 - 5000)) ) { 
    previousSampleMillis = millis(); 
    collectSensorData();
    if (currentTime.tm_min % REPORTING_INTERVAL_MIN == 0) {
      Serial.print("[Timing] NTP-Aligned: Triggering ");
      Serial.print(REPORTING_INTERVAL_MIN);
      Serial.print("-minute report. Current minute: ");
      Serial.println(currentTime.tm_min);
      postAveragedDataToGoogleSheet();
    }
  }
}


// ==============================================================================================================================================
// INITIALIZATION HELPER FUNCTIONS
// ==============================================================================================================================================
void initializeWiFi() {
  Serial.println("[WiFi] Initializing WiFiManager...");
  wifiManager.setConnectTimeout(20); 
  wifiManager.setConfigPortalTimeout(180); 

  if (!wifiManager.autoConnect(AP_CONFIG_SSID, AP_CONFIG_PASS)) {
    Serial.println("[WiFi] Failed to connect via WiFiManager and/or portal timed out. Restarting ESP32...");
    delay(3000);
    ESP.restart(); 
  }
  Serial.print("[WiFi] Connected via WiFiManager to: "); Serial.println(WiFi.SSID());
  Serial.print("[WiFi] IP Address: "); Serial.println(WiFi.localIP());
}

void initializeNTPTime() {
  Serial.print("[NTP] Configuring time with server: "); Serial.println(NTP_SERVER_URL);
  configTime(GMT_OFFSET_SECONDS, DAYLIGHT_OFFSET_SECONDS, NTP_SERVER_URL);

  Serial.print("[NTP] Waiting for initial time synchronization...");
  struct tm timeinfo;
  unsigned long startAttemptTime = millis();
  while (!getLocalTime(&timeinfo, 100)) { 
    if (millis() - startAttemptTime > NTP_SYNC_TIMEOUT_MS) {
      Serial.println("\n[NTP] Failed to obtain time within timeout. Continuing with potentially unsynced time.");
      return;
    }
    Serial.print(".");
    delay(NTP_RETRY_DELAY_MS / 5); 
  }
  Serial.println("\n[NTP] Time synchronized successfully.");
  printLocalTime("[NTP] Current time after sync");
}

void initializeDHTSensor() {
  dht.begin();
  Serial.println("[DHT] Sensor initialized.");
  float initial_temp = dht.readTemperature();
  float initial_humi = dht.readHumidity();
  if (isnan(initial_temp) || isnan(initial_humi)) {
    Serial.println("[DHT] Warning: Failed to read from DHT sensor during initialization.");
  } else {
    Serial.printf("[DHT] Initial test reading: T: %.2f °C, H: %.2f %%\n", initial_temp, initial_humi);
  }
}

void initializeArduinoCloud() {
  Serial.println("[Cloud] Initializing Arduino IoT Cloud connection...");
  Serial.println("[Cloud] IMPORTANT: Ensure SECRET_SSID and SECRET_OPTIONAL_PASS in 'arduino_secrets.h'");
  Serial.println("[Cloud] MATCH the WiFi network connected by WiFiManager (e.g., AaelDedekWifi) for stable cloud connectivity!");
  
  initProperties(); 
  ArduinoCloud.begin(ArduinoIoTPreferredConnection); 
  setDebugMessageLevel(3); 
  ArduinoCloud.printDebugInfo(); 
  Serial.println("[Cloud] Arduino IoT Cloud setup process initiated. Check subsequent logs for connection status.");
  delay(1000); 
}

// ==============================================================================================================================================
// DATA COLLECTION & POSTING FUNCTIONS
// ==============================================================================================================================================
void collectSensorData() {
  float humidity    = dht.readHumidity();
  float temperature = dht.readTemperature(); 

  // Temporary variables for what will be stored, after validation
  float stored_temp = NAN;
  float stored_humi = NAN;

  if (isnan(temperature) || temperature < -40.0 || temperature > 85.0) { 
    Serial.println("[Sensor] Invalid temperature reading!");
    stored_temp = NAN;
  } else {
    stored_temp = temperature;
  }

  if (isnan(humidity) || humidity < 0.0 || humidity > 100.0) {
    Serial.println("[Sensor] Invalid humidity reading!");
    stored_humi = NAN;
  } else {
    stored_humi = humidity;
  }

  // Store (potentially NAN) values
  temperatureBuffer[bufferIndex] = stored_temp;
  humidityBuffer[bufferIndex] = stored_humi;
  
  printLocalTime("[Sensor] ");
  Serial.printf("Sample [%2d/%2d] | Raw T: %6.2f C, H: %5.2f %% | Stored T: %6.2f, H: %5.2f\n",
                bufferIndex + 1, DATA_BUFFER_SIZE,
                temperature, humidity,
                stored_temp, stored_humi);

  // Update Arduino IoT Cloud variables only if readings are valid
  if (!isnan(stored_humi)) {
    dhtHumi = stored_humi; 
  }
  if (!isnan(stored_temp)) {
    dhtTemp = stored_temp;
  }
  
  bufferIndex++;
  // No preemptive post here; let the main loop's 15-minute interval or buffer full (in fallback) handle it.
}

void postAveragedDataToGoogleSheet() {
  if (bufferIndex == 0) { 
    Serial.println("[POST] No new samples in buffer to average and post.");
    return; 
  }

  float sumTemperature = 0.0f, sumHumidity = 0.0f;
  int validReadingsCount = 0;

  Serial.println("[POST] --- Values in Buffer for Averaging (Processing " + String(bufferIndex) + " collected samples) ---");
  for (int i = 0; i < bufferIndex; ++i) { 
    Serial.printf("  Buffer[%2d]: Temp = %6.2f, Humid = %5.2f %s\n",
                  i,
                  temperatureBuffer[i],
                  humidityBuffer[i],
                  (!isnan(temperatureBuffer[i]) && !isnan(humidityBuffer[i])) ? "(Valid Pair)" : "(Invalid Pair or NAN)"
                  );
    if (!isnan(temperatureBuffer[i]) && !isnan(humidityBuffer[i])) { // Pair must be valid
      sumTemperature += temperatureBuffer[i];
      sumHumidity += humidityBuffer[i];
      validReadingsCount++;
    }
  }
  Serial.println("[POST] --- End of Buffer Values ---");

  if (validReadingsCount == 0) {
    Serial.println("[POST] No valid sensor reading pairs in the buffer to average.");
    // Clear buffer as these invalid samples have been processed (logged)
    for (int i = 0; i < bufferIndex; ++i) { 
        temperatureBuffer[i] = NAN; humidityBuffer[i] = NAN;
    }
    bufferIndex = 0; 
    return;
  }

  float averageTemperature = sumTemperature / validReadingsCount;
  float averageHumidity    = sumHumidity / validReadingsCount;
  printLocalTime("[POST] ");
  Serial.printf("Calculated AVG (from %d valid pairs out of %d samples) -> T: %.2f C | H: %.2f %%\n", 
                validReadingsCount, bufferIndex, averageTemperature, averageHumidity);

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[POST] WiFi not connected. Skipping Google Sheet POST. Data remains in buffer.");
    return; // Data will be attempted next interval
  }

  HTTPClient httpClient;
  httpsClient.setInsecure(); // IMPORTANT: For production, consider CA certs.

  Serial.print("[POST] Connecting to Google Sheets URL: "); Serial.println(GOOGLE_SHEET_POST_URL);
  
  bool httpBeginSuccess = httpClient.begin(httpsClient, GOOGLE_SHEET_POST_URL);

  if (httpBeginSuccess) {
    httpClient.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    httpClient.addHeader("Content-Type", "application/json; charset=utf-8");
    httpClient.setTimeout(HTTP_POST_TIMEOUT_MS); 

    char jsonPayload[128]; 
    snprintf(jsonPayload, sizeof(jsonPayload),
             "{\"temperature\":%.2f,\"humidity\":%.2f,\"mcu\":\"sputnik1\"}",
             averageTemperature, averageHumidity);

    Serial.print("[POST] Sending JSON Payload: "); Serial.println(jsonPayload);
    int httpResponseCode = httpClient.POST(jsonPayload);

    if (httpResponseCode > 0) {
      Serial.printf("[POST] HTTP Response Code: %d\n", httpResponseCode);
      String responsePayload = "";
      // Check if there's a response body before trying to read it
      if (httpClient.getSize() > 0) {
        responsePayload = httpClient.getString();
      }
      Serial.print("[POST] HTTP Response Body: "); Serial.println(responsePayload);
      
      // Consider HTTP_CODE_OK (200) or redirects (30X) as success for POST to Apps Script
      if (httpResponseCode == HTTP_CODE_OK || (httpResponseCode >= 300 && httpResponseCode < 400)) { 
         Serial.println("[POST] Successfully sent data to Google Sheet.");
         // Clear buffer ONLY after successful post
         for (int i = 0; i < bufferIndex; ++i) { 
            temperatureBuffer[i] = NAN; humidityBuffer[i] = NAN;
         }
         bufferIndex = 0; 
      } else {
        Serial.println("[POST] Warning: POST to server returned HTTP " + String(httpResponseCode) + ". Data remains in buffer.");
      }
    } else {
      Serial.printf("[POST] HTTP POST failed, Error: %s (Code: %d)\n", httpClient.errorToString(httpResponseCode).c_str(), httpResponseCode);
      Serial.println("[POST] Data remains in buffer due to POST failure.");
    }
    httpClient.end();
  } else {
    Serial.println("[POST] Failed to initialize HTTP connection to Google Sheets URL. Data remains in buffer.");
  }
  Serial.printf("Free Heap Post-POST attempt: %u bytes\n", ESP.getFreeHeap());
}


// ==============================================================================================================================================
// UTILITY FUNCTIONS
// ==============================================================================================================================================
void printLocalTime(const char* prefix) {
  struct tm timeinfo;
  if (getLocalTime(&timeinfo, 0)) { 
    Serial.print(prefix);
    char timeStr[64];
    strftime(timeStr, sizeof(timeStr), "%A, %B %d %Y %H:%M:%S (%Z)", &timeinfo); // Added %Z for timezone if available
    Serial.print(timeStr);
    Serial.println();
  } else {
    Serial.print(prefix);
    Serial.println("Time not available.");
  }
}
