/*
 SmartCold Guard — integrated demonstration firmware (ESP32-S3)
 Software rules:
  - Temperature strictly > 23.5 C: sound alarm, record transition
  - Unauthorized opening: distance > 30 cm AND LDR > 350: sound alarm
  - Valid RFID authorizes opening for 60s; once opened, authorization stays
    active until the door closes. Unknown RFID does not grant access.
  - Every scheduled sensor sample and RFID/door/alarm event is written to
    LittleFS before HTTPS upload. Supabase ACK controls deletion.
  - Physical relay/lock NOT controlled; RFID UID is DEMO-ONLY authentication.

 REQUIRED libraries (Arduino Library Manager):
  Adafruit DHT sensor library + Unified Sensor, MFRC522, TinyGPSPlus,
  ArduinoJson 7, U8g2; ESP32 board package 3.3.12+ for built-in TLS roots.
 The buzzer driver circuit must already have been tested electrically.
 Secrets below: DO NOT upload real values to GitHub.
*/
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <time.h>
#include <DHT.h>
#include <SPI.h>
#include <MFRC522.h>
#include <TinyGPS++.h>
#include <Wire.h>
#include <U8g2lib.h>

// >>> LOCAL COPY ONLY: fill the three placeholders. Never commit credentials.
const char* WIFI_SSID = "pi";
const char* WIFI_PASSWORD = "12345678";
const char* DEVICE_TOKEN = "935a477ad7671d97acf35f3cae5809b4e24604c5a9e6fb25";
const char* SERVER_BASE = "https://smartcold-guard.onrender.com";
const char* DEVICE_ID = "esp32-s3-01";

// Demonstration RFID allowlist: edit to match your permitted cards.
const char* AUTHORIZED_UIDS[] = { "C7EC6B7A" };

// Your already-tested pin arrangement.
constexpr uint8_t PIN_DHT = 4;
constexpr uint8_t PIN_LDR = 1;
constexpr uint8_t PIN_BUZZER = 2;
constexpr uint8_t PIN_TRIG = 5;
constexpr uint8_t PIN_ECHO = 6; // HC-SR04 echo MUST use 1k/2k divider
constexpr uint8_t PIN_RFID_SS = 10;
constexpr uint8_t PIN_RFID_MOSI = 11;
constexpr uint8_t PIN_RFID_SCK = 12;
constexpr uint8_t PIN_RFID_MISO = 13;
constexpr uint8_t PIN_RFID_RST = 14;
constexpr uint8_t PIN_GPS_RX = 17;
constexpr uint8_t PIN_GPS_TX = 16;
constexpr uint8_t PIN_OLED_SDA = 8;
constexpr uint8_t PIN_OLED_SCL = 9;

// Demonstration-only thresholds (NOT clinical cold-chain requirements).
constexpr float TEMP_LIMIT_C = 23.5f;
constexpr float DOOR_DISTANCE_CM = 30.0f;
constexpr int LIGHT_ALERT_RAW = 350;
constexpr int DOOR_LDR_RAW = 350;
constexpr uint32_t AUTH_WINDOW_MS = 60000UL;
constexpr uint32_t SAMPLE_MS = 5000UL;
constexpr uint32_t WIFI_RETRY_MS = 10000UL;
constexpr uint32_t OLED_RESULT_MS = 4000UL;
constexpr uint32_t MIN_FREE_FLASH_BYTES = 8192UL;

DHT dht(PIN_DHT, DHT22);
MFRC522 rfid(PIN_RFID_SS, PIN_RFID_RST);
HardwareSerial gpsSerial(2);
TinyGPSPlus gps;
U8G2_SSD1306_128X64_NONAME_F_HW_I2C oled(U8G2_R0, U8X8_PIN_NONE);
Preferences prefs;
SemaphoreHandle_t fileMutex = nullptr;

bool flashReady = false;
bool storageFault = false;
bool oledReady = false;
bool doorOpen = false;
bool accessGranted = false;
bool tempAlarm = false;
bool securityAlarm = false;
bool buzzerActive = false;
bool lastDistanceValid = false;
float lastTemperature = NAN;
float lastHumidity = NAN;
float lastDistance = NAN;
int lastLight = 0;
uint32_t authorizationAtMs = 0;
uint32_t oledResultAtMs = 0;
bool showingResult = false;
uint32_t lastSensorAtMs = 0;
uint32_t lastWifiRetryMs = 0;
uint32_t nextSequence = 0;

// Forward declarations (Arduino IDE also generates prototypes).
void logEvent(const char* kind, const char* code = nullptr,
              const char* uid = nullptr, const char* authorization = nullptr,
              bool active = false);
