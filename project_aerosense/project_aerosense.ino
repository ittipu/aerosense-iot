/*
  Project AeroSense - Edge Node Firmware
  Board: ESP32 Dev Module
  Sensors: SCD30 (I2C Bus 0), TSL2561 (I2C Bus 1)
  
  Description: Reads precise environmental telemetry and publishes
  it as a JSON payload to a remote MQTT broker.
*/

#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <Wire.h>

// Sensor Libraries
#include "SparkFun_SCD30_Arduino_Library.h" // For SCD30
#include <Adafruit_Sensor.h>
#include <Adafruit_TSL2561_U.h>             // For TSL2561

// --- CONFIGURATION ---
// Wi-Fi Credentials
const char* ssid = "tipu_pc";
const char* password = "tipu1234@";

// MQTT Broker Settings (Reyax)
const char* mqtt_server = "iot.reyax.com";
const int mqtt_port = 1883;
const char* mqtt_user = "sRkG5DeaQT";
const char* mqtt_pass = "H5ZQydxeFM";
const char* mqtt_topic = "greenhouse/telemetry";

// Telemetry Settings
const unsigned long PUBLISH_INTERVAL = 5000; // Publish every 5 seconds
unsigned long lastPublishTime = 0;
const unsigned long WIFI_RETRY_INTERVAL = 5000;
const unsigned long MQTT_RETRY_INTERVAL = 5000;
unsigned long lastWiFiRetryTime = 0;
unsigned long lastMqttRetryTime = 0;

// --- I2C PIN DEFINITIONS ---
#define I2C_0_SDA 27
#define I2C_0_SCL 14

#define I2C_1_SDA 25
#define I2C_1_SCL 26

// --- GLOBAL OBJECTS ---
WiFiClient espClient;
PubSubClient client(espClient);

SCD30 scd30;
TwoWire I2C_Two = TwoWire(1); // Second I2C instance for TSL2561
Adafruit_TSL2561_Unified tsl2561 = Adafruit_TSL2561_Unified(TSL2561_ADDR_FLOAT, 12345);

// --- SETUP FUNCTIONS ---
void setup_wifi() {
  Serial.println();
  WiFi.mode(WIFI_STA);
  Serial.print("Connecting to Wi-Fi: ");
  Serial.println(ssid);
  WiFi.begin(ssid, password);
}

bool ensure_wifi_connected(unsigned long now) {
  if (WiFi.status() == WL_CONNECTED) {
    return true;
  }

  if (lastWiFiRetryTime == 0 || now - lastWiFiRetryTime >= WIFI_RETRY_INTERVAL) {
    lastWiFiRetryTime = now;
    Serial.println("Wi-Fi disconnected, retrying...");
    WiFi.disconnect();
    WiFi.begin(ssid, password);
  }

  return false;
}

bool ensure_mqtt_connected(unsigned long now) {
  if (client.connected()) {
    return true;
  }

  if (lastMqttRetryTime == 0 || now - lastMqttRetryTime >= MQTT_RETRY_INTERVAL) {
    lastMqttRetryTime = now;
    Serial.print("Attempting MQTT connection...");

    String clientId = "AeroSenseNode-";
    clientId += String(random(0xffff), HEX);

    if (client.connect(clientId.c_str(), mqtt_user, mqtt_pass)) {
      Serial.println("✅ Connected to Reyax Broker!");
      return true;
    }

    Serial.print("❌ Failed, rc=");
    Serial.println(client.state());
  }

  return false;
}

void setup() {
  Serial.begin(115200);

  // 1. Initialize the two I2C buses
  Wire.begin(I2C_0_SDA, I2C_0_SCL);      // Start Bus 0 (SCD30)
  I2C_Two.begin(I2C_1_SDA, I2C_1_SCL);   // Start Bus 1 (TSL2561)

  // 2. Initialize Wi-Fi & MQTT
  setup_wifi();
  client.setServer(mqtt_server, mqtt_port);

  // 3. Initialize SCD30
  if (scd30.begin(Wire) == false) {
    Serial.println("❌ SCD30 not detected. Please check wiring.");
    // while (1);
  }
  Serial.println("SCD30 Initialized.");

  // 4. Initialize TSL2561
  if (!tsl2561.begin(&I2C_Two)) {
    Serial.println("❌ TSL2561 not detected. Please check wiring.");
    // while (1);
  }
  tsl2561.enableAutoRange(true);            
  tsl2561.setIntegrationTime(TSL2561_INTEGRATIONTIME_13MS); 
  Serial.println("TSL2561 Initialized.");
}

// --- MAIN LOOP ---
void loop() {
  unsigned long now = millis();

  if (!ensure_wifi_connected(now)) {
    return;
  }

  static bool wifiConnectedLogged = false;
  if (!wifiConnectedLogged) {
    Serial.println("✅ Wi-Fi connected!");
    Serial.print("IP address: ");
    Serial.println(WiFi.localIP());
    wifiConnectedLogged = true;
  }

  if (!ensure_mqtt_connected(now)) {
    return;
  }
  client.loop();

  if (now - lastPublishTime >= PUBLISH_INTERVAL) {
    lastPublishTime = now;

    // --- 1. Read Sensors ---
    // Read SCD30
    float scd_co2 = 0.0, scd_t = 0.0, scd_h = 0.0;
    bool scd_ok = false;
    if (scd30.dataAvailable()) {
      scd_co2 = scd30.getCO2();
      scd_t = scd30.getTemperature();
      scd_h = scd30.getHumidity();
      scd_ok = true;
    }

    // Read TSL2561
    sensors_event_t event;
    tsl2561.getEvent(&event);
    float tsl_lux = 0.0;
    bool lux_ok = !isnan(event.light);
    if (lux_ok) {
      tsl_lux = event.light;
    }

    if (!scd_ok && !lux_ok) {
      Serial.println("⚠️ No valid sensor data this cycle, skipping publish.");
      return;
    }

    // --- 2. Construct JSON Payload ---
    JsonDocument doc; 
    
    // Add data to the JSON document
    doc["temp"] = scd_t;         // Simplified key name
    doc["humidity"] = scd_h;     // Simplified key name
    doc["co2"] = scd_co2;        // Simplified key name
    doc["lux"] = tsl_lux;        // Simplified key name
    doc["scd_ok"] = scd_ok;
    doc["lux_ok"] = lux_ok;

    // Serialize JSON to a string buffer
    char jsonBuffer[256];
    serializeJson(doc, jsonBuffer);

    // --- 3. Publish to MQTT ---
    Serial.print("Publishing Payload: ");
    Serial.println(jsonBuffer);
    
    if (client.publish(mqtt_topic, jsonBuffer)) {
      Serial.println("✅ Publish Success");
    } else {
      Serial.println("❌ Publish Failed");
    }
  }
}