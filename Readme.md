
---

# 🌱 Smart Greenhouse Control System

### ESP32 + Firebase Real-Time Environmental Regulation

---

## 📘 Overview

This project implements an automated greenhouse control system using an ESP32 microcontroller integrated with Firebase Realtime Database.

The system monitors:

* 🌡 Temperature
* 💧 Humidity
* 🌱 Soil Moisture

And controls:

* 🌀 Ventilation Fan
* 💡 Warm LED (Heating Support)
* 🚿 Humidifier
* 🚰 Water Pump

The control logic is designed using hysteresis thresholds to ensure environmental stability and prevent rapid relay switching.

---

# ⚙️ Control Philosophy

The system maintains a **stable environmental band**, not a single fixed point.

Devices activate only when values move outside defined thresholds and deactivate once the environment returns to the safe zone.

This prevents:

* Relay chattering
* Power waste
* Plant stress
* Rapid oscillations

---

# 🌡 Temperature Regulation

**Target Stability Band:**
**20.0°C – 22.0°C**

| Condition          | 🌀 Fan | 💡 Warm LED |
| ------------------ | ------ | ----------- |
| Temperature > 22°C | ✅ ON   | ❌ OFF       |
| Temperature < 20°C | ❌ OFF  | ✅ ON        |
| 20°C – 22°C        | ❌ OFF  | ❌ OFF       |

✔ Stable comfort band
✔ No device active inside safe range
✔ Prevents heating/cooling conflict

---

# 💧 Humidity Regulation

**Target Stability Band:**
**55% – 65%**

| Condition      | 🚿 Humidifier          |
| -------------- | ---------------------- |
| Humidity < 55% | ✅ ON                   |
| Humidity > 65% | ❌ OFF                  |
| 55% – 65%      | Maintain Current State |

✔ Uses hysteresis
✔ Prevents rapid ON/OFF cycling
✔ Maintains plant transpiration balance

---

# 🌱 Soil Moisture Regulation

**Target Stability Band:**
**45% – 65%**

| Condition      | 🚰 Pump |
| -------------- | ------- |
| Moisture < 45% | ✅ ON    |
| Moisture > 65% | ❌ OFF   |
| 45% – 65%      | ❌ OFF   |

✔ Prevents overwatering
✔ Allows mid-range soil flexibility (~50%)
✔ Supports root oxygen balance

---

# 🔁 Relay Logic

⚠ **Relays are ACTIVE LOW**

| Signal Level | Relay State |
| ------------ | ----------- |
| LOW          | ON          |
| HIGH         | OFF         |

The firmware accounts for this inversion to ensure correct actuation.

---

# 🧠 System Architecture

* Multi-tasked using FreeRTOS
* Mutex-protected shared state
* Firebase RTDB for configuration + monitoring
* Local fallback configuration via LittleFS
* Dynamic threshold adjustment from cloud

---

# 🛠 Manual Override (Scalable Feature)

The system architecture supports manual override via Firebase:

* Temporary actuator triggering
* Timed activation (e.g., 15s / 25s / 30s)
* Automatic return to control logic
* Safe coexistence with environmental automation

This enables UI slider controls and remote intervention without compromising stability.

---

# 📊 Sensor Mapping

Soil moisture is mapped dynamically from raw ADC values into a calibrated 0–100% scale:

* `adc_dry` → 0%
* `adc_wet` → 100%

Calibration values are stored in configuration and can be updated without firmware modification.

---

# 🧩 Fail-Safe Behavior

If a sensor fails:

* Temperature and Humidity return `-999`
* System avoids undefined actuation
* Firebase reflects sensor fault state
* Physical hardware remains safe

---

# 📡 Cloud Structure

```
greenhouse/
│
├── config/
│   ├── temp_high
│   ├── temp_low
│   ├── hum_high
│   ├── hum_low
│   ├── moisture_dry
│   └── moisture_target
│
├── sensors/
│   ├── temperature
│   ├── humidity
│   └── moisture
│
└── state/
    ├── fan
    ├── lights
    ├── humidifier
    └── pump
```

---

# 🎯 Design Goals

* Environmental stability
* Safe relay control
* Cloud scalability
* Clear threshold separation
* Expandable architecture
* Clean documentation for future contributors

---

# 👨‍💻 Author

**S.E Evans** <br><br>

Embedded Systems & Environmental Automation Development

---


