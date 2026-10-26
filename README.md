# Health Monitoring for Alzheimer's Patients

An embedded IoT wearable health-monitoring and fall-detection band designed specifically for Alzheimer's patients. Built around an **ESP32 microcontroller**, the system provides continuous real-time telemetry (heart rate, blood oxygen, ambient temperature, humidity, 3-axis motion, and GPS tracking), automated medicine reminder alarms via an active buzzer, a 4-stage fall-detection algorithm, and a password-locked web dashboard accessible via Wi-Fi or direct device hotspot.

---

## 1. Project Overview & Abstract

Alzheimer's disease and age-related cognitive decline present severe risks regarding wandering, missed medication schedules, and sudden falls. The **AlzCare Band** is a low-cost, edge-computed wearable prototype that addresses these challenges without relying on external cloud subscriptions. 

---

## 2. System Architecture & Dual-Core Design

Standard single-core microcontrollers struggle when handling simultaneous Wi-Fi requests, TCP/IP overhead, and timing-critical optical sensors like the MAX30100 (which requires buffer updates every ~10ms). The AlzCare Band overcomes this using **Dual-Core Task Pinning**:

* **Core 1 (`App_CPU`):** Dedicated exclusively to high-speed, jitter-free sensor polling. It continuously updates the `MAX30100` pulse oximeter FIFO buffer and samples the `ADXL345` accelerometer at 100 Hz for real-time impact detection.
* **Core 0 (`Pro_CPU`):** Handles the background tasks, Wi-Fi reconnection stack, HTTP server client requests, NTP atomic time synchronization, and non-blocking `DHT11` temperature/humidity polling every 2.5 seconds. This isolation prevents Wi-Fi packet latency from corrupting patient health metrics.

---

## 3. Bill of Materials (BOM)

| Item | Quantity | Description |
| :--- | :---: | :--- |
| **ESP32 DevKit** | 1 | Main processing unit with built-in Wi-Fi and Bluetooth |
| **MAX30100 Module** | 1 | Optical Heart Rate (BPM) and Blood Oxygen ($\text{SpO}_2$) sensor |
| **ADXL345 (GY-291) Module** | 1 | 3-Axis digital accelerometer for motion and fall detection |
| **DHT11 Sensor Module** | 1 | Ambient temperature and relative humidity sensor |
| **NEO-6M GPS Module** | 1 | Satellite receiver for real-time latitude, longitude, and speed |
| **Active Buzzer Module** | 1 | Audio transducer for medicine reminders and fall sirens |
| **Breadboard** | 1 | Compact prototyping hub for power and I2C distribution |
| **Connecting Wires** | Set | Male-to-male and male-to-female Dupont jumpers |
| **Power Supply** | 1 | 5V 1A USB wall adapter or regulated portable power bank |

---

## 4. Pin Wiring Table & Power Plan

### Power Plan & Current Management
* **Input Power:** Feed 5V into the ESP32 via its USB port or `VIN` pin using a stable 5V 1A supply.
* **Rail Distribution:** Connect the ESP32 `3.3V` pin to the breadboard positive rail and `GND` to the negative rail. 
* **Sensor Logic:** Power all sensors (`MAX30100`, `ADXL345`, `DHT11`, `NEO-6M GPS`) from the `3.3V` rail, ensuring safe 3.3V logic compatibility with the ESP32 GPIOs. (Note: Connect GPS `VCC` to `VIN/5V` if your module uses an onboard LDO regulator requiring >4V).

### Complete Pin Connections

| Component | Module Pin | ESP32 Pin / Rail | Engineering Notes |
| :--- | :--- | :--- | :--- |
| **Power Bus** | `3V3` | ESP32 `3V3` | Main 3.3V regulated power rail |
| **Ground Bus** | `GND` | ESP32 `GND` | Common ground rail |
| **MAX30100** | `VIN`, `GND` | `3.3V`, `GND` | Optical Heart Rate & $\text{SpO}_2$ |
| **MAX30100** | `SDA`, `SCL` | `GPIO 21`, `GPIO 22` | Shared I2C Bus |
| **ADXL345** | `VCC`, `GND` | `3.3V`, `GND` | 3-Axis Accelerometer |
| **ADXL345** | `SDA`, `SCL` | `GPIO 21`, `GPIO 22` | Shared I2C Bus |
| **ADXL345** | `CS` | `3.3V` | Tied to 3.3V to force I2C mode |
| **ADXL345** | `SDO` | `GND` | Tied to GND to set I2C address to `0x53` |
| **NEO-6M GPS**| `VCC`, `GND` | `VIN (5V)`, `GND` | Powered from 5V line for stable LDO operation |
| **NEO-6M GPS**| `TX`, `RX` | `GPIO 16 (RX2)`, `GPIO 17 (TX2)` | UART2 serial communication |
| **DHT11 Sensor**| `VCC`, `GND` | `3.3V`, `GND` | Temperature & Humidity |
| **DHT11 Sensor**| `DATA` | `GPIO 4` | Single-wire digital data pin |
| **Active Buzzer**| `Positive (+)` / `I/O` | `GPIO 26` | Direct drive (add $100\,\Omega$ resistor if bare coil) |
| **Active Buzzer**| `Negative (-)` | `GND` | Common ground |

