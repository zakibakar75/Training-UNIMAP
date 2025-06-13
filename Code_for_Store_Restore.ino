#include <WiFi.h>
#include <SPI.h>
#include <SD.h>
#include <RTClib.h>
#include <DHT.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <ArduinoJson.h>
#include <PubSubClient.h>

#define DHTPIN 4
#define DHTTYPE DHT11
#define SD_CS 5
#define SDA_PIN 21
#define SCL_PIN 22

DHT dht(DHTPIN, DHTTYPE);
RTC_DS1307 rtc;
WiFiUDP c;
NTPClient timeClient(c, "asia.pool.ntp.org", 0, 86400000);
WiFiClient espClient;
PubSubClient mqttClient(espClient);

const char* ssid = "MyRouter4";
const char* password = "mimosian";
const char* mqttServer = "cap.mimos.my";
const int mqttPort = 1883;
const char* mqttToken = "DBS055DbzuCyc8ob6tWZ";
const char* mqttTopic = "v1/devices/me/telemetry";
const char* dataFile = "/datalog.txt";
const char* dir = "/data/";
const char* sentFile = "/data/sent.txt";

unsigned long lastNTPSyncTime = 0;
unsigned long lastLogTime = 0;
const unsigned long logInterval = 10000; // 10 seconds
const int maxPublishRetries = 3;
const int maxNTPSyncRetries = 3;
const int maxSdRetries = 3;
bool sdAvailable = false;
bool ntpSyncedThisCycle = false;
const long timeZoneOffset = 28800; // +08:00
bool lastWiFiState = false;

// Non-blocking WiFi reconnect variables
enum WiFiState { WIFI_IDLE, WIFI_CONNECTING, WIFI_CHECK_INTERNET };
WiFiState wifiState = WIFI_IDLE;
unsigned long wifiStartTime = 0;
const unsigned long wifiTimeout = 15000; // 15 seconds
const unsigned long wifiCheckInterval = 100; // Check every 100ms
const unsigned long wifiRetryInterval = 30000; // Retry every 30 seconds if offline
unsigned long lastWiFiRetryTime = 0;
unsigned long lastWiFiCheckTime = 0;
const unsigned long wifiIdleCheckInterval = 1000; // Check every 1 second in idle
unsigned long lastDotPrintTime = 0; // Track last dot print
const unsigned long dotPrintInterval = 1000; // Print dot every 1 second
int wifiResetCount = 0;
const int maxWiFiResets = 5; // Limit resets per cycle
int lastWiFiStatus = -1; // Track last WiFi status for logging

void setup() {
      Serial.begin(115200);
      Serial.println("Booting...");
      Serial.printf("Free heap: %u bytes\n", ESP.getFreeHeap());

      dht.begin();

      Wire.begin(SDA_PIN, SCL_PIN);
      if (!rtc.begin()) {
          Serial.println("RTC initialization failed!");
          while (true) delay(1000);
      }

      if (!rtc.isrunning()) {
          rtc.adjust(DateTime(2025, 6, 11, 8, 3, 0)); // June 11, 2025, 08:03:00 UTC
          Serial.println("Set default time: 2025-06-11 08:03:00 UTC");
      }

      Serial.println("Initializing SD card...");
      int sdRetries = 0;
      while (!SD.begin(SD_CS) && sdRetries < maxSdRetries) {
          Serial.printf("SD init attempt %d failed!\n", sdRetries + 1);
          delay(1000);
          sdRetries++;
      }

      if (SD.begin(SD_CS)) {
          sdAvailable = true;
          Serial.println("SD card initialized!");
          if (!SD.exists(dir)) {
              if (!SD.mkdir(dir)) {
                Serial.println("Directory creation failed!");
                sdAvailable = false;
              } else {
                Serial.println("Directory created!");
              }
          }
          File file = SD.open(sentFile, FILE_WRITE);
          if (file) {
              file.close();
              Serial.println("sent.txt initialized!");
          } else {
              Serial.println("Error opening sent.txt!");
              sdAvailable = false;
          }
      } else {
          Serial.println("All SD init attempts failed, proceeding offline!");
          sdAvailable = false;
      }

      startWiFiConnect();
      mqttClient.setServer(mqttServer, mqttPort);
}

