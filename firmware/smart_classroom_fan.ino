/*
  SMART CLASSROOM FAN v3 — CLOUD IOT, TWO CLASSROOMS

  This is a monitoring + remote/manual control system, NOT an automatic
  temperature/occupancy controller. DHT22 and PIR values are telemetry only.

  GPIO map (same physical wiring as the agreed v2 map):
    DHT22 A DATA -> GPIO16       PIR A OUT -> GPIO32
    Relay A IN  -> GPIO25       Button A  -> GPIO27 and GND
    DHT22 B DATA -> GPIO17       PIR B OUT -> GPIO33
    Relay B IN  -> GPIO26       Button B  -> GPIO18 and GND

  Libraries:
    - DHT sensor library by Adafruit
    - Adafruit Unified Sensor
    - ArduinoJson 6.x or 7.x

  Configure the HTTPS root CA before a public/production deployment.
  INSECURE_TLS_FOR_TESTING=true disables certificate verification and is
  ONLY suitable for temporary prototype testing on a trusted network.
*/

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <DHT.h>
#include <math.h>
#include <string.h>

static const uint8_t DHT_A_PIN = 16;
static const uint8_t DHT_B_PIN = 17;
static const uint8_t PIR_A_PIN = 32;
static const uint8_t PIR_B_PIN = 33;
static const uint8_t RELAY_A_PIN = 25;
static const uint8_t RELAY_B_PIN = 26;
static const uint8_t BUTTON_A_PIN = 27;
static const uint8_t BUTTON_B_PIN = 18;

#define DHT_TYPE DHT22
DHT dhtA(DHT_A_PIN, DHT_TYPE);
DHT dhtB(DHT_B_PIN, DHT_TYPE);

// ---------- CONFIGURATION ----------
const char* WIFI_SSID = "NAMA_WIFI_2_4GHZ";
const char* WIFI_PASSWORD = "PASSWORD_WIFI";
const char* SERVER_BASE_URL = "https://YOUR-APP.onrender.com";
const char* DEVICE_TOKEN = "GANTI_DENGAN_DEVICE_TOKEN_RANDOM_MINIMAL_32_KARAKTER";

// Set false and paste your hosting provider's trusted root CA certificate below
// before using this project beyond a temporary prototype test.
const bool INSECURE_TLS_FOR_TESTING = true;
const char* ROOT_CA_CERT = R"EOF(
PASTE_ROOT_CA_CERTIFICATE_HERE
)EOF";

// Verify the relay module's trigger polarity before attaching fan power.
const bool RELAY_ACTIVE_LOW = true;

static const uint32_t DHT_INTERVAL_MS = 3000;
static const uint32_t TELEMETRY_INTERVAL_MS = 10000;
static const uint32_t COMMAND_INTERVAL_MS = 2000;
static const uint32_t WIFI_RETRY_INTERVAL_MS = 5000;
static const uint32_t BUTTON_DEBOUNCE_MS = 45;
static const uint32_t HTTP_TIMEOUT_MS = 6000;
static const uint32_t LOCAL_SYNC_RETRY_MS = 2000;

struct RoomState {
  const char* id;
  DHT* dht;
  uint8_t pirPin;
  uint8_t relayPin;
  uint8_t buttonPin;
  float temperature;
  float humidity;
  bool dhtValid;
  bool motion;
  bool fanOn;
  bool initializedFromCloud;
  long lastCommandId;
  bool lastButtonRaw;
  bool stableButtonState;
  uint32_t lastDebounceMs;
  bool localControlDirty;
  uint32_t localControlLastTryMs;
};

RoomState roomA = {"A", &dhtA, PIR_A_PIN, RELAY_A_PIN, BUTTON_A_PIN,
                   NAN, NAN, false, false, false, false, 0,
                   true, true, 0, false, 0};
RoomState roomB = {"B", &dhtB, PIR_B_PIN, RELAY_B_PIN, BUTTON_B_PIN,
                   NAN, NAN, false, false, false, false, 0,
                   true, true, 0, false, 0};

uint32_t lastDhtReadMs = 0;
uint32_t lastTelemetryMs = 0;
uint32_t lastCommandMs = 0;
uint32_t lastWiFiRetryMs = 0;

uint8_t relayOffLevel() { return RELAY_ACTIVE_LOW ? HIGH : LOW; }
uint8_t relayOnLevel() { return RELAY_ACTIVE_LOW ? LOW : HIGH; }

void initializeRelayOff(uint8_t pin) {
  digitalWrite(pin, relayOffLevel());
  pinMode(pin, OUTPUT);
  digitalWrite(pin, relayOffLevel());
}

void applyRelay(RoomState& room) {
  digitalWrite(room.relayPin, room.fanOn ? relayOnLevel() : relayOffLevel());
}