void applyBuzzer();
void reevaluateSecurity();

// Return trusted timestamp or JSON null. When started offline, GPS UTC time
// can help even if a satellite position fix is unavailable. Otherwise null.
String utcTimestamp() {
  const time_t epoch = time(nullptr);
  struct tm utc;
  char buf[30];
  if (epoch >= 1735689600 && gmtime_r(&epoch, &utc)) {
    strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &utc);
    return String(buf);
  }
  if (gps.date.isValid() && gps.time.isValid() &&
      gps.date.age() < 10000UL && gps.time.age() < 10000UL &&
      gps.date.year() >= 2025) {
    snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02dZ",
             gps.date.year(), gps.date.month(), gps.date.day(),
             gps.time.hour(), gps.time.minute(), gps.time.second());
    return String(buf);
  }
  return String();
}

void setOptionalFloat(JsonObject obj, const char* key, float val) {
  if (isfinite(val)) obj[key] = val;
  else obj[key] = nullptr;
}
void addGPS(JsonDocument& doc) {
  JsonObject g = doc["gps"].to<JsonObject>();
  const bool fix = gps.location.isValid() && gps.location.age() < 10000UL;
  g["hasFix"] = fix;
  if (fix) {
    g["latitude"] = gps.location.lat();
    g["longitude"] = gps.location.lng();
  } else {
    g["latitude"] = nullptr;
    g["longitude"] = nullptr;
  }
  if (fix && gps.speed.isValid() && gps.speed.age() < 10000UL)
    g["speedKmph"] = gps.speed.kmph();
  else g["speedKmph"] = nullptr;
  g["charsProcessed"] = gps.charsProcessed();
  g["satellites"] = gps.satellites.isValid() ? (int)gps.satellites.value() : 0;
}

// Each record has a durable, device-scoped monotonic ID. Save the sequence
// first: an interrupted write can leave a gap but cannot reuse an ID.
// The .tmp file is completed and renamed to .json before being eligible to send.
bool persistRecord(JsonDocument& doc) {
  if (!flashReady || fileMutex == nullptr) {
    storageFault = true;
    applyBuzzer();
    Serial.println("CRITICAL: LittleFS unavailable; record cannot be saved");
    return false;
  }
  if (xSemaphoreTake(fileMutex, pdMS_TO_TICKS(2000)) != pdTRUE) {
    storageFault = true;
    applyBuzzer();
    Serial.println("CRITICAL: flash lock unavailable");
    return false;
  }
  bool ok = false;
  do {
    if (LittleFS.totalBytes() - LittleFS.usedBytes() < MIN_FREE_FLASH_BYTES) {
      Serial.println("CRITICAL: local queue FULL; no records will be silently overwritten");
      break;
    }
    const uint32_t next = nextSequence + 1;
    if (prefs.putULong("seq", next) == 0) {
      Serial.println("CRITICAL: cannot persist record sequence");
      break;
    }
    nextSequence = next;
    char id[70], tmp[30], finalPath[30];
    snprintf(id, sizeof(id), "%s-%010lu", DEVICE_ID, (unsigned long)next);
    snprintf(tmp, sizeof(tmp), "/q/%010lu.tmp", (unsigned long)next);
    snprintf(finalPath, sizeof(finalPath), "/q/%010lu.json", (unsigned long)next);
    doc["id"] = id;
    doc["sequence"] = next;
    doc["deviceId"] = DEVICE_ID;
    doc["uptimeMs"] = millis();
    const String timestamp = utcTimestamp();
    if (timestamp.length()) doc["occurredAt"] = timestamp;
    else doc["occurredAt"] = nullptr;
    File file = LittleFS.open(tmp, "w");
    if (!file) { Serial.println("CRITICAL: unable to create journal file"); break; }
    const size_t written = serializeJson(doc, file);
    file.flush();
    const bool complete = written > 0 && file.size() == written;
    file.close();
    if (!complete || !LittleFS.rename(tmp, finalPath)) {
      Serial.println("CRITICAL: journal write or rename failed");
      break;
    }
    ok = true;
  } while (false);
  xSemaphoreGive(fileMutex);
  if (!ok) storageFault = true;
  applyBuzzer();
  return ok;
}

