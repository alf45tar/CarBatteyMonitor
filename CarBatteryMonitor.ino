#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <LittleFS.h>
#include "time.h" // Built-in ESP32 Time library

//====================================================
// WIFI & CLOUD REPORTING CONFIGURATION
//====================================================
const char* ssid     = "MyWireless";     // Replace with your WiFi SSID
const char* password = "1234567890";     // Replace with your WiFi password

const bool ENABLE_THINGSPEAK = true;     // Set true to enable ThingSpeak uploads
const bool ENABLE_THINGSBOARD = true;    // Set true to enable ThingsBoard uploads

const char* writeApiKey = "XXXXXXXXXXXXXXXX";   // Replace with your ThingSpeak Write API Key
const char* channelID   = "0000000";            // Replace with your ThingSpeak Channel ID
const char* server      = "http://api.thingspeak.com/channels/";

const char* thingsBoardServer = "https://eu.thingsboard.cloud";  // Replace with your ThingsBoard host
const char* thingsBoardToken  = "XXXXXXXXXXXXXXXXXXXX";          // Replace with your ThingsBoard Device Access Token

//====================================================
// NTP SERVER CONFIGURATION
//====================================================
const char* ntpServer = "pool.ntp.org";                 // NTP server for time synchronization
const char* timeZone = "CET-1CEST,M3.5.0/2,M10.5.0/3";  // Central Europe with automatic DST (e.g. EST5EDT,M3.2.0/2,M11.1.0/2 for US Eastern)

//====================================================
// HARDWARE & CONSTANTS
//====================================================
const int analogPin = 34;                               // GPIO34 (ADC1_CH6) for battery voltage measurement
const float DIVIDER_RATIO = (330.0f + 47.0f) / 47.0f;   // Voltage divider ratio for scaling down battery voltage to ADC range

const uint32_t SAMPLE_INTERVAL_MS = 10;
const uint32_t THINGSPEAK_INTERVAL_MS = 15000;  // 15 seconds to comply with ThingSpeak free tier limits and allow for multiple records in bulk upload
const time_t FALLBACK_EPOCH = 1;                // small non-zero sentinel when NTP is unavailable
const time_t MIN_VALID_EPOCH = 1700000000;      // Treat anything older as boot-time placeholder data
const int BOOT_BUTTON_PIN = 0;                  // GPIO0 is typically the BOOT button on ESP32 dev boards

//====================================================
// FREERTOS & FILE SYSTEM CONFIGURATION
//====================================================
QueueHandle_t voltageQueue;
const char* DATA_FILE = "/offline_data.bin";

// Binary structure saved directly to flash storage with absolute epoch time
struct DataPoint {
    float voltage;
    time_t timestamp; // Absolute Unix Epoch Time (seconds since 1970)
};

void TaskMeasure(void *pvParameters);
void TaskSend(void *pvParameters);
void TaskBlinkOffline(void *pvParameters);
void handleBootButtonFlashDelete();
bool isTimeSynchronized();
bool sendThingSpeakBatch(const DataPoint* records, size_t numRecords);
bool sendThingsBoardBatch(const DataPoint* records, size_t numRecords);
String buildThingSpeakPayload(const DataPoint* records, size_t numRecords);
String buildThingsBoardPayload(const DataPoint* records, size_t numRecords);

//====================================================
// UTILITY: GET CURRENT UNIX TIMESTAMP
//====================================================
time_t getEpochTime() {
    time_t now;
    struct tm timeinfo;
    if (!getLocalTime(&timeinfo)) {
        return 0; // Return 0 if time synchronization hasn't happened yet
    }
    time(&now);
    return now;
}

bool isTimeSynchronized() {
    struct tm timeinfo;
    return getLocalTime(&timeinfo);
}

