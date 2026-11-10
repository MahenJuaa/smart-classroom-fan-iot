/*
  SMART CLASSROOM FAN - OPTIMIZED
  ESP32 / 2 classrooms / DHT11 / PIR / Relay / physical buttons
  WiFiManager + Vercel API + existing endpoint paths

  PIN MAP (preserved from supplied sketch)
    CLASS A: DHT11=GPIO16, PIR=GPIO26, RELAY=GPIO32, BUTTON=GPIO27
    CLASS B: DHT11=GPIO17, PIR=GPIO25, RELAY=GPIO33, BUTTON=GPIO18

  AUTO:
    temperature >= 30 C AND motion detected -> FAN ON
    no motion for 5 minutes -> FAN OFF
    temperature < 30 C -> FAN OFF

  Physical button / dashboard commands temporarily override AUTO.
  Dashboard can return a room to AUTO by sending control_mode:"AUTO".

  IMPORTANT:
    1) Replace DEVICE_TOKEN with a newly rotated token. The token in the
       uploaded source should be considered exposed.
    2) INSECURE_TLS_FOR_TESTING is only for a trusted prototype network.
       Configure ROOT_CA_CERT and set it false for production.
    3) Verify RELAY_ACTIVE_LOW against the actual relay module.
*/

#include <WiFi.h>
#include <WiFiManager.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <DHT.h>
#include <math.h>

// ---------------- PIN CONFIGURATION ----------------
static const uint8_t DHT_A_PIN = 16;
static const uint8_t DHT_B_PIN = 17;
static const uint8_t PIR_A_PIN = 26;
static const uint8_t PIR_B_PIN = 25;
static const uint8_t RELAY_A_PIN = 32;
static const uint8_t RELAY_B_PIN = 33;
static const uint8_t BUTTON_A_PIN = 27;
static const uint8_t BUTTON_B_PIN = 14;

#define DHT_TYPE DHT11
DHT dhtA(DHT_A_PIN, DHT_TYPE);
DHT dhtB(DHT_B_PIN, DHT_TYPE);

// ---------------- SERVER CONFIGURATION ----------------
const char* SERVER_BASE_URL = "https://smart-classroom-fan-iot.vercel.app";
const char* DEVICE_TOKEN = "GANTI_DENGAN_DEVICE_TOKEN_RANDOM_MINIMAL_32_KARAKTER";
const bool INSECURE_TLS_FOR_TESTING = true; // prototype only
const char* ROOT_CA_CERT = R"EOF(
PASTE_ROOT_CA_CERTIFICATE_HERE
)EOF";

// ---------------- SYSTEM CONFIGURATION ----------------
static const float TEMP_THRESHOLD_C = 30.0f;
static const uint32_t NO_MOTION_TIMEOUT_MS = 5UL * 60UL * 1000UL;
static const uint32_t DHT_INTERVAL_MS = 3000;
static const uint32_t BUTTON_DEBOUNCE_MS = 45;
static const uint32_t COMMAND_INTERVAL_MS = 3000;
static const uint32_t TELEMETRY_INTERVAL_MS = 30000;
static const uint32_t WIFI_RETRY_INTERVAL_MS = 5000;
static const uint32_t HTTP_TIMEOUT_MS = 2500;
static const uint32_t WIFI_CONFIG_HOLD_MS = 5000;
static const uint32_t WIFI_CONFIG_PORTAL_TIMEOUT_SEC = 300;
static const bool RELAY_ACTIVE_LOW = true;

struct RoomState {
  const char* id;
  DHT* dht;
  uint8_t pirPin;
  uint8_t relayPin;
  uint8_t buttonPin;
  float temperature = NAN;
  float humidity = NAN;
  bool dhtValid = false;
  bool motion = false;
  bool fanOn = false;
  bool manualOverride = false;
  bool manualFanState = false;
  bool lastButtonRaw = HIGH;
  bool stableButtonState = HIGH;
  uint32_t lastDebounceMs = 0;
  uint32_t lastMotionMs = 0;
  long lastCommandId = 0;
  bool initializedFromCloud = false;
  volatile bool localControlDirty = false;

  RoomState(const char* roomId, DHT* sensor, uint8_t pir, uint8_t relay, uint8_t button)
    : id(roomId), dht(sensor), pirPin(pir), relayPin(relay), buttonPin(button) {}
};

RoomState roomA("A", &dhtA, PIR_A_PIN, RELAY_A_PIN, BUTTON_A_PIN);
RoomState roomB("B", &dhtB, PIR_B_PIN, RELAY_B_PIN, BUTTON_B_PIN);

uint32_t lastDhtReadMs = 0;
uint32_t lastWiFiRetryMs = 0;
uint32_t bothButtonsHeldSinceMs = 0;
bool wifiPortalTriggered = false;
String activePortalName;
String activePortalPassword;