void readRoom(RoomState& room) {
  float t = room.dht->readTemperature();
  float h = room.dht->readHumidity();
  if (isnan(t) || isnan(h) || h < 0.0f || h > 100.0f || t < -20.0f || t > 80.0f) {
    room.dhtValid = false;
    Serial.printf("Kelas %s: pembacaan DHT22 gagal.\n", room.id);
  } else {
    room.temperature = t;
    room.humidity = h;
    room.dhtValid = true;
  }
  room.motion = (digitalRead(room.pirPin) == HIGH);
}

bool beginAuthenticatedRequest(HTTPClient& http, WiFiClientSecure& client, const String& path) {
  if (WiFi.status() != WL_CONNECTED) return false;
  if (INSECURE_TLS_FOR_TESTING) {
    client.setInsecure();
  } else {
    client.setCACert(ROOT_CA_CERT);
  }
  http.setConnectTimeout(HTTP_TIMEOUT_MS);
  http.setTimeout(HTTP_TIMEOUT_MS);
  if (!http.begin(client, String(SERVER_BASE_URL) + path)) return false;
  http.addHeader("X-Device-Token", DEVICE_TOKEN);
  return true;
}

bool postTelemetry() {
  if (WiFi.status() != WL_CONNECTED) return false;
  WiFiClientSecure client;
  HTTPClient http;
  if (!beginAuthenticatedRequest(http, client, "/api/device/telemetry")) return false;
  http.addHeader("Content-Type", "application/json");

  StaticJsonDocument<512> payload;
  JsonObject classes = payload.createNestedObject("classes");
  RoomState* rooms[] = {&roomA, &roomB};
  for (RoomState* room : rooms) {
    JsonObject item = classes.createNestedObject(room->id);
    if (room->dhtValid) {
      item["temperature"] = room->temperature;
      item["humidity"] = room->humidity;
    } else {
      item["temperature"] = nullptr;
      item["humidity"] = nullptr;
    }
    item["motion"] = room->motion;
    item["fan_on"] = room->fanOn;
  }

  String body;
  serializeJson(payload, body);
  int code = http.POST(body);
  String response = http.getString();
  http.end();
  Serial.printf("Telemetry HTTP %d: %s\n", code, response.c_str());
  return code >= 200 && code < 300;
}

bool postPhysicalButtonState(RoomState& room) {
  if (WiFi.status() != WL_CONNECTED) return false;
  WiFiClientSecure client;
  HTTPClient http;
  if (!beginAuthenticatedRequest(http, client, "/api/device/local-control")) return false;
  http.addHeader("Content-Type", "application/json");

  StaticJsonDocument<128> payload;
  payload["class_id"] = room.id;
  payload["fan_on"] = room.fanOn;
  String body;
  serializeJson(payload, body);
  int code = http.POST(body);
  String response = http.getString();
  http.end();
  Serial.printf("Sync tombol Kelas %s HTTP %d: %s\n", room.id, code, response.c_str());
  if (code >= 200 && code < 300) {
    room.localControlDirty = false;
    return true;
  }
  return false;
}

bool acknowledgeRemoteCommand(RoomState& room, long commandId) {
  WiFiClientSecure client;
  HTTPClient http;
  if (!beginAuthenticatedRequest(http, client, "/api/device/ack")) return false;
  http.addHeader("Content-Type", "application/json");

  StaticJsonDocument<160> payload;
  payload["class_id"] = room.id;
  payload["command_id"] = commandId;
  payload["fan_on"] = room.fanOn;
  String body;
  serializeJson(payload, body);
  int code = http.POST(body);
  String response = http.getString();
  http.end();
  Serial.printf("ACK perintah Kelas %s id=%ld HTTP %d: %s\n",
                room.id, commandId, code, response.c_str());
  return code >= 200 && code < 300;
}