//====================================================
// UTILITY: CONVERT UNIX TIMESTAMP TO ISO 8601 String
//====================================================
String getISO8601Time(time_t epochTime) {
    struct tm *timeinfo;
    timeinfo = gmtime(&epochTime); // Convert to UTC time for ThingSpeak
    char buffer[25];
    // Format: YYYY-MM-DDTHH:MM:SSZ
    strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", timeinfo);
    return String(buffer);
}

//====================================================
// UTILITY: CONVERT UNIX TIMESTAMP TO LOCAL TIME String
//====================================================
String getLocalTimeString(time_t epochTime) {
    struct tm localTimeInfo;
    localtime_r(&epochTime, &localTimeInfo);
    char buffer[25];
    // Format: YYYY-MM-DD HH:MM:SS
    strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &localTimeInfo);
    return String(buffer);
}

//====================================================
// UTILITY: DECODE HTTP STATUS CODE
//====================================================
String getHttpStatusMessage(int httpCode) {
    switch (httpCode) {
        case 200: return "OK";
        case 201: return "Created";
        case 202: return "Accepted";
        case 204: return "No Content";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 413: return "Payload Too Large";
        case 429: return "Too Many Requests";
        case 500: return "Internal Server Error";
        case 502: return "Bad Gateway";
        case 503: return "Service Unavailable";
        default: return "Unknown Status";
    }
}

//====================================================
// UTILITY: BUILD THINGSPEAK BULK PAYLOAD
//====================================================
String buildThingSpeakPayload(const DataPoint* records, size_t numRecords) {
    String jsonPayload = "{\"write_api_key\":\"" + String(writeApiKey) + "\",\"updates\":[";
    for (size_t i = 0; i < numRecords; i++) {
        jsonPayload += "{\"created_at\":\"" + getISO8601Time(records[i].timestamp) + "\",";
        jsonPayload += "\"field1\":\"" + String(records[i].voltage, 3) + "\"}";
        if (i < numRecords - 1) jsonPayload += ",";
    }
    jsonPayload += "]}";
    return jsonPayload;
}

//====================================================
// UTILITY: BUILD THINGSBOARD TELEMETRY PAYLOAD
//====================================================
String buildThingsBoardPayload(const DataPoint* records, size_t numRecords) {
    String jsonPayload = "[";
    for (size_t i = 0; i < numRecords; i++) {
        char timestampBuffer[24];
        snprintf(timestampBuffer, sizeof(timestampBuffer), "%lld", (long long)records[i].timestamp * 1000LL);

        jsonPayload += "{\"ts\":";
        jsonPayload += timestampBuffer;
        jsonPayload += ",\"values\":{\"voltage\":";
        jsonPayload += String(records[i].voltage, 3);
        jsonPayload += "}}";

        if (i < numRecords - 1) jsonPayload += ",";
    }
    jsonPayload += "]";
    return jsonPayload;
}

//====================================================
// UTILITY: SEND THINGSPEAK BATCH
//====================================================
bool sendThingSpeakBatch(const DataPoint* records, size_t numRecords) {
    if (!ENABLE_THINGSPEAK) {
        return true;
    }

    String jsonPayload = buildThingSpeakPayload(records, numRecords);

    digitalWrite(LED_BUILTIN, HIGH);
    HTTPClient http;
    String url = String(server) + String(channelID) + "/bulk_update.json";
    http.begin(url);
    http.addHeader("Content-Type", "application/json");

    int httpCode = http.POST(jsonPayload);
    String responseBody = http.getString();
    digitalWrite(LED_BUILTIN, LOW);

    Serial.print("[ThingSpeak] HTTP ");
    Serial.print(httpCode);
    Serial.print(" ");
    Serial.print(getHttpStatusMessage(httpCode));
    Serial.print(" ");
    Serial.println(responseBody);

    http.end();
    return httpCode == 202;
}