void fillSnapshot(JsonDocument& doc) {
  setOptionalFloat(doc.as<JsonObject>(), "temperatureC", lastTemperature);
  setOptionalFloat(doc.as<JsonObject>(), "humidityPercent", lastHumidity);
  setOptionalFloat(doc.as<JsonObject>(), "distanceCm", lastDistance);
  doc["ldrRawValue"] = lastLight;
  doc["doorOpen"] = doorOpen;
  doc["doorKnown"] = lastDistanceValid;
  doc["accessGranted"] = accessGranted;
  doc["temperatureAlarm"] = tempAlarm;
  doc["securityAlarm"] = securityAlarm;
  doc["buzzerActive"] = buzzerActive;
  doc["storageFault"] = storageFault;
  addGPS(doc);
}

void logEvent(const char* kind, const char* code,
              const char* uid, const char* authorization, bool active) {
  JsonDocument doc;
  doc["kind"] = kind;
  if (code) doc["code"] = code;
  if (uid) doc["uid"] = uid;
  if (authorization) doc["authorization"] = authorization;
  if (strcmp(kind, "alarm") == 0) doc["active"] = active;
  fillSnapshot(doc);
  if (!persistRecord(doc)) Serial.println("CRITICAL: event not persisted");
}

void applyBuzzer() {
  const bool wanted = tempAlarm || securityAlarm || storageFault;
  if (wanted == buzzerActive) return;
  buzzerActive = wanted;
  ledcWriteTone(PIN_BUZZER, buzzerActive ? 2000 : 0);
  Serial.printf("BUZZER ALARM: %s (temperature=%d security=%d storage=%d)\n",
                buzzerActive ? "ON" : "OFF", tempAlarm, securityAlarm, storageFault);
}

void showReady() {
  if (!oledReady) return;
  oled.clearBuffer();
  oled.setFont(u8g2_font_6x12_tr);
  oled.drawStr(0, 13, "SmartCold Guard");
  oled.drawStr(0, 37, "Scan RFID card");
  if (tempAlarm || securityAlarm || storageFault)
    oled.drawStr(0, 57, "ALARM ACTIVE");
  else oled.drawStr(0, 57, "Monitoring...");
  oled.sendBuffer();
  showingResult = false;
}
void showCard(const String& uid, bool authorized) {
  if (!oledReady) return;
  oled.clearBuffer();
  oled.setFont(u8g2_font_6x10_tr);
  oled.drawStr(0, 11, "SmartCold Guard");
  oled.setFont(u8g2_font_9x15B_tr);
  oled.drawStr(0, 36, authorized ? "AUTHORIZED" : "UNAUTHORIZED");
  oled.setFont(u8g2_font_6x10_tr);
  String label = "UID: " + uid;
  oled.drawStr(0, 55, label.c_str());
  oled.sendBuffer();
  showingResult = true;
  oledResultAtMs = millis();
}

void reevaluateSecurity() {
  // Access permission expires when unused, or upon the door being closed.
  if (accessGranted && !doorOpen &&
      millis() - authorizationAtMs >= AUTH_WINDOW_MS)
    accessGranted = false;
  
  const bool lightDetected = lastLight > LIGHT_ALERT_RAW;

  const bool newAlarm =
      (doorOpen || lightDetected) && !accessGranted;

  if (newAlarm != securityAlarm) {
    securityAlarm = newAlarm;
    applyBuzzer();
    logEvent("alarm", "unauthorized_open", nullptr, nullptr, securityAlarm);
  }
}

void readRFID() {
  if (!rfid.PICC_IsNewCardPresent() || !rfid.PICC_ReadCardSerial()) return;
  String uid;
  for (byte i = 0; i < rfid.uid.size; ++i) {
    if (rfid.uid.uidByte[i] < 16) uid += "0";
    uid += String(rfid.uid.uidByte[i], HEX);
  }
  uid.toUpperCase();
  rfid.PICC_HaltA();
  rfid.PCD_StopCrypto1();
  bool authorized = false;
  for (const char* allowed : AUTHORIZED_UIDS) {
    if (uid == allowed) { authorized = true; break; }
  }
  if (authorized) {
    accessGranted = true;
    authorizationAtMs = millis();
  } else {
    accessGranted = false; // fail closed on unknown scan
  }
  reevaluateSecurity();
  showCard(uid, authorized);
  Serial.printf("RFID: %s UID=%s\n", authorized ? "AUTHORIZED" : "UNAUTHORIZED", uid.c_str());
  logEvent("rfid", "card_scanned", uid.c_str(), authorized ? "authorized" : "unauthorized");
}

