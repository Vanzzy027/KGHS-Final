/*
 * Kangaru Girls High School - ESP32 Smart Greenhouse Controller
 * Cloud-Connected via Firebase RTDB with Robust Error Handling
 * Hardware:
 * - DHT22 (Pin 4)
 * - Analog Moisture Sensor (Pin 34)
 * - Relay 1: Pump (Pin 25)
 * - Relay 2: Fan (Pin 26) - ON > 22°C
 * - Relay 3: Warm Lights (Pin 27) - ON < 20°C
 * - Humidifier Trigger (Pin 12) - Pulses ON < 45%, Pulses OFF >= 50%
 */

#include <WiFi.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <DHTesp.h>
#include <WiFiManager.h>
#include <Firebase_ESP_Client.h>
#include "addons/TokenHelper.h"
#include "addons/RTDBHelper.h"

// ============ FIREBASE CONFIGURATION (Update with your project details) ============
#define FIREBASE_API_KEY "AIzaSy..."          // Your API key
#define FIREBASE_DATABASE_URL "https://...firebaseio.com/" // Your RTDB URL
#define USER_EMAIL "user@example.com"          // Firebase Auth email
#define USER_PASSWORD "password"                // Firebase Auth password

// ============ PIN CONFIGURATION ============
#define ADC_PIN 34
#define DHT_PIN 4
#define PUMP_RELAY_PIN 25
#define FAN_RELAY_PIN 26
#define LIGHT_RELAY_PIN 27
#define HUMIDIFIER_PIN 12

// ============ SYSTEM STATE & CONFIG ============
struct Config {
  float temp_high = 22.0;       // Fan ON above this
  float temp_low = 20.0;         // Lights ON below this
  float hum_low = 45.0;           // Humidifier ON below this
  float hum_high = 50.0;          // Humidifier OFF above this
  float moisture_dry = 45.0;      // Pump ON below this
  float moisture_target = 60.0;   // Pump OFF above this
  int adc_wet = 1500;
  int adc_dry = 4095;
} sysConfig;

struct State {
  float temp = 0.0;
  float humidity = 0.0;
  float moisture = 0.0;
  bool pump_on = false;
  bool fan_on = false;
  bool lights_on = false;
  bool humidifier_is_on = false;
} sysState;

SemaphoreHandle_t xMutex;
DHTesp dht;

// Firebase objects
FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig fbConfig;
bool firebaseReady = false;          // Becomes true after successful sign-in
unsigned long lastAuthAttempt = 0;
const unsigned long authRetryInterval = 30000; // 30 seconds

// ============ HELPER FUNCTIONS ============
float mapMoisture(int raw) {
  if (raw <= sysConfig.adc_wet) return 100.0;
  if (raw >= sysConfig.adc_dry) return 0.0;
  return 100.0 * (1.0 - (float)(raw - sysConfig.adc_wet) / (float)(sysConfig.adc_dry - sysConfig.adc_wet));
}

void pulseHumidifier() {
  digitalWrite(HUMIDIFIER_PIN, HIGH);
  vTaskDelay(pdMS_TO_TICKS(500));
  digitalWrite(HUMIDIFIER_PIN, LOW);
}

// Save config to LittleFS
void saveConfig() {
  File configFile = LittleFS.open("/config.json", FILE_WRITE);
  if (!configFile) {
    Serial.println("Failed to open config file for writing");
    return;
  }
  StaticJsonDocument<256> doc;
  doc["temp_high"] = sysConfig.temp_high;
  doc["temp_low"] = sysConfig.temp_low;
  doc["hum_low"] = sysConfig.hum_low;
  doc["hum_high"] = sysConfig.hum_high;
  doc["moisture_dry"] = sysConfig.moisture_dry;
  doc["moisture_target"] = sysConfig.moisture_target;
  doc["adc_wet"] = sysConfig.adc_wet;
  doc["adc_dry"] = sysConfig.adc_dry;
  if (serializeJson(doc, configFile) == 0) {
    Serial.println("Failed to write config file");
  }
  configFile.close();
}