//====================================================
// UTILITY: SEND THINGSBOARD BATCH
//====================================================
bool sendThingsBoardBatch(const DataPoint* records, size_t numRecords) {
    if (!ENABLE_THINGSBOARD) {
        return true;
    }

    String jsonPayload = buildThingsBoardPayload(records, numRecords);

    digitalWrite(LED_BUILTIN, HIGH);
    WiFiClientSecure secureClient;
    secureClient.setInsecure();

    HTTPClient http;
    String url = String(thingsBoardServer) + "/api/v1/" + String(thingsBoardToken) + "/telemetry";
    http.begin(secureClient, url);
    http.addHeader("Content-Type", "application/json");

    int httpCode = http.POST(jsonPayload);
    String responseBody = http.getString();
    digitalWrite(LED_BUILTIN, LOW);

    Serial.print("[ThingsBoard] HTTP ");
    Serial.print(httpCode);
    Serial.print(" ");
    Serial.print(getHttpStatusMessage(httpCode));
    Serial.print(" ");
    Serial.println(responseBody);

    http.end();
    return httpCode >= 200 && httpCode < 300;
}

//====================================================
// UTILITY: BOOT BUTTON FLASH DELETE
//====================================================
void handleBootButtonFlashDelete() {
    pinMode(BOOT_BUTTON_PIN, INPUT_PULLUP);

    if (digitalRead(BOOT_BUTTON_PIN) == LOW) {
        delay(50);
        if (digitalRead(BOOT_BUTTON_PIN) == LOW) {
            if (LittleFS.exists(DATA_FILE)) {
                if (LittleFS.remove(DATA_FILE)) {
                    Serial.println("[Flash] Boot button held. Stored data deleted.");
                } else {
                    Serial.println("[Flash] Boot button held, but delete failed.");
                }
            } else {
                Serial.println("[Flash] Boot button held, but no stored data file found.");
            }
        }
    }
}

//====================================================
// WIFI & NTP SETUP (Blocking Version for Setup Only)
//====================================================
void initNetwork() {
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, password);
    Serial.print("Connecting to WiFi");
    uint32_t startTime = millis();

    while (WiFi.status() != WL_CONNECTED) {
        vTaskDelay(pdMS_TO_TICKS(500));
        Serial.print(".");
        if (millis() - startTime > 10000) {
            Serial.println("\nWiFi connection timeout. Booting to offline tracking...");
            return;
        }
    }
    Serial.println("\nWiFi connected!");

    // Initialize and sync time via NTP
    Serial.println("Synchronizing time via NTP...");
    configTzTime(timeZone, ntpServer);
    
    struct tm timeinfo;
    if(getLocalTime(&timeinfo)){
        time_t now;
        time(&now);
        Serial.print("Time synchronized! Current local time: ");
        Serial.println(getLocalTimeString(now));
    } else {
        Serial.println("NTP Sync Failed. Internal clock will approximate time until online.");
    }
}

//====================================================
// SETUP
//====================================================
void setup() {
    Serial.begin(115200);
    delay(1000);

    pinMode(LED_BUILTIN, OUTPUT);
    digitalWrite(LED_BUILTIN, LOW);

    if (!LittleFS.begin(true)) {
        Serial.println("LittleFS Mount Failed!");
        return;
    }

    handleBootButtonFlashDelete();

    analogReadResolution(12);
#if ESP_ARDUINO_VERSION_MAJOR >= 3
    analogSetPinAttenuation(analogPin, ADC_ATTENDB_MAX);
#else
    analogSetPinAttenuation(analogPin, ADC_11db);
#endif

    initNetwork();

    voltageQueue = xQueueCreate(10, sizeof(float));

    if (voltageQueue != NULL) {
        xTaskCreatePinnedToCore(TaskMeasure, "Measure", 4096, NULL, 2, NULL, 1);
        xTaskCreatePinnedToCore(TaskSend, "Send", 12288, NULL, 1, NULL, 0); // Stack bumped to 12k for dynamic time strings
        xTaskCreatePinnedToCore(TaskBlinkOffline, "BlinkOffline", 2048, NULL, 1, NULL, 1);
        Serial.println("FreeRTOS Tasks initialized on dual cores with NTP.");
    }
}