void loop() {
      unsigned long currentMillis = millis();
  
      // Handle WiFi reconnection
      handleWiFiReconnect(currentMillis);

      // Check MQTT connection
      if (isWiFiConnected() && !mqttClient.connected()) {
          reconnectMQTT();
      }
      
      mqttClient.loop();

      // Handle NTP sync
      DateTime now = rtc.now();
      unsigned long unixTime = now.unixtime();
      if (isWiFiConnected() && !ntpSyncedThisCycle && 
          (lastNTPSyncTime == 0 || (unixTime - lastNTPSyncTime) >= 21600)) {
          syncNTP();
          ntpSyncedThisCycle = true;
      }

      // Log data every 10 seconds
      if (currentMillis - lastLogTime >= logInterval) {
          Serial.printf("Free heap before logging: %u bytes\n", ESP.getFreeHeap());
          //float temp = dht.readTemperature();
          float temp = 24.8; // Uncomment for testing
          if (isnan(temp)) {
              Serial.println("DHT read failed!");
              temp = -999;
          }

          now = rtc.now();
          unixTime = now.unixtime();
          Serial.printf("RTC read: Unix: %u, Year: %u, Local: %s\n", unixTime, now.year(), formatLocalTime(now).c_str());
          bool invalid = false;
    
          if (now.year() < 2025) {
              Serial.printf("Invalid RTC: Year < 2025 (%u)\n", now.year());
              invalid = true;
          } else if (unixTime < 1704067200) {
              Serial.printf("Invalid RTC: Unix < Jan 1, 2024 (%u)\n", unixTime);
              invalid = true;
          } else if (now.year() > 2026) {
              Serial.printf("Invalid RTC: Year > 2026 (%u)\n", now.year());
              invalid = true;
          } else if (lastNTPSyncTime > 0 && (unixTime - lastNTPSyncTime) > 3600 && abs((long)(unixTime - lastNTPSyncTime)) > 604800) {
              Serial.printf("Invalid RTC: Drift > 7 days (Unix: %u, Last NTP: %u)\n", unixTime, lastNTPSyncTime);
              invalid = true;
          }

          if (invalid && isWiFiConnected() && !ntpSyncedThisCycle) {
              Serial.println("Invalid RTC time, re-syncing!");
              syncNTP();
              ntpSyncedThisCycle = true;
              now = rtc.now();
              unixTime = now.unixtime();
              Serial.printf("RTC re-read: Unix: %u, Year: %u, Local: %s\n", unixTime, now.year(), formatLocalTime(now).c_str());
          }

          StaticJsonDocument<128> doc;
          doc["ts"] = (uint64_t)unixTime * 1000;
          JsonObject values = doc.createNestedObject("values");
          values["temperature"] = temp;
          char dataString[128];
          serializeJson(doc, dataString, sizeof(dataString));
          Serial.printf("MQTT JSON: %s\n", dataString);
          
          StaticJsonDocument<128> logDoc;
          logDoc["timestamp"] = formatLocalTime(now);
          logDoc["temperature"] = temp;
          char logString[128];
          serializeJson(logDoc, logString, sizeof(logString));
    
          if (!isWiFiConnected() || !mqttClient.connected()) {
              if (sdAvailable) {
                  File dataFileHandle = SD.open(dataFile, FILE_APPEND);
                  if (dataFileHandle) {
                      dataFileHandle.println(logString);
                      dataFileHandle.close();
                      Serial.printf("SD saved: %s\n", logString);
                  } else {
                      Serial.println("Error opening datalog.txt!");
                      sdAvailable = false;
                  }
              } else {
                  Serial.println("SD unavailable, skipping logging!");
              }
          } else {
              if (sdAvailable) {
                sendStoredData();
              }
              publishData(dataString);
          }

          lastLogTime = currentMillis;
          ntpSyncedThisCycle = false;
          Serial.printf("Free heap after logging: %u bytes\n", ESP.getFreeHeap());
      }
}