float readDistanceCm() {
  digitalWrite(PIN_TRIG, LOW);
  delayMicroseconds(2);
  digitalWrite(PIN_TRIG, HIGH);
  delayMicroseconds(10);
  digitalWrite(PIN_TRIG, LOW);
  const unsigned long microseconds = pulseIn(PIN_ECHO, HIGH, 30000UL);
  if (microseconds == 0) return NAN;
  return microseconds * 0.0343f / 2.0f;
}
void sampleAndRecord() {
  lastTemperature = dht.readTemperature();
  lastHumidity = dht.readHumidity();
  lastDistance = readDistanceCm();
  lastDistanceValid = isfinite(lastDistance);
  lastLight = analogRead(PIN_LDR);

  const bool newTempAlarm = isfinite(lastTemperature) && lastTemperature > TEMP_LIMIT_C;
  if (newTempAlarm != tempAlarm) {
    tempAlarm = newTempAlarm;
    applyBuzzer();
    logEvent("alarm", "high_temp", nullptr, nullptr, tempAlarm);
  }
  // Missing HC-SR04 echo is unknown, NOT a closed-door reading.
  if (lastDistanceValid) {
    const bool newDoorOpen = lastDistance > DOOR_DISTANCE_CM;
    if (newDoorOpen != doorOpen) {
      const bool wasOpen = doorOpen;
      doorOpen = newDoorOpen;
      if (wasOpen && !doorOpen) accessGranted = false; // consume access session
      logEvent("door", doorOpen ? "opened" : "closed", nullptr, nullptr, doorOpen);
      if (!showingResult) showReady();
    }
  }
  reevaluateSecurity();
  applyBuzzer();
  JsonDocument doc;
  doc["kind"] = "sensor";
  fillSnapshot(doc);
  persistRecord(doc);
  Serial.printf("T=%.1fC RH=%.1f%% Dist=%.1fcm LDR=%d Fix=%d Door=%d Auth=%d\n",
                lastTemperature, lastHumidity, lastDistance, lastLight,
                gps.location.isValid() && gps.location.age() < 10000UL,
                doorOpen, accessGranted);
}

// Called in the background task. Never delete a record based on network
// connectivity alone; only remove after matching server ACK for this ID.
bool nextPending(String& id, String& payload, String& path) {
  if (xSemaphoreTake(fileMutex, pdMS_TO_TICKS(3000)) != pdTRUE) return false;
  uint32_t smallest = UINT32_MAX;
  File dir = LittleFS.open("/q");
  if (dir && dir.isDirectory()) {
    File file = dir.openNextFile();
    while (file) {
      const String name = String(file.name());
      if (name.endsWith(".json")) {
        const int slash = name.lastIndexOf('/');
        const String basename = name.substring(slash + 1);
        const uint32_t seq = (uint32_t)strtoul(basename.c_str(), nullptr, 10);
        if (seq > 0 && seq < smallest) {
          smallest = seq;
          path = name.startsWith("/") ? name : (String("/q/") + name);
        }
      }
      file = dir.openNextFile();
    }
  }
  dir.close();
  bool ok = false;
  if (smallest != UINT32_MAX) {
    File f = LittleFS.open(path, "r");
    if (f) { payload = f.readString(); f.close(); }
    JsonDocument doc;
    if (deserializeJson(doc, payload) == DeserializationError::Ok &&
        doc["id"].is<const char*>()) {
      id = doc["id"].as<String>();
      ok = true;
    } else {
      Serial.printf("CRITICAL: invalid journal record %s\n", path.c_str());
      storageFault = true;
      // Do not silently discard an invalid record.
    }
  }
  xSemaphoreGive(fileMutex);
  return ok;
}
void syncWorker(void*) {
  for (;;) {
    if (!flashReady || WiFi.status() != WL_CONNECTED ||
        time(nullptr) < 1735689600) {
      vTaskDelay(pdMS_TO_TICKS(1000));
      continue;
    }
    String id, payload, path;
    if (!nextPending(id, payload, path)) {
      vTaskDelay(pdMS_TO_TICKS(700));
      continue;
    }
    WiFiClientSecure client;
    client.useBuiltinCACertBundle(); // Verified working on user's ESP32-S3
    HTTPClient http;
    http.setConnectTimeout(8000);
    http.setTimeout(12000);
    const String url = String(SERVER_BASE) + "/api/record";
    bool ack = false;
    if (http.begin(client, url)) {
      http.addHeader("Content-Type", "application/json");
      http.addHeader("X-Device-Token", DEVICE_TOKEN);
      const int code = http.POST(payload);
      if (code == 200) {
        JsonDocument reply;
        if (deserializeJson(reply, http.getString()) == DeserializationError::Ok) {
          ack = reply["ackId"].as<String>() == id;
        }
      } else {
        Serial.printf("Cloud retry queued record %s: HTTP %d\n", id.c_str(), code);
      }
    }
    http.end();
    if (ack && xSemaphoreTake(fileMutex, pdMS_TO_TICKS(3000)) == pdTRUE) {
      if (!LittleFS.remove(path)) Serial.println("WARNING: cloud ACK received but local remove failed; will retry");
      else Serial.printf("Cloud saved and ACKed: %s\n", id.c_str());
      xSemaphoreGive(fileMutex);
    }
    vTaskDelay(pdMS_TO_TICKS(ack ? 80 : 1500));
  }
}