bool getRemoteCommands() {
  if (WiFi.status() != WL_CONNECTED) return false;
  WiFiClientSecure client;
  HTTPClient http;
  if (!beginAuthenticatedRequest(http, client, "/api/device/commands")) return false;
  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("GET commands gagal HTTP %d\n", code);
    http.end();
    return false;
  }

  String response = http.getString();
  http.end();
  StaticJsonDocument<512> document;
  DeserializationError error = deserializeJson(document, response);
  if (error || document["ok"] != true || !document["classes"].is<JsonObject>()) {
    Serial.println("Respons commands tidak valid.");
    return false;
  }

  JsonObject classes = document["classes"].as<JsonObject>();
  RoomState* rooms[] = {&roomA, &roomB};
  for (RoomState* room : rooms) {
    if (!classes[room->id].is<JsonObject>()) continue;
    JsonObject item = classes[room->id].as<JsonObject>();
    long commandId = item["command_id"] | 0L;
    bool desiredFanOn = item["fan_on"] | false;

    // Initial cloud synchronization lets the relay reflect the last saved state.
    if (!room->initializedFromCloud) {
      room->fanOn = desiredFanOn;
      applyRelay(*room);
      room->initializedFromCloud = true;
    }

    // A pending dashboard command is applied once and acknowledged by its ID.
    if (commandId > 0 && commandId != room->lastCommandId) {
      room->fanOn = desiredFanOn;
      applyRelay(*room);
      if (acknowledgeRemoteCommand(*room, commandId)) {
        room->lastCommandId = commandId;
      }
      Serial.printf("Perintah dashboard Kelas %s: kipas %s\n",
                    room->id, room->fanOn ? "ON" : "OFF");
    }
  }
  return true;
}

void handleButton(RoomState& room, uint32_t now) {
  bool raw = digitalRead(room.buttonPin) == HIGH; // released HIGH, pressed LOW
  if (raw != room.lastButtonRaw) {
    room.lastButtonRaw = raw;
    room.lastDebounceMs = now;
  }
  if ((uint32_t)(now - room.lastDebounceMs) >= BUTTON_DEBOUNCE_MS &&
      raw != room.stableButtonState) {
    room.stableButtonState = raw;
    if (room.stableButtonState == LOW) {
      room.fanOn = !room.fanOn;
      applyRelay(room);
      room.localControlDirty = true;
      room.localControlLastTryMs = now - LOCAL_SYNC_RETRY_MS;
      Serial.printf("Tombol fisik Kelas %s: kipas %s\n",
                    room.id, room.fanOn ? "ON" : "OFF");
    }
  }
}

void startWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.println("Menghubungkan ESP32 ke Wi-Fi 2.4 GHz...");
}

void setup() {
  Serial.begin(115200);
  delay(200);

  initializeRelayOff(RELAY_A_PIN);
  initializeRelayOff(RELAY_B_PIN);
  roomA.fanOn = false;
  roomB.fanOn = false;

  pinMode(PIR_A_PIN, INPUT);
  pinMode(PIR_B_PIN, INPUT);
  pinMode(BUTTON_A_PIN, INPUT_PULLUP);
  pinMode(BUTTON_B_PIN, INPUT_PULLUP);
  roomA.lastButtonRaw = roomA.stableButtonState = digitalRead(BUTTON_A_PIN);
  roomB.lastButtonRaw = roomB.stableButtonState = digitalRead(BUTTON_B_PIN);

  dhtA.begin();
  dhtB.begin();
  startWiFi();

  Serial.println("Smart Classroom Fan v3 siap: monitoring cloud + kontrol manual.");
  Serial.println("Sensor suhu/kelembapan/PIR tidak menjalankan otomasi kipas.");
}

void loop() {
  uint32_t now = millis();

  roomA.motion = digitalRead(PIR_A_PIN) == HIGH;
  roomB.motion = digitalRead(PIR_B_PIN) == HIGH;
  handleButton(roomA, now);
  handleButton(roomB, now);

  if ((uint32_t)(now - lastDhtReadMs) >= DHT_INTERVAL_MS) {
    lastDhtReadMs = now;
    readRoom(roomA);
    readRoom(roomB);
  }

  if (WiFi.status() != WL_CONNECTED) {
    if ((uint32_t)(now - lastWiFiRetryMs) >= WIFI_RETRY_INTERVAL_MS) {
      lastWiFiRetryMs = now;
      Serial.println("Wi-Fi terputus; mencoba koneksi ulang. Tombol fisik tetap bekerja.");
      WiFi.reconnect();
    }
  } else {
    // Sync local button changes before reading remote commands.
    if (roomA.localControlDirty && (uint32_t)(now - roomA.localControlLastTryMs) >= LOCAL_SYNC_RETRY_MS) {
      roomA.localControlLastTryMs = now;
      postPhysicalButtonState(roomA);
    }
    if (roomB.localControlDirty && (uint32_t)(now - roomB.localControlLastTryMs) >= LOCAL_SYNC_RETRY_MS) {
      roomB.localControlLastTryMs = now;
      postPhysicalButtonState(roomB);
    }

    if (!roomA.localControlDirty && !roomB.localControlDirty &&
        (uint32_t)(now - lastCommandMs) >= COMMAND_INTERVAL_MS) {
      lastCommandMs = now;
      getRemoteCommands();
    }
    if ((uint32_t)(now - lastTelemetryMs) >= TELEMETRY_INTERVAL_MS) {
      lastTelemetryMs = now;
      postTelemetry();
    }
  }
  delay(10);
}