void loop() {
    vTaskDelete(NULL); 
}

//====================================================
// TASK 1: MEASURING & AVERAGING (Runs on Core 1)
//====================================================
void TaskMeasure(void *pvParameters) {
    TickType_t xLastWakeTime = xTaskGetTickCount();
    
    uint64_t adcAccumulator = 0;
    uint32_t adcSamples = 0;
    uint32_t lastUploadCheck = millis();

    while (1) {
        vTaskDelayUntil(&xLastWakeTime, pdMS_TO_TICKS(SAMPLE_INTERVAL_MS));

        uint32_t adc_mV = analogReadMilliVolts(analogPin);
        adc_mV = adc_mV < 150 ? 0 : adc_mV;
        
        adcAccumulator += adc_mV;
        adcSamples++;

        if (millis() - lastUploadCheck >= THINGSPEAK_INTERVAL_MS) {
            lastUploadCheck = millis();

            if (adcSamples > 0) {
                float adcVoltage = ((float)adcAccumulator / adcSamples) / 1000.0f;
                float inputVoltage = round(adcVoltage * DIVIDER_RATIO * 100) / 100.0f;

                if (xQueueSend(voltageQueue, &inputVoltage, 0) != pdPASS) {
                    Serial.println("[TaskMeasure Error] Queue full!");
                }

                adcAccumulator = 0;
                adcSamples = 0;
            }
        }
    }
}