// Load config from LittleFS
void loadConfig() {
  if (!LittleFS.begin(true)) {
    Serial.println("LittleFS mount failed");
    return;
  }
  File configFile = LittleFS.open("/config.json", FILE_READ);
  if (!configFile) {
    Serial.println("No config file, using defaults");
    saveConfig(); // create default
    return;
  }
  StaticJsonDocument<256> doc;
  DeserializationError error = deserializeJson(doc, configFile);
  if (error) {
    Serial.println("Failed to parse config file, using defaults");
    saveConfig();
    return;
  }
  sysConfig.temp_high = doc["temp_high"] | sysConfig.temp_high;
  sysConfig.temp_low = doc["temp_low"] | sysConfig.temp_low;
  sysConfig.hum_low = doc["hum_low"] | sysConfig.hum_low;
  sysConfig.hum_high = doc["hum_high"] | sysConfig.hum_high;
  sysConfig.moisture_dry = doc["moisture_dry"] | sysConfig.moisture_dry;
  sysConfig.moisture_target = doc["moisture_target"] | sysConfig.moisture_target;
  sysConfig.adc_wet = doc["adc_wet"] | sysConfig.adc_wet;
  sysConfig.adc_dry = doc["adc_dry"] | sysConfig.adc_dry;
  configFile.close();
  Serial.println("Config loaded from LittleFS");
}

// ============ TASKS ============

// 1. Read Sensors
void sensorTask(void *pvParameters) {
  while (true) {
    float t = dht.getTemperature();
    float h = dht.getHumidity();

    long sum = 0;
    for (int i = 0; i < 10; i++) { sum += analogRead(ADC_PIN); vTaskDelay(5); }
    int raw_adc = sum / 10;
    float m = mapMoisture(raw_adc);

    if (xSemaphoreTake(xMutex, portMAX_DELAY)) {
      if (!isnan(t)) sysState.temp = t;
      if (!isnan(h)) sysState.humidity = h;
      sysState.moisture = m;
      xSemaphoreGive(xMutex);
    }
    vTaskDelay(pdMS_TO_TICKS(2000));
  }
}

