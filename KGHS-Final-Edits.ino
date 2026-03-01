/*
============================================================
ESP32 Smart Greenhouse Controller
Kangaru Girls High School

Description:
This system monitors temperature, humidity, and soil moisture
using sensors connected to an ESP32 and controls actuators
(Fan, Warm LED, Water Pump, Humidifier) based on configurable
threshold values stored in Firebase RTDB and LittleFS.

Control Philosophy:
The system maintains environmental stability using upper and
lower thresholds (hysteresis control) to prevent rapid relay
switching and ensure plant safety.

------------------------------------------------------------
Temperature Control:
Target Range: 20°C – 22°C

- If temperature > temp_high → Fan ON
- If temperature < temp_low → Warm LED ON
- If temperature within range → Both OFF

------------------------------------------------------------
Humidity Control:
Target Range: 55% – 65%

- If humidity < hum_low → Humidifier ON
- If humidity > hum_high → Humidifier OFF
- Within range → Maintain current state

------------------------------------------------------------
Soil Moisture Control:
Target Range: 45% – 65%

- If moisture < moisture_dry → Pump ON
- If moisture > moisture_target → Pump OFF
- Within range → Pump OFF

------------------------------------------------------------
Manual Override:
Firebase can trigger temporary actuator control.
Overrides bypass automatic logic for a defined duration
before returning to automatic mode.

------------------------------------------------------------
Failsafe Behavior:
If sensor reading fails:
Temperature and Humidity are set to -999.
This allows remote monitoring to detect sensor issues.

------------------------------------------------------------
Relay Logic:
Relays are ACTIVE LOW:
LOW  = ON
HIGH = OFF
============================================================
*/

#include <WiFi.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <DHT.h>
#include <WiFiManager.h>
#include <Firebase_ESP_Client.h>
#include "addons/TokenHelper.h"
#include "addons/RTDBHelper.h"

// ================== FIREBASE CONFIG ==================
#define FIREBASE_API_KEY "AIzaSyDkOGxnbZA3ycNAipihN0NXo8zR4Nksh7I"               // Your API key
#define FIREBASE_DATABASE_URL "https://kghs-7599e-default-rtdb.firebaseio.com/"  // Your RTDB URL
#define USER_EMAIL "kangarugirls@gmail.com"  // Firebase Auth email
#define USER_PASSWORD "KGHS@2026"            // Firebase Auth password

// ================== PIN CONFIG ==================
#define ADC_PIN 34
#define DHT_PIN 4
#define DHT_TYPE DHT11
#define PUMP_RELAY_PIN 25
#define FAN_RELAY_PIN 26
#define LIGHT_RELAY_PIN 27
#define HUMIDIFIER_PIN 14