// HTTP work runs on a separate FreeRTOS task. Local control never waits for it.
enum NetEventType : uint8_t { NET_REMOTE_COMMAND, NET_ACK_COMMAND };
struct NetEvent {
  NetEventType type;
  uint8_t roomIndex;
  bool fanOn;
  bool setAuto;
  long commandId;
};
QueueHandle_t netEventQueue = nullptr;
QueueHandle_t ackQueue = nullptr;

uint8_t relayOffLevel() { return RELAY_ACTIVE_LOW ? HIGH : LOW; }
uint8_t relayOnLevel() { return RELAY_ACTIVE_LOW ? LOW : HIGH; }

void applyRelay(RoomState& room) {
  digitalWrite(room.relayPin, room.fanOn ? relayOnLevel() : relayOffLevel());
}

void setFan(RoomState& room, bool on, const char* reason) {
  if (room.fanOn == on) return;
  room.fanOn = on;
  applyRelay(room);
  Serial.printf("Kelas %s: kipas %s (%s)\n", room.id, on ? "ON" : "OFF", reason);
}

void applyAutomaticControl(RoomState& room, uint32_t now) {
  if (room.manualOverride) {
    setFan(room, room.manualFanState, "manual override");
    return;
  }

  // Fail-safe: without a valid temperature reading, do not start automatically.
  if (!room.dhtValid || room.temperature < TEMP_THRESHOLD_C) {
    setFan(room, false, !room.dhtValid ? "sensor suhu tidak valid" : "suhu di bawah ambang");
    return;
  }

  if (room.motion) {
    room.lastMotionMs = now;
    setFan(room, true, "suhu >= 30 C dan gerakan terdeteksi");
  } else if (room.fanOn && (uint32_t)(now - room.lastMotionMs) >= NO_MOTION_TIMEOUT_MS) {
    setFan(room, false, "tidak ada gerakan selama 5 menit");
  }
}

void readRoomTemperature(RoomState& room) {
  float t = room.dht->readTemperature();
  float h = room.dht->readHumidity();
  if (isnan(t) || isnan(h) || h < 0.0f || h > 100.0f || t < -20.0f || t > 80.0f) {
    room.dhtValid = false;
    Serial.printf("Kelas %s: pembacaan DHT11 gagal.\n", room.id);
  } else {
    room.temperature = t;
    room.humidity = h;
    room.dhtValid = true;
  }
}

bool beginRequest(HTTPClient& http, WiFiClientSecure& client, const String& path) {
  if (WiFi.status() != WL_CONNECTED) return false;
  if (INSECURE_TLS_FOR_TESTING) client.setInsecure();
  else client.setCACert(ROOT_CA_CERT);
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
  if (!beginRequest(http, client, "/api/device/telemetry")) return false;
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
  http.end();
  Serial.printf("Telemetry HTTP %d\n", code);
  return code >= 200 && code < 300;
}

bool postLocalControl(RoomState& room) {
  if (WiFi.status() != WL_CONNECTED) return false;
  WiFiClientSecure client;
  HTTPClient http;
  if (!beginRequest(http, client, "/api/device/local-control")) return false;
  http.addHeader("Content-Type", "application/json");
  StaticJsonDocument<160> payload;
  payload["class_id"] = room.id;
  payload["fan_on"] = room.fanOn;
  String body;
  serializeJson(payload, body);
  int code = http.POST(body);
  http.end();
  Serial.printf("Sync tombol Kelas %s HTTP %d\n", room.id, code);
  return code >= 200 && code < 300;
}

bool postAck(const NetEvent& event) {
  if (WiFi.status() != WL_CONNECTED) return false;
  RoomState& room = event.roomIndex == 0 ? roomA : roomB;
  WiFiClientSecure client;
  HTTPClient http;
  if (!beginRequest(http, client, "/api/device/ack")) return false;
  http.addHeader("Content-Type", "application/json");
  StaticJsonDocument<192> payload;
  payload["class_id"] = room.id;
  payload["command_id"] = event.commandId;
  payload["fan_on"] = event.fanOn;
  String body;
  serializeJson(payload, body);
  int code = http.POST(body);
  http.end();
  Serial.printf("ACK Kelas %s id=%ld HTTP %d\n", room.id, event.commandId, code);
  return code >= 200 && code < 300;
}

