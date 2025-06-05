#include "arduino_secrets.h"
#include "thingProperties.h"
#include "DHT.h"
#include <time.h>

#define DHTPIN 15     // DHT connected to pin 15
#define DHTTYPE DHT21 // DHT 21 (AM2301)

DHT dht(DHTPIN, DHTTYPE);
unsigned long previousMillis = 0;
const char* ntpServer = "pool.ntp.org";

void setup() {
  Serial.begin(9600);
  delay(1500);
  
  // Initialize DHT sensor
  dht.begin();
  
  // Initialize NTP
  configTime(0, 0, ntpServer);
  
  // Defined in thingProperties.h
  initProperties();
  
  // Connect to Arduino IoT Cloud
  ArduinoCloud.begin(ArduinoIoTPreferredConnection);
  setDebugMessageLevel(2);
  ArduinoCloud.printDebugInfo();
}

void loop() {
  ArduinoCloud.update();
  
  struct tm timeinfo;
  if (getLocalTime(&timeinfo)) {
    // Check if we're at the start of a minute
    if (timeinfo.tm_sec == 0 || millis() - previousMillis >= 60000) {
      previousMillis = millis();
      
      // Read sensor data
      dhtHumi = dht.readHumidity();
      dhtTemp = dht.readTemperature();
      
      // Print values to Serial
      Serial.print("Time: ");
      Serial.print(timeinfo.tm_hour);
      Serial.print(":");
      Serial.print(timeinfo.tm_min);
      Serial.print(":");
      Serial.println(timeinfo.tm_sec);
      Serial.print("Temperature: ");
      Serial.println(dhtTemp);
      Serial.print("Humidity: ");
      Serial.println(dhtHumi);
    }
  }
}