/*
  SmartCold Guard - Step 9: secure online reporting to Render
  ESP32-S3 + DHT22 + HC-SR04 + RC522 + SIM28 GPS

  Sends to:
  https://smartcold-guard.onrender.com/api/sensor-data
  https://smartcold-guard.onrender.com/api/rfid-event

  REQUIRED ARDUINO LIBRARIES:
    DHT sensor library by Adafruit (+ Adafruit Unified Sensor)
    MFRC522 (provides MFRC522.h)
    TinyGPSPlus by Mikal Hart
    ESP32CertBundle by TANAKA Masayuki (provides esp32_cert_bundle.h)

  IMPORTANT:
    DEVICE_TOKEN is NOT your dashboard password or Supabase secret key.
    Never publish a sketch containing your real Wi-Fi password or device token.
    The HC-SR04 ECHO must reach GPIO6 via a 1k/2k resistor divider.
*/

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <time.h>
#include <DHT.h>
#include <SPI.h>
#include <MFRC522.h>
#include <TinyGPS++.h>
#include <Wire.h>
#include <U8g2lib.h>


// ---------- Fill in these three settings only ----------
const char *WIFI_SSID = "pi";
const char *WIFI_PASSWORD = "12345678";
const char *DEVICE_TOKEN = "935a477ad7671d97acf35f3cae5809b4e24604c5a9e6fb25";

// Use YOUR Render website, not localhost or the laptop's private IP.
const char *SERVER_BASE = "https://smartcold-guard.onrender.com";
const char *DEVICE_ID = "esp32-s3-01";

// ---------- Same GPIOs as your successfully tested wiring ----------
constexpr int DHT_PIN = 4;
constexpr int LDR_PIN = 1;  // LDR connected to GPIO1
const int BUZZER_PIN = 2;
const int LIGHT_THRESHOLD = 4200; // Demo threshold
constexpr int TRIG_PIN = 5;
constexpr int ECHO_PIN = 6; // Must have 1 kOhm / 2 kOhm voltage divider
constexpr int RFID_SS = 10;
constexpr int RFID_MOSI = 11;
constexpr int RFID_SCK = 12;
constexpr int RFID_MISO = 13;
constexpr int RFID_RST = 14;
constexpr int GPS_RX = 17; // GPS TX -> ESP32 GPIO17
constexpr int GPS_TX = 16; // GPS RX <- ESP32 GPIO16
constexpr int OLED_SDA = 8;
constexpr int OLED_SCL = 9;

// Demo authorized card.
// Replace this UID with the card you want to allow.
const char *AUTHORIZED_UID = "C7EC6B7A";

U8G2_SSD1306_128X64_NONAME_F_HW_I2C oled(
  U8G2_R0,
  U8X8_PIN_NONE
);

bool oledShowingCard = false;
unsigned long oledCardMs = 0;


DHT dht(DHT_PIN, DHT22);
MFRC522 rfid(RFID_SS, RFID_RST);
HardwareSerial gpsSerial(2);
TinyGPSPlus gps;

const unsigned long SEND_INTERVAL_MS = 5000;
const unsigned long RECONNECT_INTERVAL_MS = 10000;
unsigned long lastSendMs = 0;
unsigned long lastReconnectMs = 0;
bool clockReady = false;

String jsonNumber(float value, int digits) {
  return isfinite(value) ? String(value, digits) : String("null");
}

bool haveValidCredentials() {
  return String(WIFI_SSID) != "YOUR_WIFI_NAME" &&
         String(WIFI_PASSWORD) != "YOUR_WIFI_PASSWORD" &&
         String(DEVICE_TOKEN) != "PASTE_RENDER_DEVICE_TOKEN_HERE" &&
         strlen(DEVICE_TOKEN) > 10;
}

// TLS certificate validation requires a reasonably correct time.
void startClock() {
  configTime(0, 0, "pool.ntp.org", "time.google.com");
  Serial.println("Synchronizing UTC time for HTTPS certificate validation...");
}