void pollRemoteCommands() {
  if (WiFi.status() != WL_CONNECTED) return;
  WiFiClientSecure client;
  HTTPClient http;
  if (!beginRequest(http, client, "/api/device/commands")) return;
  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("GET commands HTTP %d\n", code);
    http.end();
    return;
  }
  String response = http.getString();
  http.end();

  StaticJsonDocument<768> doc;
  if (deserializeJson(doc, response) || doc["ok"] != true || !doc["classes"].is<JsonObject>()) {
    Serial.println("Respons commands tidak valid.");
    return;
  }

  JsonObject classes = doc["classes"].as<JsonObject>();
  RoomState* rooms[] = {&roomA, &roomB};
  for (uint8_t i = 0; i < 2; i++) {
    RoomState& room = *rooms[i];
    if (!classes[room.id].is<JsonObject>()) continue;
    JsonObject item = classes[room.id].as<JsonObject>();
    long commandId = item["command_id"] | 0L;
    if (commandId <= 0 || commandId == room.lastCommandId) continue;

    bool desiredFanOn = item["fan_on"] | false;
    const char* modeText = item["control_mode"] | "MANUAL";
    bool setAuto = (String(modeText).equalsIgnoreCase("AUTO"));

    NetEvent event{NET_REMOTE_COMMAND, i, desiredFanOn, setAuto, commandId};
    if (netEventQueue) xQueueSend(netEventQueue, &event, 0);
  }
}

void handleNetworkEvents() {
  if (!netEventQueue) return;
  NetEvent event;
  while (xQueueReceive(netEventQueue, &event, 0) == pdTRUE) {
    if (event.type != NET_REMOTE_COMMAND) continue;
    RoomState& room = event.roomIndex == 0 ? roomA : roomB;
    if (event.setAuto) {
      room.manualOverride = false;
      Serial.printf("Dashboard: Kelas %s kembali ke AUTO\n", room.id);
    } else {
      room.manualOverride = true;
      room.manualFanState = event.fanOn;
      setFan(room, event.fanOn, "perintah dashboard");
      Serial.printf("Dashboard: Kelas %s mode MANUAL\n", room.id);
    }
    room.lastCommandId = event.commandId;
    NetEvent ack{NET_ACK_COMMAND, event.roomIndex, room.fanOn, event.setAuto, event.commandId};
    if (ackQueue) xQueueSend(ackQueue, &ack, 0);
  }
}