bool isWiFiConnected() {
  int currentStatus = WiFi.status();
  if (currentStatus != WL_CONNECTED) {
    if (currentStatus != lastWiFiStatus) {
      Serial.printf("WiFi not connected, status: %d\n", currentStatus);
      lastWiFiStatus = currentStatus;
    }
    return false;
  }
  WiFiClient client;
  if (!client.connect("8.8.8.8", 53)) {
    if (lastWiFiStatus != 255) { // Custom code for no internet
      Serial.println("WiFi connected but no internet access!");
      lastWiFiStatus = 255;
    }
    return false;
  }
  client.stop();
  if (lastWiFiStatus != WL_CONNECTED) {
    Serial.println("WiFi connected with internet access!");
    lastWiFiStatus = WL_CONNECTED;
  }
  return true;
}

void startWiFiConnect() {
  if (wifiState != WIFI_IDLE || wifiResetCount >= maxWiFiResets) {
    if (wifiResetCount >= maxWiFiResets) {
      Serial.println("Max WiFi resets reached, staying offline!");
    }
    return;
  }
  Serial.printf("Starting WiFi connect (reset %d/%d)...\n", wifiResetCount + 1, maxWiFiResets);
  WiFi.persistent(false); // Disable persistent credentials
  WiFi.disconnect(true); // Force disconnect
  WiFi.mode(WIFI_OFF); // Turn off WiFi
  delay(2000); // Increased delay for antenna re-attachment
  WiFi.mode(WIFI_STA); // Set station mode
  int status = WiFi.begin(ssid, password);
  Serial.printf("WiFi.begin status: %d\n", status);
  wifiState = WIFI_CONNECTING;
  wifiStartTime = millis(); // Set start time here
  lastDotPrintTime = wifiStartTime; // Initialize dot timer
  wifiResetCount++;
  Serial.println("Connecting to WiFi...");
}