bool checkClock() {
  if (clockReady) return true;
  time_t now = time(nullptr);
  // 2025-01-01 UTC, safely before the current deployment date.
  if (now >= 1735689600) {
    clockReady = true;
    Serial.println("UTC time synchronized; HTTPS enabled.");
    return true;
  }
  return false;
}

float readDistanceCm() {
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);
  const unsigned long duration = pulseIn(ECHO_PIN, HIGH, 30000UL);
  if (duration == 0) return NAN;
  return duration * 0.0343f / 2.0f;
}

bool postJson(const char *route, const String &json) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Not sent: Wi-Fi disconnected");
    return false;
  }
  if (!checkClock()) {
    Serial.println("Not sent: waiting for NTP time sync (TLS safety check)");
    return false;
  }
  if (!haveValidCredentials()) {
    Serial.println("Not sent: fill in Wi-Fi and DEVICE_TOKEN placeholders");
    return false;
  }


  
time_t now = time(nullptr);
struct tm utc;
gmtime_r(&now, &utc);

char dateTime[40];
strftime(
  dateTime,
  sizeof(dateTime),
  "%Y-%m-%d %H:%M:%S UTC",
  &utc
);

Serial.print("ESP32 UTC clock: ");
Serial.println(dateTime);


  // Create a fresh client for each POST: avoids retained TLS state between requests.
  WiFiClientSecure secureClient;
  secureClient.useBuiltinCACertBundle();

  HTTPClient http;
  http.setConnectTimeout(15000);
  http.setTimeout(20000);
  const String url = String(SERVER_BASE) + route;
  if (!http.begin(secureClient, url)) {
    Serial.println("HTTPS begin failed");
    return false;
  }

  http.addHeader("Content-Type", "application/json");
  http.addHeader("X-Device-Token", DEVICE_TOKEN);
  const int responseCode = http.POST(json);
  Serial.printf("POST %s => HTTP %d\n", route, responseCode);
  if (responseCode < 0) {
    Serial.println(http.errorToString(responseCode));
  } else if (responseCode != 200) {
    // Helpful for debugging authentication/validation errors.
    Serial.println(http.getString());
  }
  http.end();
  return responseCode == 200;
}

void readGPS() {
  while (gpsSerial.available() > 0) {
    gps.encode(gpsSerial.read());
  }
}


void showReadyScreen() {
  oled.clearBuffer();

  oled.setFont(u8g2_font_6x12_tr);
  oled.drawStr(0, 18, "SmartCold Guard");
  oled.drawStr(0, 42, "Scan RFID card");

  oled.sendBuffer();
  oledShowingCard = false;
}

void showRFIDResult(String uid, bool authorized) {
  oled.clearBuffer();

  oled.setFont(u8g2_font_6x10_tr);
  oled.drawStr(0, 11, "SmartCold Guard");

  oled.setFont(u8g2_font_9x15B_tr);
  oled.drawStr(
    0, 36,
    authorized ? "AUTHORIZED" : "UNAUTHORIZED"
  );

  oled.setFont(u8g2_font_6x10_tr);
  String uidText = "UID: " + uid;
  oled.drawStr(0, 55, uidText.c_str());

  oled.sendBuffer();

  oledCardMs = millis();
  oledShowingCard = true;
}

void checkRFID() {
  if (!rfid.PICC_IsNewCardPresent() || !rfid.PICC_ReadCardSerial()) return;
  String uid;
  for (byte i = 0; i < rfid.uid.size; ++i) {
    if (rfid.uid.uidByte[i] < 0x10) uid += '0';
    uid += String(rfid.uid.uidByte[i], HEX);
  }
  uid.toUpperCase();
  bool authorized = (uid == AUTHORIZED_UID);

  showRFIDResult(uid, authorized);

  Serial.println(
    authorized ? "RFID AUTHORIZED" : "RFID UNAUTHORIZED"
  );

  rfid.PICC_HaltA();
  rfid.PCD_StopCrypto1();

  Serial.print("RFID card scanned. UID: ");
  Serial.println(uid);
  // This logs a card scan; it does NOT authorize a physical lock.
  const String payload = String("{\"uid\":\"") + uid + "\"}";
  postJson("/api/rfid-event", payload);
}

