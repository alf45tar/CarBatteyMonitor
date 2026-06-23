# ESP32 0-24V Voltage Monitor

An advanced, industrial-grade voltage monitoring system designed for the ESP32 platform. This project samples analog voltage (0-24V range) at a high frequency on a dedicated CPU core, averages the data, and securely uploads the data to ThingSpeak and/or ThingsBoard.

Featuring a dual-core **FreeRTOS architecture**, **NTP real-world time synchronization**, and an offline **LittleFS flash storage buffer**, this system guarantees zero data loss and uninterrupted sampling even during total network or power outages.

---

## 📌 Example Use Cases

This monitor is especially useful for vehicles that sit for long periods and are more likely to develop battery issues over time:

* Watching any low-use vehicle that may slowly discharge while parked.
* Tracking the battery health of an old or vintage car where the battery is often the weak point.
* Monitoring other 6V-12V-24V battery-powered equipment such as motorcycles, scooters, boats, RVs, tractors, generators, and utility vehicles.
* Monitoring a battery while it is connected to a battery charger.

ThingSpeak can also be used to trigger an alert when battery voltage drops to a level where it is time to recharge, while ThingsBoard can be used for dashboards, rules, and telemetry replay.

Example live data from my spare car: [ThingSpeak Channel 3405145](https://thingspeak.mathworks.com/channels/3405145)

---

## 🚀 Key Features

* **Dual-Core Isolation (FreeRTOS):**
    * **Core 1 (`TaskMeasure`):** Dedicated entirely to strict, time-critical 10ms hardware ADC sampling.
    * **Core 0 (`TaskSend`):** Handles heavy, blocking network tasks (WiFi reconnection, NTP sync, HTTP POST payloads).
* **Zero-Loss Persistency (LittleFS):** If WiFi is down, data points are immediately appended as raw binary structures to the ESP32's onboard Flash memory.
* **Power-Loss Proof Time Tracking (NTP):** Uses Network Time Protocol to log data with absolute Unix Epoch timestamps. If booted offline, it maintains a precise relative timeline and auto-corrects/backdates points before cloud upload once the network recovers.
* **Timezone-Aware Local Logs:** Serial output uses a real timezone rule with automatic daylight saving time adjustment, while ThingSpeak uploads remain in UTC and ThingsBoard telemetry uses client-side timestamps in milliseconds.
* **Smart Bulk Uploading:** Uses ThingSpeak's Bulk Update JSON API and ThingsBoard's batched telemetry format to push up to 30 accumulated points at once, bypassing free-tier rate limits and avoiding RAM exhaustion.

---

## 🔌 Hardware Configuration

### Voltage Divider Circuit
Since the ESP32 ADC safe input limit is around **3.1V** (when configured with maximum 11dB attenuation), a voltage divider is required to safely scale down the battery input.

The divider used in this project is well suited for monitoring a **12V battery** and still leaves some headroom above the normal charging range.

* **Resistor 1 ($R_1$):** $330\text{ k}\Omega$
* **Resistor 2 ($R_2$):** $47\text{ k}\Omega$
* **Analog Pin:** GPIO 34 (ADC1)

$$\text{Attenuation Ratio} = \frac{R_1 + R_2}{R_2} = \frac{330\text{k} + 47\text{k}}{47\text{k}} \approx 8.0212$$

Maximum Measurable Voltage: $3.1\text{V} \times 8.0212 \approx 24.86\text{V}$.

```
                   GPIO 34
                      |
                      |
12V -----/\/\/\/------+-----/\/\/\/----- GND
          330k                47k
```

For a **24V battery system**, use a higher-ratio divider so the ESP32 still has safe headroom when the battery is charging. A simple alternative is:

* **Resistor 1 ($R_1$):** $470\text{ k}\Omega$
* **Resistor 2 ($R_2$):** $47\text{ k}\Omega$

This gives an attenuation ratio of about $11.00$ and raises the measurable range to roughly $34.1\text{V}$, which is a better fit for a 24V battery setup.

```
                   GPIO 34
                      |
                      |
24V -----/\/\/\/------+-----/\/\/\/----- GND
          470k                47k
```
---

## 💾 Installation & Setup

### 1. Prerequisites
Ensure you have the following libraries and configurations ready in your Arduino IDE / PlatformIO environment:
* **Board Manager:** ESP32 Arduino Core (v2.x or v3.x fully supported).
* **Filesystem:** `LittleFS` (built into the ESP32 core).

### 2. ThingSpeak / ThingsBoard Configuration
1. Log into your [ThingSpeak Account](https://thingspeak.com/).
2. Create a new **Channel** and enable **Field 1**.
3. Note your **Channel ID** and **Write API Key**.
4. If you want ThingsBoard reporting, create a device in your ThingsBoard tenant and copy the device access token.

### 3. Firmware Configuration
Open the source code file and update the configuration sections at the top:

```cpp
// WiFi Credentials
const char* ssid     = "Your_WiFi_SSID";
const char* password = "Your_WiFi_Password";

// Cloud Reporting Parameters
const bool ENABLE_THINGSPEAK = true;
const bool ENABLE_THINGSBOARD = false;

const char* writeApiKey = "YOUR_THINGSPEAK_WRITE_KEY";
const char* channelID   = "YOUR_CHANNEL_ID_NUMBER";
const char* thingsBoardServer = "https://thingsboard.cloud";
const char* thingsBoardToken  = "YOUR_THINGSBOARD_DEVICE_ACCESS_TOKEN";

// NTP Parameters (Adjust for your timezone rule)
const char* timeZone = "CET-1CEST,M3.5.0/2,M10.5.0/3"; // Example: CET/CEST with automatic DST

// Example alternatives:
// const char* timeZone = "EST5EDT,M3.2.0/2,M11.1.0/2"; // US Eastern with automatic DST
// const char* timeZone = "UTC0"; // No daylight saving time
```
---

## 🛠️ Software Architecture

Data travels safely across cores and into the cloud through the following pipeline:
```
[VOLTAGE INPUT]
│
▼  (Core 1 - Precision 10ms Timer Loop)
┌────────────────────────────────────────┐
│ TaskMeasure: Continuous ADC Sampling   │
└────────────────────────────────────────┘
│
▼  (Averages 1500 samples every 15 seconds)
┌────────────────────────────────────────┐
│ FreeRTOS Inter-Task Queue (Safe Pipe)  │
└────────────────────────────────────────┘
│
▼  (Core 0 - Asynchronous Network Core)
┌────────────────────────────────────────┐
│ TaskSend: Writes immediately to Flash  │
└────────────────────────────────────────┘
│
├──► [WiFi Offline] ──► Keep buffering in LittleFS. Retry WiFi silently.
│
└──► [WiFi Online]  ──► Sync NTP Clock ──► Cloud Uploads ──► [ThingSpeak and/or ThingsBoard]
```
---

# Fault Tolerance Behavior Matrix

This document outlines how the ESP32 Voltage Monitor responds to various network, power, and infrastructure anomalies while maintaining high-frequency data sampling integrity.

| Scenario | System State | Hardware Sampling Core (Core 1) | Network & Storage Core (Core 0) | Data Preservation | Recovery Action |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **Normal Operation** | Online | Samples ADC every 10ms. Queues 15-second average voltage. | Translates data to absolute timestamps and uploads to whichever cloud targets are enabled. | Real-time transmission. Local storage file remains empty. | N/A |
| **WiFi Access Point Disconnected** | Offline | Continues uninterrupted 10ms sampling and 15s averaging. | Appends raw data structure to `/offline_data.bin` in LittleFS Flash memory. Initiates a non-blocking `WiFi.begin()` loop. | **100% Preserved** locally on internal flash memory. | Periodically checks WiFi status without stalling the system. |
| **ThingSpeak or ThingsBoard Cloud Down** | Offline | Continues uninterrupted 10ms sampling and 15s averaging. | Detects failed HTTP response from the enabled cloud target(s). Leaves data intact within LittleFS. | **100% Preserved** locally on internal flash memory. | Retries transmission on the next 15-second loop iteration. |
| **Power Loss during Offline Phase** | Critical Failure | Hardware turns off. | Hardware turns off. | **100% Preserved** up to the last completed 15-second mark. LittleFS is non-volatile. | On reboot, reads current NTP time. If internet is unavailable, constructs a reliable relative timeline based on internal milestones. |
| **WiFi Connection Re-established** | Recovery | Continues uninterrupted 10ms sampling and 15s averaging. | Fetches real-world time via pool.ntp.org. Extracts up to 30 past data points from Flash, matches them to absolute time metrics, and sends the batch to each enabled cloud target. | **100% Recovered**. History is populated chronologically. | Deletes processed entries from the LittleFS binary file after all enabled uploads succeed. |

---

### Core Isolation Visual Design

The absolute stability of this fault tolerance architecture relies on FreeRTOS task prioritization and core pinning:

* **Core 1 (High Priority Task):** Strictly isolated from network instabilities. It will never drop a sample because of a slow server response or poor WiFi signals.
* **Core 0 (Low Priority Task):** Designed to handle network bottlenecks, delays, or blocking socket requests without interfering with Core 1 operations.

---

### Operational Notes

- **Time handling & DST:** Configure the `timeZone` POSIX string in `CarBatteryMonitor.ino` to match your locale (examples are included in the source). The device calls `configTzTime()` and prints local timestamps on Serial with automatic daylight saving adjustments; ThingSpeak uploads are produced from UTC (`gmtime()`), while ThingsBoard telemetry uses Unix milliseconds in the `ts` field.

- **Fallback timestamps & correction on sync:** If the device boots without network/NTP, it stores measurements immediately to LittleFS using a sentinel fallback epoch (`FALLBACK_EPOCH = 1`) and keeps incrementing a local counter to preserve ordering. When NTP time becomes available, the firmware computes an offset and applies it only to placeholder-era records (those with timestamp < `MIN_VALID_EPOCH`, currently `1700000000`) so already-valid timestamps are not rebased. Corrected points are then uploaded in chronological order.

- **Upload gating until NTP locked:** To avoid sending placeholder timestamps or creating duplicate/far-future timestamps, the firmware will buffer data locally and will not attempt ThingSpeak uploads until `isTimeSynchronized()` confirms a valid NTP clock.

- **Boot-button flash delete:** If you need to clear the offline buffer, hold the BOOT button (GPIO0) while powering on / resetting the device. On startup the firmware checks that pin and will delete `/offline_data.bin` from LittleFS when detected.

- **LED indicator semantics:** The onboard status LED indicates network/time state independently of the 15s sampling loop:
    - Slow blink (~500 ms on/off): WiFi is offline.
    - Fast blink (~250 ms on/off): WiFi is connected but NTP time sync is pending.
    - Steady off: Device fully online (WiFi + NTP) and operating normally. During HTTP POST the LED briefly pulses HIGH to signal an upload in progress.

- **Offline storage file:** The LittleFS binary buffer is stored at `/offline_data.bin`. Processed records are removed or truncated only after all enabled cloud uploads succeed.

- **Where to change divider / ADC constants:** The voltage divider and ADC scaling constants are defined in `CarBatteryMonitor.ino` (look for `DIVIDER_RATIO` and the resistor value comments). For 12V systems the shipped values are `R1=330k, R2=47k`; for 24V systems use the suggested `R1=470k, R2=47k` alternative.

---