void handleWiFiReconnect(unsigned long currentMillis) {
  // Debug state
  static unsigned long lastDebugTime = 0;
  if (currentMillis - lastDebugTime >= 5000) {
    Serial.printf("WiFi state: %d, lastRetry: %u, resetCount: %d\n", 
                  wifiState, lastWiFiRetryTime, wifiResetCount);
    lastDebugTime = currentMillis;
  }

  // Periodic retry if offline for too long
  if (!isWiFiConnected() && wifiState == WIFI_IDLE && 
      (lastWiFiRetryTime == 0 || currentMillis - lastWiFiRetryTime >= wifiRetryInterval)) {
    Serial.println("Periodic WiFi retry...");
    wifiResetCount = 0; // Reset counter for new attempt
    startWiFiConnect();
    lastWiFiRetryTime = currentMillis;
    return;
  }

  if (wifiState == WIFI_IDLE) {
    if (currentMillis - lastWiFiCheckTime >= wifiIdleCheckInterval) {
      if (!isWiFiConnected()) {
        Serial.println("WiFi disconnected, attempting reconnect...");
        startWiFiConnect();
      }
      lastWiFiCheckTime = currentMillis;
    }
    return;
  }

  if (wifiState == WIFI_CONNECTING) {
    if (currentMillis - wifiStartTime >= wifiCheckInterval) {
      int currentStatus = WiFi.status();
      if (currentStatus == WL_CONNECTED) {
        wifiState = WIFI_CHECK_INTERNET;
        if (currentMillis - lastDotPrintTime >= dotPrintInterval) {
          Serial.print(".");
          lastDotPrintTime = currentMillis;
        }
      } else if (currentStatus == WL_NO_SSID_AVAIL) {
        Serial.printf("\nNo SSID available! Status: %d\n", currentStatus);
        WiFi.disconnect(true);
        WiFi.mode(WIFI_OFF); // Reset WiFi stack
        wifiState = WIFI_IDLE;
        wifiResetCount = max(0, wifiResetCount - 1);
        lastWiFiRetryTime = currentMillis; // Schedule next retry
        lastWiFiCheckTime = currentMillis;
        lastDotPrintTime = currentMillis;
      } else if (currentMillis - wifiStartTime >= wifiTimeout) {
        Serial.printf("\nWiFi connect failed! Status: %d\n", currentStatus);
        WiFi.disconnect(true);
        WiFi.mode(WIFI_OFF); // Reset WiFi stack
        wifiState = WIFI_IDLE;
        wifiResetCount = max(0, wifiResetCount - 1);
        lastWiFiRetryTime = currentMillis; // Schedule next retry
        lastWiFiCheckTime = currentMillis;
        lastDotPrintTime = currentMillis;
      } else {
        if (currentMillis - lastDotPrintTime >= dotPrintInterval) {
          Serial.printf(". [Status: %d]", currentStatus);
          lastDotPrintTime = currentMillis;
        }
      }
    }
    // Force exit if stuck
    if (currentMillis - wifiStartTime >= wifiTimeout) {
      Serial.println("\nWiFi stuck in CONNECTING, forcing IDLE!");
      WiFi.disconnect(true);
      WiFi.mode(WIFI_OFF);
      wifiState = WIFI_IDLE;
      wifiResetCount = max(0, wifiResetCount - 1);
      lastWiFiRetryTime = currentMillis;
      lastWiFiCheckTime = currentMillis;
      lastDotPrintTime = currentMillis;
    }
  } else if (wifiState == WIFI_CHECK_INTERNET) {
    WiFiClient client;
    if (client.connect("8.8.8.8", 53)) {
      client.stop();
      Serial.printf("\nWiFi connected! IP: %s\n", WiFi.localIP().toString().c_str());
      Serial.printf("DNS: %s\n", WiFi.dnsIP().toString().c_str());
      Serial.printf("RSSI: %d dBm\n", WiFi.RSSI());
      wifiState = WIFI_IDLE;
      lastWiFiState = true;
      wifiResetCount = 0; // Reset on success
      lastWiFiRetryTime = 0; // Clear retry timer
      lastWiFiCheckTime = currentMillis;
      lastDotPrintTime = currentMillis;
    } else {
      Serial.println("\nWiFi connected but no internet access!");
      WiFi.disconnect(true);
      WiFi.mode(WIFI_OFF); // Reset WiFi stack
      wifiState = WIFI_IDLE;
      wifiResetCount = max(0, wifiResetCount - 1);
      lastWiFiRetryTime = currentMillis; // Schedule next retry
      lastWiFiCheckTime = currentMillis;
      lastDotPrintTime = currentMillis;
    }
  }
}

String formatLocalTime(DateTime dt) {
  char buf[20];
  snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
           dt.year(), dt.month(), dt.day(),
           (dt.hour() + timeZoneOffset / 3600) % 24, dt.minute(), dt.second());
  return String(buf);
}

void syncNTP() {
  const char* ntpServers[] = {"asia.pool.ntp.org", "pool.ntp.org", "time.google.com"};
  bool synced = false;
  const unsigned long timeout = 10000;
  Serial.printf("Free heap before NTP: %u bytes\n", ESP.getFreeHeap());
  for (int serverIndex = 0; serverIndex < 3 && !synced; serverIndex++) {
    timeClient.setPoolServerName(ntpServers[serverIndex]);
    timeClient.begin();
    IPAddress ntpIP;
    WiFi.hostByName(ntpServers[serverIndex], ntpIP);
    Serial.printf("NTP server: %s, IP: %s\n", ntpServers[serverIndex], ntpIP.toString().c_str());
    int retries = 0;
    unsigned long startTime = millis();
    while (retries < maxNTPSyncRetries && !synced && (millis() - startTime) < timeout) {
      Serial.printf("NTP sync attempt %d for %s\n", retries + 1, ntpServers[serverIndex]);
      if (timeClient.forceUpdate()) {
        unsigned long epoch = timeClient.getEpochTime();
        if (epoch > 1704067200 && epoch < 1767225600) {
          rtc.adjust(DateTime(epoch));
          lastNTPSyncTime = epoch;
          Serial.printf("RTC synced: %u, Local: %s\n", epoch, formatLocalTime(rtc.now()).c_str());
          synced = true;
        } else {
          Serial.printf("Invalid NTP time: %u\n", epoch);
          retries++;
          delay(2000);
        }
      } else {
        Serial.printf("NTP attempt %d failed!\n", retries + 1);
        retries++;
        delay(2000);
      }
    }
    timeClient.end();
  }
  if (!synced) {
    Serial.println("All NTP attempts failed for all servers!");
    lastNTPSyncTime = 0;
  }
  Serial.printf("Free heap after NTP: %u bytes\n", ESP.getFreeHeap());
}