void recoverTempFiles() {
  File dir = LittleFS.open("/q");
  if (!dir || !dir.isDirectory()) return;
  File file = dir.openNextFile();
  while (file) {
    const String name = String(file.name());
    file.close();
    if (name.endsWith(".tmp")) {
      const String tmp = name.startsWith("/") ? name : String("/q/") + name;
      const String dst = tmp.substring(0, tmp.length() - 4) + ".json";
      File f = LittleFS.open(tmp, "r");
      String content = f ? f.readString() : String();
      if (f) f.close();
      JsonDocument doc;
      if (deserializeJson(doc, content) == DeserializationError::Ok &&
          doc["id"].is<const char*>() && LittleFS.rename(tmp, dst)) {
        Serial.println("Recovered interrupted journal write: " + dst);
      } else {
        storageFault = true;
        Serial.println("CRITICAL: incomplete temp file requires manual inspection: " + tmp);
      }
    }
    file = dir.openNextFile();
  }
  dir.close();
}

void setup() {
  Serial.begin(115200);
  delay(1200);
  Serial.println("SmartCold Guard FINAL — buffered telemetry and alarms");
  ledcAttach(PIN_BUZZER, 2000, 8);
  ledcWriteTone(PIN_BUZZER, 0);
  Wire.begin(PIN_OLED_SDA, PIN_OLED_SCL);
  oledReady = oled.begin();
  showReady();
  dht.begin();
  analogReadResolution(12);
  analogSetPinAttenuation(PIN_LDR, ADC_11db);
  pinMode(PIN_TRIG, OUTPUT);
  pinMode(PIN_ECHO, INPUT);
  digitalWrite(PIN_TRIG, LOW);
  SPI.begin(PIN_RFID_SCK, PIN_RFID_MISO, PIN_RFID_MOSI, PIN_RFID_SS);
  rfid.PCD_Init();
  gpsSerial.setRxBufferSize(1024);
  gpsSerial.begin(9600, SERIAL_8N1, PIN_GPS_RX, PIN_GPS_TX);
  fileMutex = xSemaphoreCreateMutex();
  flashReady = LittleFS.begin(false); // Do NOT auto-format / erase unsent data
  if (flashReady) {
    if (!LittleFS.exists("/q")) flashReady = LittleFS.mkdir("/q");
    if (flashReady) {
      if (!prefs.begin("smartcold", false)) flashReady = false;
      else {
        nextSequence = prefs.getULong("seq", 0);
        recoverTempFiles();
      }
    }
  }
  if (!flashReady) {
    Serial.println("CRITICAL: LittleFS/NVS failed. DO NOT FORMAT: queued data may exist!");
    storageFault = true;
    applyBuzzer();
  }
  if (String(WIFI_SSID).startsWith("YOUR_") ||
      String(WIFI_PASSWORD).startsWith("YOUR_") ||
      String(DEVICE_TOKEN).startsWith("YOUR_")) {
    Serial.println("ERROR: fill in Wi-Fi credentials and NEW Render DEVICE_TOKEN");
  }
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  configTime(0, 0, "pool.ntp.org", "time.google.com");
  lastWifiRetryMs = millis();
  if (flashReady) {
    xTaskCreatePinnedToCore(syncWorker, "cloud-sync", 10240, nullptr, 1, nullptr, 0);
  }
}

void loop() {
  while (gpsSerial.available()) gps.encode(gpsSerial.read());
  readRFID();
  if (WiFi.status() != WL_CONNECTED && millis() - lastWifiRetryMs >= WIFI_RETRY_MS) {
    lastWifiRetryMs = millis();
    WiFi.reconnect();
  }
  const uint32_t now = millis();
  if (now - lastSensorAtMs >= SAMPLE_MS) {
    lastSensorAtMs = now;
    sampleAndRecord();
  }
  if (showingResult && now - oledResultAtMs >= OLED_RESULT_MS) showReady();
  delay(3);
}