void sendSensors() {
  const float temperature = dht.readTemperature();
  const float humidity = dht.readHumidity();
  const float distance = readDistanceCm();
  Serial.print("GPS Satellites: ");

  if (gps.satellites.isValid()) {
    Serial.println(gps.satellites.value());
  } else {
    Serial.println("Unknown");
  }

  Serial.print("GPS Valid NMEA Sentences: ");
  Serial.println(gps.passedChecksum());

  Serial.print("GPS Failed Checksums: ");
  Serial.println(gps.failedChecksum());

  Serial.print("GPS Location Fix: ");
  Serial.println(
    gps.location.isValid() &&
    gps.location.age() < 10000
      ? "YES" : "NO"
  );

  const int ldrRawValue = analogRead(LDR_PIN);
  if (ldrRawValue > LIGHT_THRESHOLD) {
    ledcWriteTone(BUZZER_PIN, 2000);
    Serial.println("BUZZER ALARM: ON");
  } else {
    ledcWriteTone(BUZZER_PIN, 0);
    Serial.println("BUZZER ALARM: OFF");
  }

  Serial.print("LDR Light Value: ");
  Serial.println(ldrRawValue);


  const bool hasFix = gps.location.isValid() && gps.location.age() < 10000UL;
  const float speed = (hasFix && gps.speed.isValid() && gps.speed.age() < 10000UL)
      ? gps.speed.kmph() : NAN;

  String json;
  json.reserve(320);
  json = "{\"deviceId\":\"" + String(DEVICE_ID) + "\"";
  json += ",\"temperatureC\":" + jsonNumber(temperature, 1);
  json += ",\"humidityPercent\":" + jsonNumber(humidity, 1);
  json += ",\"distanceCm\":" + jsonNumber(distance, 1);
  json += ",\"ldrRawValue\":" + String(ldrRawValue);
  json += ",\"gps\":{\"hasFix\":";
  json += hasFix ? "true" : "false";
  json += ",\"latitude\":";
  json += hasFix ? String(gps.location.lat(), 6) : String("null");
  json += ",\"longitude\":";
  json += hasFix ? String(gps.location.lng(), 6) : String("null");
  json += ",\"speedKmph\":" + jsonNumber(speed, 2);
  json += ",\"charsProcessed\":" + String(gps.charsProcessed());
  json += "}}";

  Serial.print("Sending: ");
  Serial.println(json);
  postJson("/api/sensor-data", json);
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Wire.begin(OLED_SDA, OLED_SCL);
  oled.begin();
  showReadyScreen();

  Serial.println("SmartCold Guard - online Render upload");
  Serial.println("HTTPS endpoint: https://smartcold-guard.onrender.com");

  dht.begin();
  analogReadResolution(12);
  analogSetPinAttenuation(LDR_PIN, ADC_11db);

  ledcAttach(BUZZER_PIN, 2000, 8);
  ledcWriteTone(BUZZER_PIN, 0);

  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  digitalWrite(TRIG_PIN, LOW);

  SPI.begin(RFID_SCK, RFID_MISO, RFID_MOSI, RFID_SS);
  rfid.PCD_Init();
  delay(5);
  Serial.printf("RC522 register: 0x%02X\n", rfid.PCD_ReadRegister(MFRC522::VersionReg));

  gpsSerial.begin(9600, SERIAL_8N1, GPS_RX, GPS_TX);
  if (!haveValidCredentials()) {
    Serial.println("ERROR: fill in the three credentials at the top of the sketch.");
  }

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  startClock();
  Serial.println("Connecting to Wi-Fi...");
}

void loop() {
  readGPS();
  checkRFID();

  const unsigned long now = millis();
  if (WiFi.status() != WL_CONNECTED && now - lastReconnectMs >= RECONNECT_INTERVAL_MS) {
    lastReconnectMs = now;
    Serial.println("Wi-Fi reconnecting...");
    WiFi.reconnect();
  }
  if (now - lastSendMs >= SEND_INTERVAL_MS) {
    lastSendMs = now;
    sendSensors();
  }

  if (oledShowingCard &&
      millis() - oledCardMs >= 4000UL) {
    showReadyScreen();
  }

  delay(5);
}