// 2. Control Logic (Relays & Humidifier)
void controlTask(void *pvParameters) {
  while (true) {
    if (xSemaphoreTake(xMutex, portMAX_DELAY)) {
      // Fan Logic
      sysState.fan_on = (sysState.temp > sysConfig.temp_high);
      digitalWrite(FAN_RELAY_PIN, sysState.fan_on ? HIGH : LOW);

      // Light Logic
      sysState.lights_on = (sysState.temp < sysConfig.temp_low);
      digitalWrite(LIGHT_RELAY_PIN, sysState.lights_on ? HIGH : LOW);

      // Pump Logic
      if (sysState.moisture < sysConfig.moisture_dry) sysState.pump_on = true;
      else if (sysState.moisture > sysConfig.moisture_target) sysState.pump_on = false;
      digitalWrite(PUMP_RELAY_PIN, sysState.pump_on ? HIGH : LOW);

      // Humidifier Pulse Logic
      if (sysState.humidity < sysConfig.hum_low && !sysState.humidifier_is_on) {
        pulseHumidifier();
        sysState.humidifier_is_on = true;
      } else if (sysState.humidity >= sysConfig.hum_high && sysState.humidifier_is_on) {
        pulseHumidifier();
        sysState.humidifier_is_on = false;
      }
      xSemaphoreGive(xMutex);
    }
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}

// 3. Firebase Sync Task (with error recovery)
void firebaseTask(void *pvParameters) {
  while (true) {
    // Attempt to re-authenticate if not ready, with backoff
    if (!firebaseReady) {
      if (millis() - lastAuthAttempt > authRetryInterval) {
        lastAuthAttempt = millis();
        Serial.println("Attempting Firebase authentication...");
        Firebase.begin(&fbConfig, &auth);
        // Wait a moment for token generation
        vTaskDelay(pdMS_TO_TICKS(2000));
        if (Firebase.ready()) {
          firebaseReady = true;
          Serial.println("Firebase authenticated successfully");
        } else {
          Serial.println("Firebase authentication failed, will retry later");
        }
      }
      vTaskDelay(pdMS_TO_TICKS(1000));
      continue;
    }

    // Sync data if Firebase is ready
    if (Firebase.ready()) {
      if (xSemaphoreTake(xMutex, portMAX_DELAY)) {
        FirebaseJson json;
        json.set("sensors/temperature", sysState.temp);
        json.set("sensors/humidity", sysState.humidity);
        json.set("sensors/moisture", sysState.moisture);
        json.set("actuators/pump", sysState.pump_on);
        json.set("actuators/fan", sysState.fan_on);
        json.set("actuators/lights", sysState.lights_on);
        json.set("actuators/humidifier", sysState.humidifier_is_on);

        if (Firebase.RTDB.setJSON(&fbdo, "/greenhouse/state", &json)) {
          // Serial.println("State updated to Firebase");
        } else {
          Serial.printf("Firebase set failed: %s\n", fbdo.errorReason().c_str());
          // If error indicates auth issue, reset firebaseReady to force re-auth
          if (fbdo.errorReason().indexOf("token") >= 0 || fbdo.errorReason().indexOf("auth") >= 0) {
            firebaseReady = false;
            Serial.println("Auth error, will re-authenticate");
          }
        }
        xSemaphoreGive(xMutex);
      }

      // Pull config updates from Firebase (only if ready)
      if (Firebase.RTDB.getFloat(&fbdo, "/greenhouse/config/temp_high")) {
        float newVal = fbdo.floatData();
        if (newVal != sysConfig.temp_high) {
          sysConfig.temp_high = newVal;
          saveConfig();
        }
      }
      if (Firebase.RTDB.getFloat(&fbdo, "/greenhouse/config/temp_low")) {
        float newVal = fbdo.floatData();
        if (newVal != sysConfig.temp_low) {
          sysConfig.temp_low = newVal;
          saveConfig();
        }
      }
      if (Firebase.RTDB.getFloat(&fbdo, "/greenhouse/config/hum_low")) {
        float newVal = fbdo.floatData();
        if (newVal != sysConfig.hum_low) {
          sysConfig.hum_low = newVal;
          saveConfig();
        }
      }
      if (Firebase.RTDB.getFloat(&fbdo, "/greenhouse/config/hum_high")) {
        float newVal = fbdo.floatData();
        if (newVal != sysConfig.hum_high) {
          sysConfig.hum_high = newVal;
          saveConfig();
        }
      }
      if (Firebase.RTDB.getFloat(&fbdo, "/greenhouse/config/moisture_dry")) {
        float newVal = fbdo.floatData();
        if (newVal != sysConfig.moisture_dry) {
          sysConfig.moisture_dry = newVal;
          saveConfig();
        }
      }
      if (Firebase.RTDB.getFloat(&fbdo, "/greenhouse/config/moisture_target")) {
        float newVal = fbdo.floatData();
        if (newVal != sysConfig.moisture_target) {
          sysConfig.moisture_target = newVal;
          saveConfig();
        }
      }
    } else {
      // Firebase not ready, maybe token expired or network issue
      firebaseReady = false;
    }
    vTaskDelay(pdMS_TO_TICKS(5000)); // Sync every 5 seconds
  }
}

// ============ SETUP ============
void setup() {
  Serial.begin(115200);

  // Initialize Pins
  pinMode(PUMP_RELAY_PIN, OUTPUT);
  pinMode(FAN_RELAY_PIN, OUTPUT);
  pinMode(LIGHT_RELAY_PIN, OUTPUT);
  pinMode(HUMIDIFIER_PIN, OUTPUT);
  digitalWrite(PUMP_RELAY_PIN, LOW);
  digitalWrite(FAN_RELAY_PIN, LOW);
  digitalWrite(LIGHT_RELAY_PIN, LOW);
  digitalWrite(HUMIDIFIER_PIN, LOW);

  dht.setup(DHT_PIN, DHTesp::DHT22);
  xMutex = xSemaphoreCreateMutex();

  // Load configuration from LittleFS
  loadConfig();

  // WiFiManager - Blocks until connected or configured via AP
  WiFiManager wifiManager;
  // wifiManager.resetSettings(); // Uncomment to clear saved WiFi
  Serial.println("Connecting to WiFi...");
  wifiManager.autoConnect("TerraNurture-Setup");
  Serial.println("WiFi Connected!");

  // Firebase configuration
  fbConfig.api_key = FIREBASE_API_KEY;
  fbConfig.database_url = FIREBASE_DATABASE_URL;
  auth.user.email = USER_EMAIL;
  auth.user.password = USER_PASSWORD;
  fbConfig.token_status_callback = tokenStatusCallback; // optional, prints token status

  // Do not call Firebase.begin here; we'll attempt in the task with retries
  // Firebase.begin(&fbConfig, &auth);
  // Firebase.reconnectWiFi(true);

  // Start Tasks
  xTaskCreatePinnedToCore(sensorTask, "Sensor Task", 4096, NULL, 1, NULL, 1);
  xTaskCreatePinnedToCore(controlTask, "Control Task", 4096, NULL, 2, NULL, 1);
  xTaskCreatePinnedToCore(firebaseTask, "Firebase Task", 8192, NULL, 1, NULL, 0);

  Serial.println("Setup complete. Tasks running.");
}

void loop() {
  vTaskDelete(NULL); // Idle
}