---

## 5. Software Prerequisites & Library Installation

1. **Arduino IDE:** Download and install [Arduino IDE 2.x](https://www.arduino.cc/en/software).
2. **ESP32 Board Package:** Under **File $\rightarrow$ Preferences**, add `https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json` to Additional Boards Manager URLs. Then go to **Boards Manager**, search for `esp32` by Espressif Systems, and install version `2.0.17` or later.
3. **Required Libraries:** Open **Library Manager** (`Ctrl + Shift + I`) and install:
   * **`TinyGPSPlus`** (by Mikal Hart) — Parses NMEA GPS strings.
   * **`DHT sensor library`** (by Adafruit) — Manages DHT11 temperature/humidity readings (accept installation of dependent *Adafruit Unified Sensor*).
   * **`MAX30100lib`** (by OXullo Intersecans) — Handles pulse oximetry data acquisition.

---

## 6. Assembly & Upload Procedure

1. **Hardware Assembly:** Mount the ESP32 and sensors on your breadboard following the pin wiring table above. Double-check power and ground connections to prevent short circuits.
2. **Code Configuration:** Open your Arduino IDE, paste the firmware sketch into your main `.ino` tab, and update your local Wi-Fi credentials in the settings block:
   ```cpp
   const char* WIFI_SSID = "YOUR_WIFI_SSID";
   const char* WIFI_PASS = "YOUR_WIFI_PASSWORD";
   ```
3. **Board Selection:** Select **ESP32 Dev Module** from the Tools menu, choose your correct COM port, and set Baud Rate to `115200`.
4. **Compilation & Upload:** Click **Upload**. *(If uploading hangs at "Connecting...", press and hold the physical `BOOT` button on the ESP32 until the upload progress bar begins).*

---

## 7. Network Configuration & Fallback Hotspot

* **Station Mode (Primary):** On boot, the ESP32 attempts to connect to your configured home or laboratory Wi-Fi network (`WIFI_SSID`). Once connected, it synchronizes its internal clock via NTP (`pool.ntp.org`) and prints its local IP address in the Serial Monitor (e.g., `http://192.168.1.50`).
* **Fallback Access Point (AP) Mode:** If no known Wi-Fi network is found within 15 seconds, the ESP32 automatically switches to standalone Access Point mode, broadcasting an open Wi-Fi network named **`AlzCare-Band`** (Password: `12345678`). You can connect any mobile phone directly to this network and browse to **`http://192.168.4.1`** without needing an internet router.

---

## 8. Core Features & Security Lock System

* **Caregiver Security Lock:** The web interface is secured behind a login screen requiring a username (`admin`) and password (`alz1234`), preventing unauthorized individuals from altering medical schedules or silencing alarms.
* **Medicine Buzzer Scheduler:** Caregivers can configure up to 6 daily medicine reminder alarms directly from the web interface. Times and names are saved persistently to the ESP32's non-volatile NVS Flash (`Preferences`). When the clock matches a scheduled time, **GPIO 26** triggers a distinct double-beep buzzer pattern.
* **4-Stage Fall Detection State Machine:** The ADXL345 continuously monitors G-force magnitude ($|a| = \sqrt{a_x^2 + a_y^2 + a_z^2}$). The algorithm processes potential falls through four rigorous states:
  1. **Idle:** Monitoring normal movement.
  2. **Free-Fall / Impact:** Detects sudden deceleration or impact ($>2.5g$).
  3. **Settling:** Pauses briefly to ignore bounce vibration.
  4. **Stillness Window:** Confirms whether the patient remains inactive for 2 seconds following impact before declaring a medical emergency.
* **GPS Live Tracking:** When a fall is confirmed, the system captures the patient's latitude and longitude from the NEO-6M GPS module and generates a direct one-click **Google Maps navigation link** on the dashboard.

---

## 9. Expected Outputs & Verification Guide

1. **Serial Monitor Output (`115200` baud):**
   ```text
   === AlzCare Single-Patient Locked Band ===
   I2C scan (expect 0x53 ADXL345, 0x57 MAX30100):
     found 0x53
     found 0x57
   ADXL345 : OK
   MAX30100: OK
   Connecting WiFi.....
   WiFi OK. Open http://192.168.1.45
   ```
2. **Web Dashboard Login:** Open the IP address in Chrome or Safari on your phone or laptop. Enter username `admin` and password `alz1234` to unlock the interface.
3. **Live Telemetry & Sparkline:** Place your finger gently on the MAX30100 sensor. Within 5–10 seconds, the heart rate (BPM) and $\text{SpO}_2$ percentage will populate alongside a real-time canvas sparkline graph.
4. **Fall Simulation Test:** Tap or shake the breadboard sharply to simulate an impact, then leave it flat and still for 2 seconds. The dashboard will immediately display a flashing red **"EMERGENCY: FALL DETECTED!"** banner, activate the browser audio siren, and trigger rapid beeping on **GPIO 26**. Click **"Acknowledge & Clear"** to reset the state machine.