//====================================================
// TASK 2: PERSISTENT STORAGE & SENDING (Runs on Core 0)
//====================================================
void TaskSend(void *pvParameters) {
    float receivedVoltage = 0.0f;
    time_t lastKnownTime = 0;

    while (1) {
        if (xQueueReceive(voltageQueue, &receivedVoltage, portMAX_DELAY) == pdPASS) {
            
            Serial.println("--------------------------------");
            Serial.print("[TaskSend] New Reading: "); Serial.print(receivedVoltage, 3); Serial.println(" V");

            // Fetch absolute time. If offline and clock isn't set, increment from last known time
            time_t currentTime = getEpochTime();
            if (currentTime == 0) { 
                if (lastKnownTime == 0) {
                    // Fallback to a small non-zero sentinel if device booted up completely blind
                    lastKnownTime = FALLBACK_EPOCH;
                } else {
                    lastKnownTime += (THINGSPEAK_INTERVAL_MS / 1000);
                }
                currentTime = lastKnownTime;
            } else {
                lastKnownTime = currentTime; // Update baseline
            }

            // 1. SAVE TO FLASH IMMEDIATELY
            File file = LittleFS.open(DATA_FILE, FILE_APPEND);
            if (file) {
                DataPoint point;
                point.voltage = receivedVoltage;
                point.timestamp = currentTime; 
                
                file.write((uint8_t*)&point, sizeof(DataPoint));
                file.close();
                Serial.print("[Flash] Saved with Local Time: "); Serial.println(getLocalTimeString(currentTime));
            }

            // 2. CHECK CONNECTION & SYNC
            if (WiFi.status() != WL_CONNECTED) { 
                Serial.println("[Network] Offline. Retaining data in flash...");
                WiFi.begin(ssid, password); // Non-blocking attempt
            } 
            else {
                // Quickly verify NTP hasn't slipped if network just recovered
                configTzTime(timeZone, ntpServer);

                if (!isTimeSynchronized()) {
                    Serial.println("[Network] WiFi online, but NTP is not ready yet. Keeping data in flash...");
                    continue;
                }

                Serial.println("[Network] Online. Processing payload using absolute timestamps...");

                File file = LittleFS.open(DATA_FILE, FILE_READ);
                if (!file || file.size() == 0) {
                    if(file) file.close();
                    continue;
                }

                size_t numRecords = file.size() / sizeof(DataPoint);
                if (numRecords > 30) numRecords = 30; // Safety threshold for dynamic string allocations

                DataPoint* syncBuffer = new DataPoint[numRecords];
                file.read((uint8_t*)syncBuffer, numRecords * sizeof(DataPoint));
                file.close();

                // If we just recovered a real NTP time, compute offset from the
                // last known offline timestamp and apply it to any records that look like
                // boot-time placeholders so their epochs are corrected before upload.
                time_t realNow = getEpochTime();
                if (realNow != 0 && lastKnownTime != 0) {
                    time_t offset = realNow - lastKnownTime;
                    if (offset != 0) {
                        for (size_t i = 0; i < numRecords; i++) {
                            if (syncBuffer[i].timestamp < MIN_VALID_EPOCH) {
                                syncBuffer[i].timestamp = syncBuffer[i].timestamp + offset;
                            }
                        }
                        Serial.print("[TimeSync] Applied offset (s): "); Serial.println((long)offset);
                    }
                }

                Serial.println("[Storage] Sync data:");
                for (size_t i = 0; i < numRecords; i++) {
                    Serial.print("  #");
                    Serial.print(i + 1);
                    Serial.print(" voltage=");
                    Serial.print(syncBuffer[i].voltage, 2);
                    Serial.print(" V, local=");
                    Serial.print(getLocalTimeString(syncBuffer[i].timestamp));
                    Serial.print(", utc=");
                    Serial.println(getISO8601Time(syncBuffer[i].timestamp));
                }

                bool reportingAttempted = false;
                bool allReportsSucceeded = true;

                if (ENABLE_THINGSPEAK) {
                    reportingAttempted = true;
                    allReportsSucceeded = sendThingSpeakBatch(syncBuffer, numRecords) && allReportsSucceeded;
                }

                if (ENABLE_THINGSBOARD) {
                    reportingAttempted = true;
                    allReportsSucceeded = sendThingsBoardBatch(syncBuffer, numRecords) && allReportsSucceeded;
                }

                if (!reportingAttempted) {
                    Serial.println("[Config] No cloud reporting method is enabled. Data will stay in flash.");
                    delete[] syncBuffer;
                    continue;
                }

                if (allReportsSucceeded) {
                    
                    // Cleanup handled records
                    File file = LittleFS.open(DATA_FILE, FILE_READ);
                    size_t totalSize = file.size();
                    size_t bytesSynced = numRecords * sizeof(DataPoint);
                    
                    if (totalSize > bytesSynced) {
                        size_t remainingBytes = totalSize - bytesSynced;
                        uint8_t* remainderBuf = new uint8_t[remainingBytes];
                        file.seek(bytesSynced);
                        file.read(remainderBuf, remainingBytes);
                        file.close();

                        file = LittleFS.open(DATA_FILE, FILE_WRITE);
                        file.write(remainderBuf, remainingBytes);
                        file.close();
                        delete[] remainderBuf;
                        Serial.println("[Flash] Partial clean complete.");
                    } else {
                        file.close();
                        LittleFS.remove(DATA_FILE);
                        Serial.println("[Flash] All clear.");
                    }
                }

                delete[] syncBuffer;
            }
        }
    }
}

//====================================================
// TASK 3: OFFLINE LED BLINKER (Runs on Core 1)
//====================================================
void TaskBlinkOffline(void *pvParameters) {
    bool ledState = false;

    while (1) {
        if (WiFi.status() != WL_CONNECTED) {
            ledState = !ledState;
            digitalWrite(LED_BUILTIN, ledState ? HIGH : LOW);
            vTaskDelay(pdMS_TO_TICKS(500));
        } else if (!isTimeSynchronized()) {
            ledState = !ledState;
            digitalWrite(LED_BUILTIN, ledState ? HIGH : LOW);
            vTaskDelay(pdMS_TO_TICKS(250));
        } else {
            ledState = false;
            digitalWrite(LED_BUILTIN, LOW);
            vTaskDelay(pdMS_TO_TICKS(200));
        }
    }
}