void networkTask(void* parameter) {
  uint32_t lastCommandPoll = 0;
  uint32_t lastTelemetry = 0;
  uint32_t lastLocalSyncA = 0;
  uint32_t lastLocalSyncB = 0;
  for (;;) {
    uint32_t now = millis();
    if (WiFi.status() == WL_CONNECTED) {
      if (roomA.localControlDirty && (uint32_t)(now - lastLocalSyncA) >= 2000) {
        lastLocalSyncA = now;
        if (postLocalControl(roomA)) roomA.localControlDirty = false;
      }
      if (roomB.localControlDirty && (uint32_t)(now - lastLocalSyncB) >= 2000) {
        lastLocalSyncB = now;
        if (postLocalControl(roomB)) roomB.localControlDirty = false;
      }
      if ((uint32_t)(now - lastCommandPoll) >= COMMAND_INTERVAL_MS) {
        lastCommandPoll = now;
        pollRemoteCommands();
      }
      if ((uint32_t)(now - lastTelemetry) >= TELEMETRY_INTERVAL_MS) {
        lastTelemetry = now;
        postTelemetry();
      }
    }

    if (ackQueue) {
      NetEvent ack;
      while (xQueueReceive(ackQueue, &ack, 0) == pdTRUE) {
        postAck(ack);
      }
    }
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

void handleButton(RoomState& room, uint32_t now) {
  bool raw = digitalRead(room.buttonPin); // INPUT_PULLUP: LOW means pressed
  if (raw != room.lastButtonRaw) {
    room.lastButtonRaw = raw;
    room.lastDebounceMs = now;
  }
  if ((uint32_t)(now - room.lastDebounceMs) >= BUTTON_DEBOUNCE_MS && raw != room.stableButtonState) {
    room.stableButtonState = raw;
    if (room.stableButtonState == LOW) {
      room.manualOverride = true;
      room.manualFanState = !room.fanOn;
      setFan(room, room.manualFanState, "tombol fisik");
      room.localControlDirty = true;
    }
  }
}

void makePortalCredentials(String& apName, String& apPassword) {
  uint64_t chipId = ESP.getEfuseMac();
  char nameBuffer[32], passwordBuffer[11];
  snprintf(nameBuffer, sizeof(nameBuffer), "SCF-Setup-%04X", (unsigned int)((chipId >> 32) & 0xFFFF));
  snprintf(passwordBuffer, sizeof(passwordBuffer), "%010llX", (unsigned long long)(chipId & 0xFFFFFFFFFFULL));
  apName = nameBuffer;
  apPassword = passwordBuffer;
}

void startWiFi(bool forcePortal = false) {
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFiManager manager;
  manager.setDebugOutput(true);
  manager.setConnectTimeout(20);
  manager.setConfigPortalTimeout(WIFI_CONFIG_PORTAL_TIMEOUT_SEC);
  manager.setWiFiAutoReconnect(true);
  makePortalCredentials(activePortalName, activePortalPassword);
  bool connected = forcePortal
    ? manager.startConfigPortal(activePortalName.c_str(), activePortalPassword.c_str())
    : manager.autoConnect(activePortalName.c_str(), activePortalPassword.c_str());
  if (connected && WiFi.status() == WL_CONNECTED) {
    Serial.printf("Wi-Fi connected | IP: %s | RSSI: %d dBm\n", WiFi.localIP().toString().c_str(), WiFi.RSSI());
  } else {
    Serial.println("Wi-Fi belum tersambung. Kontrol lokal tetap berjalan.");
  }
}

void openWiFiConfigPortal() {
  startWiFi(true); // This setup portal is intentionally blocking while configuration is active.
  roomA.lastButtonRaw = roomA.stableButtonState = digitalRead(BUTTON_A_PIN);
  roomB.lastButtonRaw = roomB.stableButtonState = digitalRead(BUTTON_B_PIN);
  roomA.lastDebounceMs = roomB.lastDebounceMs = millis();
}

void setup() {
  Serial.begin(115200);

  // Set output latches before switching pins to OUTPUT to minimize relay glitches.
  digitalWrite(RELAY_A_PIN, relayOffLevel());
  digitalWrite(RELAY_B_PIN, relayOffLevel());
  pinMode(RELAY_A_PIN, OUTPUT);
  pinMode(RELAY_B_PIN, OUTPUT);
  digitalWrite(RELAY_A_PIN, relayOffLevel());
  digitalWrite(RELAY_B_PIN, relayOffLevel());

  pinMode(PIR_A_PIN, INPUT);
  pinMode(PIR_B_PIN, INPUT);
  pinMode(BUTTON_A_PIN, INPUT_PULLUP);
  pinMode(BUTTON_B_PIN, INPUT_PULLUP);
  roomA.lastButtonRaw = roomA.stableButtonState = digitalRead(BUTTON_A_PIN);
  roomB.lastButtonRaw = roomB.stableButtonState = digitalRead(BUTTON_B_PIN);

  dhtA.begin();
  dhtB.begin();
  roomA.lastMotionMs = roomB.lastMotionMs = millis();

  Serial.println("Smart Classroom Fan optimized: AUTO + manual override + cloud.");
  Serial.println("Tahan kedua tombol 5 detik untuk membuka portal Wi-Fi.");
  startWiFi(false);

  netEventQueue = xQueueCreate(8, sizeof(NetEvent));
  ackQueue = xQueueCreate(8, sizeof(NetEvent));
  xTaskCreatePinnedToCore(networkTask, "NetworkTask", 8192, nullptr, 1, nullptr, 0);
}

void loop() {
  uint32_t now = millis();

  roomA.motion = (digitalRead(PIR_A_PIN) == HIGH);
  roomB.motion = (digitalRead(PIR_B_PIN) == HIGH);

  bool bothButtonsPressed = digitalRead(BUTTON_A_PIN) == LOW && digitalRead(BUTTON_B_PIN) == LOW;
  if (bothButtonsPressed) {
    if (bothButtonsHeldSinceMs == 0) bothButtonsHeldSinceMs = now;
    else if (!wifiPortalTriggered && (uint32_t)(now - bothButtonsHeldSinceMs) >= WIFI_CONFIG_HOLD_MS) {
      wifiPortalTriggered = true;
      openWiFiConfigPortal();
    }
  } else {
    bothButtonsHeldSinceMs = 0;
    wifiPortalTriggered = false;
    handleButton(roomA, now);
    handleButton(roomB, now);
  }

  if ((uint32_t)(now - lastDhtReadMs) >= DHT_INTERVAL_MS) {
    lastDhtReadMs = now;
    readRoomTemperature(roomA);
    readRoomTemperature(roomB);
  }

  // Refresh motion timestamp immediately; control is independent per classroom.
  if (roomA.motion) roomA.lastMotionMs = now;
  if (roomB.motion) roomB.lastMotionMs = now;
  applyAutomaticControl(roomA, now);
  applyAutomaticControl(roomB, now);
  handleNetworkEvents();

  if (WiFi.status() != WL_CONNECTED && (uint32_t)(now - lastWiFiRetryMs) >= WIFI_RETRY_INTERVAL_MS) {
    lastWiFiRetryMs = now;
    WiFi.reconnect();
  }

  // No delay(): loop remains responsive to PIR and physical buttons.
  yield();
}
