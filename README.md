# SPMS – Smart Power Monitoring System

LankaSPMS is an Industrial IoT (IIoT) based **Smart Power Monitoring System (SPMS)** designed to monitor power availability across multiple distribution lines in real-time.

Each device supports **8 independent power lines** and communicates over **4G LTE** to **ThingsBoard Cloud**, enabling centralized nationwide monitoring across Sri Lanka.

Currently, **5 devices are deployed and active**.

---

# 🚀 Key Features

* ✅ Monitor up to **8 power lines per device**
* ✅ Detect power presence / power loss
* ✅ Real-time voltage measurement
* ✅ 4G LTE communication
* ✅ MQTT telemetry to ThingsBoard Cloud
* ✅ Live dashboard with:

  * Device location map
  * Device active/inactive status
  * Voltage graphs
  * Line status monitoring
* ✅ Event-based telemetry (sends only on change)
* ✅ Low-power light sleep mode
* ✅ Automatic reconnection & watchdog recovery
* ✅ Error-based automatic restart

---

# 🧠 System Architecture

## 📟 Edge Device

* ESP32 Microcontroller
* 4G LTE Modem (IIOT Dev Kit 4G)
* SIM Card
* 8 Optocoupler-isolated inputs
* ADC voltage sensing circuit
* 12V Power Supply
* Buzzer indicator

## ☁️ Cloud Platform

* ThingsBoard Cloud
* MQTT Telemetry
* Device Map Visualization
* Realtime Graphs
* Device State Monitoring

---

# 🔌 Hardware Overview

| Component        | Description                 |
| ---------------- | --------------------------- |
| ESP32            | Main controller             |
| 4G LTE Module    | Internet connectivity       |
| Optocouplers     | AC line isolation detection |
| SIM Card         | Cellular communication      |
| 12V Supply       | Device power input          |
| ADC Pin          | Voltage measurement         |
| 8 Digital Inputs | Line detection              |

---

# ⚡ Power Monitoring Logic

* Digital HIGH → No Power
* Digital LOW → Power Available
* Optocouplers provide safe AC isolation
* 8 lines monitored independently

Each line state change triggers telemetry transmission.

---

# 📡 Telemetry Format

## Line Status

Example:

```json
{
  "1": "true"
}
```

Where:

* `"true"` → Power Available
* `"false"` → No Power

## Voltage Telemetry

```json
{
  "V": 219.5
}
```

* Averaged from 10 ADC samples
* Sent every 30 seconds
* Automatically clamped if invalid

---

# 🔄 Firmware State Machine

The firmware uses a state-based architecture:

```
_Check_Sleep
_ON_Init_4G
_MQTT_START
_MQTT_Connect
_send_data
```

### Logic Flow:

1. Initialize 4G modem
2. Ensure PDP context active
3. Start MQTT client
4. Connect to ThingsBoard
5. Send:

   * Line change events
   * Voltage telemetry (30s interval)
6. Enter light sleep mode
7. Restart on repeated failures

---

# 🔑 Device Authentication & Multi-Device Deployment

All SPMS devices run the **same firmware codebase**.

However, each deployed device must use **unique MQTT credentials** for secure identification in ThingsBoard Cloud.

## 🔐 Unique Per Device:

```cpp
#define MQTT_CLIENTID   "DEVICE_UNIQUE_CLIENT_ID"
#define MQTT_USERNAME   "DEVICE_ACCESS_TOKEN"
#define MQTT_PASSWORD   "DEVICE_PASSWORD"
```

## Why Unique Credentials Are Required

Each device in ThingsBoard is registered as a separate entity.

Unique credentials allow:

* Individual device identification
* Separate telemetry storage
* Independent device status tracking
* Accurate map visualization
* Secure device isolation

Even though firmware is identical, **credentials must differ for each device**.

---

# 🏗 Deployment Model

| Device    | Client ID | Access Token | Location |
| --------- | --------- | ------------ | -------- |
| 0000_MSAN | Unique    | Unique       | Region A |
| 0001_MSAN | Unique    | Unique       | Region B |
| 0002_MSAN | Unique    | Unique       | Region C |
| 0003_MSAN | Unique    | Unique       | Region D |
| 0004_MSAN | Unique    | Unique       | Region E |

📍 Country: Sri Lanka
📡 Connected Devices: 5

---

# 🔐 MQTT Configuration

```cpp
#define MQTT_SERVER  "thingsboard.cloud"
#define MQTT_PORT    "1883"
#define MQTT_TELE_Topic "v1/devices/me/telemetry"
```

Device authentication uses MQTT username/password credentials provided per device.

---

# 🛠 Firmware Highlights

* PDP context verification before MQTT connection
* Automatic MQTT session cleanup
* Watchdog timer enabled
* Error threshold auto-restart mechanism
* Light sleep optimization
* Change-based telemetry transmission

---

# 🔒 Security Recommendations (Production)

For production deployments:

* Avoid hardcoding credentials in firmware
* Store credentials securely in NVS
* Use MQTT over TLS (port 8883)
* Implement secure device provisioning
* Enable OTA firmware updates
* Use encrypted communication

---

# 📊 ThingsBoard Dashboard Features

* 🗺 Device location map
* 🟢 Active / 🔴 Inactive status indicator
* 📈 Voltage history graph
* 🔌 8-line monitoring panel
* 📡 Real-time device activity

---

# 📂 Repository Structure

```
/firmware
    main.cpp
/hardware
    schematics.pdf
/images
    dashboard.png
README.md
```

---

# 🚀 Future Improvements

* OTA firmware updates
* TLS secure MQTT
* Line outage duration tracking
* Fault classification
* SMS alert system
* Regional grouping & analytics
* Edge buffering when network unavailable
* Historical outage reporting

---

# 👨‍💻 Use Case

Nationwide infrastructure power visibility and remote line monitoring across distributed locations in Sri Lanka.

---

# 🤝 Contributions

This is an active IIoT deployment project.
Improvements and feature suggestions are welcome.

---