void reconnectMQTT() {
  if (!mqttClient.connected()) {
    Serial.print("Connecting MQTT...");
    int retries = 0;
    while (!mqttClient.connected() && retries < 3) {
      if (mqttClient.connect("", mqttToken, NULL)) {
        Serial.println("connected!");
      } else {
        Serial.printf("failed, rc=%d\n", mqttClient.state());
        delay(1000);
        retries++;
      }
    }
  }
}

void sendStoredData() {
  if (!sdAvailable) {
    Serial.println("SD unavailable, skipping send!");
    return;
  }
  Serial.printf("Free heap before sendStoredData: %u bytes\n", ESP.getFreeHeap());
  File dataFileHandle = SD.open(dataFile, FILE_READ);
  File sentFileHandle = SD.open(sentFile, FILE_APPEND);
  if (!dataFileHandle || !sentFileHandle) {
    Serial.println("Error opening datalog.txt/sent.txt!");
    if (dataFileHandle) dataFileHandle.close();
    if (sentFileHandle) sentFileHandle.close();
    sdAvailable = false;
    return;
  }
  while (dataFileHandle.available()) {
    String line = dataFileHandle.readStringUntil('\n');
    if (line.length() > 0) {
      char logString[128];
      line.toCharArray(logString, sizeof(logString));
      StaticJsonDocument<128> logDoc;
      DeserializationError error = deserializeJson(logDoc, logString);
      if (!error) {
        String ts = logDoc["timestamp"].as<String>();
        int year, month, day, hour, minute, second;
        if (sscanf(ts.c_str(), "%d-%d-%d %d:%d:%d",
                   &year, &month, &day, &hour, &minute, &second) == 6) {
          DateTime dt(year, month, day, hour, minute, second);
          StaticJsonDocument<128> pubDoc;
          pubDoc["ts"] = (uint64_t)(dt.unixtime() - timeZoneOffset) * 1000;
          JsonObject values = pubDoc.createNestedObject("values");
          values["temperature"] = logDoc["temperature"].as<float>();
          char pubString[128];
          serializeJson(pubDoc, pubString, sizeof(pubString));
          int retries = 0;
          bool published = false;
          while (!published && retries < maxPublishRetries) {
            published = mqttClient.publish(mqttTopic, pubString);
            if (published) {
              sentFileHandle.println(logString);
              Serial.printf("Published stored: %s\n", pubString);
            } else {
              Serial.printf("Publish failed, retrying: %s\n", pubString);
              retries++;
              delay(100);
            }
          }
        } else {
          Serial.printf("Invalid timestamp: %s\n", ts.c_str());
        }
      } else {
        Serial.printf("JSON error: %s\n", error.c_str());
      }
    }
    delay(1);
  }
  dataFileHandle.close();
  sentFileHandle.close();
  SD.remove(dataFile);
  File newDataFile = SD.open(dataFile, FILE_WRITE);
  if (newDataFile) {
    newDataFile.close();
  } else {
    Serial.println("Error creating datalog.txt!");
    sdAvailable = false;
  }
  Serial.printf("Free heap after sendStoredData: %u bytes\n", ESP.getFreeHeap());
}

void publishData(const char* data) {
  int retries = 0;
  bool published = false;
  while (!published && retries < maxPublishRetries) {
    if (mqttClient.connected()) {
      Serial.printf("Publishing: %s\n", data);
      published = mqttClient.publish(mqttTopic, data);
      if (published) {
        Serial.printf("Published: %s\n", data);
      } else {
        Serial.printf("Failed, retrying: %s\n", data);
        retries++;
        delay(100);
      }
    } else {
      Serial.println("MQTT disconnected!");
      break;
    }
  }
}