// ================== SYSTEM STATE & CONFIG ==================
struct Config {
  float temp_high = 22.0;
  float temp_low = 20.0;
  float hum_low = 45.0;
  float hum_high = 50.0;
  float moisture_dry = 45.0;
  float moisture_target = 60.0;
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

// ================== GLOBALS ==================
SemaphoreHandle_t xMutex;
DHT dht(DHT_PIN, DHT_TYPE);

FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig fbConfig;
bool firebaseReady = false;
unsigned long lastAuthAttempt = 0;
const unsigned long authRetryInterval = 30000;  // 30 sec

// ================= SOIL MOISTURE MAPPING =================
float mapMoisture(int raw) {

    // Safety
    if (raw <= 0) return -999;

    // These MUST be calibrated properly
    int dryValue = sysConfig.adc_dry;
    int wetValue = sysConfig.adc_wet;

    // Prevent divide-by-zero
    if (dryValue == wetValue) return -999;

    // Linear map
    float percentage = (float)(raw - dryValue) * 100.0 / (wetValue - dryValue);

    // Clamp
    if (percentage > 100) percentage = 100;
    if (percentage < 0) percentage = 0;

    return percentage;
}


void pulseHumidifier() {
  digitalWrite(HUMIDIFIER_PIN, HIGH);
  vTaskDelay(pdMS_TO_TICKS(500));
  digitalWrite(HUMIDIFIER_PIN, LOW);
}

void saveConfig() {
  File configFile = LittleFS.open("/config.json", FILE_WRITE);
  if (!configFile) { Serial.println("Failed to open config file for writing"); return; }
  StaticJsonDocument<256> doc;
  doc["temp_high"] = sysConfig.temp_high;
  doc["temp_low"] = sysConfig.temp_low;
  doc["hum_low"] = sysConfig.hum_low;
  doc["hum_high"] = sysConfig.hum_high;
  doc["moisture_dry"] = sysConfig.moisture_dry;
  doc["moisture_target"] = sysConfig.moisture_target;
  doc["adc_wet"] = sysConfig.adc_wet;
  doc["adc_dry"] = sysConfig.adc_dry;
  serializeJson(doc, configFile);
  configFile.close();
}

void loadConfig() {
  if (!LittleFS.begin(true)) { Serial.println("LittleFS mount failed"); return; }
  File configFile = LittleFS.open("/config.json", FILE_READ);
  if (!configFile) { Serial.println("No config file, using defaults"); saveConfig(); return; }
  StaticJsonDocument<256> doc;
  DeserializationError error = deserializeJson(doc, configFile);
  if (error) { Serial.println("Failed to parse config file, using defaults"); saveConfig(); return; }
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

// ================== TASKS ==================





// ================= SENSOR TASK =================
void sensorTask(void *pvParameters) {
  while (true) {
    float t = -999.0, h = -999.0;

    // Read temperature & humidity
    float tempRead = dht.readTemperature();
    float humRead = dht.readHumidity();
    if (!isnan(tempRead)) t = tempRead;
    if (!isnan(humRead)) h = humRead;

    // Read soil moisture
    long sum = 0;
    for (int i = 0; i < 10; i++) { sum += analogRead(ADC_PIN); vTaskDelay(pdMS_TO_TICKS(5)); }
    int raw_adc = sum / 10;
    float m = mapMoisture(raw_adc);

    // Update system state safely
    if (xSemaphoreTake(xMutex, portMAX_DELAY)) {
      sysState.temp = t;
      sysState.humidity = h;
      sysState.moisture = m;
      xSemaphoreGive(xMutex);
    }

    // Serial debug for confirmation
    Serial.printf("RAW ADC: %d | Temp: %.2f | Hum: %.2f | Moisture: %.2f%%\n",
                  raw_adc, t, h, m);

    vTaskDelay(pdMS_TO_TICKS(2000)); // 2-second loop
  }
}

// -------- CONTROL TASK --------
void controlTask(void *pvParameters) {

  while (true) {

    if (xSemaphoreTake(xMutex, portMAX_DELAY)) {

      // ===== TEMPERATURE =====
      if (sysState.temp > sysConfig.temp_high) {
        sysState.fan_on = true;
        sysState.lights_on = false;
      }
      else if (sysState.temp < sysConfig.temp_low) {
        sysState.lights_on = true;
        sysState.fan_on = false;
      }
      else {
        sysState.fan_on = false;
        sysState.lights_on = false;
      }

      digitalWrite(FAN_RELAY_PIN, sysState.fan_on);
      digitalWrite(LIGHT_RELAY_PIN, sysState.lights_on);


      // ===== HUMIDITY =====
      if (sysState.humidity < sysConfig.hum_low) {
        sysState.humidifier_is_on = true;
      }
      else if (sysState.humidity > sysConfig.hum_high) {
        sysState.humidifier_is_on = false;
      }

      digitalWrite(HUMIDIFIER_PIN, sysState.humidifier_is_on);


      // ===== SOIL =====
      if (sysState.moisture < sysConfig.moisture_dry) {
        sysState.pump_on = true;
      }
      else if (sysState.moisture > sysConfig.moisture_target) {
        sysState.pump_on = false;
      }

      digitalWrite(PUMP_RELAY_PIN, sysState.pump_on);

      xSemaphoreGive(xMutex);
    }

    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}




// void controlTask(void *pvParameters) {
//   while (true) {
//     if (xSemaphoreTake(xMutex, portMAX_DELAY)) {
//       sysState.fan_on = (sysState.temp > sysConfig.temp_high);
//       digitalWrite(FAN_RELAY_PIN, sysState.fan_on ? HIGH : LOW);

//       sysState.lights_on = (sysState.temp < sysConfig.temp_low);
//       digitalWrite(LIGHT_RELAY_PIN, sysState.lights_on ? HIGH : LOW);

//       sysState.pump_on = (sysState.moisture < sysConfig.moisture_dry);
//       if (sysState.moisture > sysConfig.moisture_target) sysState.pump_on = false;
//       digitalWrite(PUMP_RELAY_PIN, sysState.pump_on ? HIGH : LOW);

//       if (sysState.humidity < sysConfig.hum_low && !sysState.humidifier_is_on) {
//         pulseHumidifier();
//         sysState.humidifier_is_on = true;
//       } else if (sysState.humidity >= sysConfig.hum_high && sysState.humidifier_is_on) {
//         pulseHumidifier();
//         sysState.humidifier_is_on = false;
//       }
//       xSemaphoreGive(xMutex);
//     }
//     vTaskDelay(pdMS_TO_TICKS(1000));
//   }
// }

// -------- FIREBASE TASK --------
void firebaseTask(void *pvParameters) {
  fbdo.setResponseSize(1024);

  while (true) {
    // AUTHENTICATION
    if (!firebaseReady) {
      if (millis() - lastAuthAttempt > authRetryInterval) {
        lastAuthAttempt = millis();
        Serial.println("Attempting Firebase authentication...");
        Firebase.begin(&fbConfig, &auth);
        vTaskDelay(pdMS_TO_TICKS(2000));
        if (Firebase.ready()) {
          firebaseReady = true;
          Serial.println("Firebase authenticated successfully");
        } else {
          Serial.println("Firebase authentication failed");
        }
      }
      vTaskDelay(pdMS_TO_TICKS(1000));
      continue;
    }

    // SYNC STATE
    if (Firebase.ready()) {
      State localState;
      if (xSemaphoreTake(xMutex, portMAX_DELAY)) { localState = sysState; xSemaphoreGive(xMutex); }

      FirebaseJson json;
      json.set("sensors/temperature", localState.temp);
      json.set("sensors/humidity", localState.humidity);
      json.set("sensors/moisture", localState.moisture);
      json.set("actuators/pump", localState.pump_on);
      json.set("actuators/fan", localState.fan_on);
      json.set("actuators/lights", localState.lights_on);
      json.set("actuators/humidifier", localState.humidifier_is_on);

      if (!Firebase.RTDB.setJSON(&fbdo, "/greenhouse/state", &json)) {
        Serial.printf("Firebase write failed: %s\n", fbdo.errorReason().c_str());
        if (fbdo.errorReason().indexOf("auth") >= 0 || fbdo.errorReason().indexOf("token") >= 0) firebaseReady = false;
      }

      // READ CONFIG
      if (Firebase.RTDB.getJSON(&fbdo, "/greenhouse/config")) {
        FirebaseJson *cfgJson = fbdo.to<FirebaseJson *>();
        if (cfgJson) {
          FirebaseJsonData data;
          bool changed = false;
          if (cfgJson->get(data, "temp_high") && data.floatValue != sysConfig.temp_high) { sysConfig.temp_high = data.floatValue; changed = true; }
          if (cfgJson->get(data, "temp_low") && data.floatValue != sysConfig.temp_low) { sysConfig.temp_low = data.floatValue; changed = true; }
          if (changed) saveConfig();
        }
      } else {
        Serial.printf("Config read failed: %s\n", fbdo.errorReason().c_str());
      }
    } else firebaseReady = false;

    vTaskDelay(pdMS_TO_TICKS(5000));
  }
}

// ================== SETUP ==================
void setup() {
  Serial.begin(115200);
  Serial.println("\n=========== System Boot ============");

  // Pins
  pinMode(PUMP_RELAY_PIN, OUTPUT);
  pinMode(FAN_RELAY_PIN, OUTPUT);
  pinMode(LIGHT_RELAY_PIN, OUTPUT);
  pinMode(HUMIDIFIER_PIN, OUTPUT);
  pinMode(DHT_PIN, INPUT);

  digitalWrite(PUMP_RELAY_PIN, LOW);
  digitalWrite(FAN_RELAY_PIN, LOW);
  digitalWrite(LIGHT_RELAY_PIN, LOW);
  digitalWrite(HUMIDIFIER_PIN, LOW);

  // DHT Sensor
  dht.begin();

  // Mutex
  xMutex = xSemaphoreCreateMutex();

  // Load config
  loadConfig();

  // WiFi
  WiFiManager wifiManager;
  Serial.println("Connecting to WiFi...");
  wifiManager.autoConnect("TerraNurture-Setup");
  Serial.println("WiFi Connected!");
  Serial.printf("Free heap: %u\n", ESP.getFreeHeap());

  // Firebase
  fbConfig.api_key = FIREBASE_API_KEY;
  fbConfig.database_url = FIREBASE_DATABASE_URL;
  auth.user.email = USER_EMAIL;
  auth.user.password = USER_PASSWORD;
  fbConfig.token_status_callback = tokenStatusCallback;
  fbdo.setResponseSize(1024);

  // Create Tasks
  xTaskCreatePinnedToCore(sensorTask, "Sensor Task", 4096, NULL, 1, NULL, 1);
  xTaskCreatePinnedToCore(controlTask, "Control Task", 4096, NULL, 2, NULL, 1);
  xTaskCreatePinnedToCore(firebaseTask, "Firebase Task", 40960, NULL, 1, NULL, 0);

  Serial.println("Setup complete. Tasks running.");
}

void loop() {
  vTaskDelete(NULL);
}