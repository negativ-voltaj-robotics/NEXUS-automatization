/*
  SMART BEDROOM 2.0 PHANTOM — ESP8266 master
  Board: NodeMCU ESP8266 / Wemos D1 mini
  Libraries: ESP8266 core, ArduinoJson 6.x (Library Manager)
  Upload data/index.html using ESP8266 LittleFS Data Upload, or use the README instructions.
*/
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <SoftwareSerial.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <time.h>

const char* WIFI_SSID = "wifi_corrado";
const char* WIFI_PASSWORD = "pdcorrado2004";
const char* SETUP_AP_NAME = "PHANTOM-SETUP";
const char* SETUP_AP_PASSWORD = "phantom8266"; // 8+ chars

// ESP-controlled channels: logical relays 1..4
const uint8_t ESP_RELAY_PINS[4] = {D1, D2, D6, D0};
const bool ESP_RELAY_ACTIVE_LOW = true;

// SoftwareSerial: ESP RX, TX. Nano TX must be level-shifted to 3.3V.
SoftwareSerial nanoSerial(D7, D5); // RX, TX
ESP8266WebServer server(80);

static const char* CONFIG_PATH = "/config.json";
static const char* SCENES_PATH = "/scenes.json";
static const char* TIMERS_PATH = "/timers.json";
static const char* LOG_PATH = "/events.json";

bool relayStates[16] = {false};
bool nanoOnline = false;
unsigned long lastNanoSeen = 0;
unsigned long lastPing = 0;
String nanoLine;

struct RelayTimer {
  bool active = false;
  bool target = false;
  uint32_t remainingAtSave = 0;
  unsigned long deadline = 0;
};
RelayTimer relayTimers[16];

DynamicJsonDocument configDoc(4096);
DynamicJsonDocument scenesDoc(8192);
DynamicJsonDocument timersDoc(4096);
DynamicJsonDocument logDoc(4096);

String defaultNames[16] = {
  "Main Light", "Desk LED", "Bedside Lamp", "Fan",
  "Relay 05", "Relay 06", "Relay 07", "Relay 08",
  "Relay 09", "Relay 10", "Relay 11", "Relay 12",
  "Relay 13", "Relay 14", "Relay 15", "Relay 16"
};

String jsonString(JsonDocument& doc) {
  String out;
  serializeJson(doc, out);
  return out;
}

bool writeJsonFile(const char* path, JsonDocument& doc) {
  File f = LittleFS.open(path, "w");
  if (!f) return false;
  serializeJson(doc, f);
  f.close();
  return true;
}

bool readJsonFile(const char* path, JsonDocument& doc) {
  if (!LittleFS.exists(path)) return false;
  File f = LittleFS.open(path, "r");
  if (!f) return false;
  DeserializationError err = deserializeJson(doc, f);
  f.close();
  return !err;
}

void addEvent(const String& message) {
  JsonArray arr = logDoc.as<JsonArray>();
  if (arr.isNull()) arr = logDoc.to<JsonArray>();
  JsonObject e = arr.createNestedObject();
  e["ms"] = millis();
  e["event"] = message;
  while (arr.size() > 30) arr.remove(0);
  writeJsonFile(LOG_PATH, logDoc);
}

void saveConfig() { writeJsonFile(CONFIG_PATH, configDoc); }
void saveScenes() { writeJsonFile(SCENES_PATH, scenesDoc); }

void setPhysicalRelay(uint8_t index, bool on) {
  if (index >= 4) return;
  digitalWrite(ESP_RELAY_PINS[index],
    ESP_RELAY_ACTIVE_LOW ? (on ? LOW : HIGH) : (on ? HIGH : LOW));
  relayStates[index] = on;
}

void sendNanoCommand(const String& cmd) {
  nanoSerial.print(cmd);
  nanoSerial.print('\n');
}

void saveTimers();

void setRelay(uint8_t index, bool on) {
  if (index >= 16) return;
  if (index < 4) {
    setPhysicalRelay(index, on);
  } else {
    sendNanoCommand("SET," + String(index + 1) + "," + String(on ? 1 : 0));
    // Command is sent to Nano; reported state is updated after ACK.
  }
  relayTimers[index].active = false;
  saveTimers();
  addEvent("Relay " + String(index + 1) + (on ? " ON" : " OFF"));
}

void handleNanoLine(String line) {
  line.trim();
  if (line == "PONG") {
    nanoOnline = true;
    lastNanoSeen = millis();
    return;
  }
  if (line.startsWith("OK,")) {
    int c1 = line.indexOf(',');
    int c2 = line.indexOf(',', c1 + 1);
    if (c2 > 0) {
      int ch = line.substring(c1 + 1, c2).toInt();
      int st = line.substring(c2 + 1).toInt();
      if (ch >= 5 && ch <= 16) {
        relayStates[ch - 1] = (st == 1);
        nanoOnline = true;
        lastNanoSeen = millis();
      }
    }
  }
  if (line.startsWith("STATE,")) {
    int start = 6;
    for (int ch = 5; ch <= 16; ch++) {
      int comma = line.indexOf(',', start);
      String token = comma < 0 ? line.substring(start) : line.substring(start, comma);
      relayStates[ch - 1] = token.toInt() == 1;
      if (comma < 0) break;
      start = comma + 1;
    }
    nanoOnline = true;
    lastNanoSeen = millis();
  }
}

void pollNano() {
  while (nanoSerial.available()) {
    char c = (char)nanoSerial.read();
    if (c == '\n') {
      handleNanoLine(nanoLine);
      nanoLine = "";
    } else if (c != '\r' && nanoLine.length() < 120) {
      nanoLine += c;
    } else if (nanoLine.length() >= 120) {
      nanoLine = "";
    }
  }
  if (millis() - lastPing > 2500) {
    sendNanoCommand("PING");
    sendNanoCommand("GET");
    lastPing = millis();
  }
  if (millis() - lastNanoSeen > 8000) nanoOnline = false;
}

void loadPersistentData() {
  if (!readJsonFile(CONFIG_PATH, configDoc)) {
    configDoc.clear();
    JsonArray names = configDoc.createNestedArray("names");
    for (int i=0;i<16;i++) names.add(defaultNames[i]);
    JsonArray enabled = configDoc.createNestedArray("enabled");
    for (int i=0;i<16;i++) enabled.add(true);
    configDoc["theme"] = "phantom";
    saveConfig();
  }
  if (!readJsonFile(SCENES_PATH, scenesDoc)) {
    scenesDoc.clear();
    scenesDoc.to<JsonArray>();
    saveScenes();
  }
  if (!readJsonFile(LOG_PATH, logDoc)) {
    logDoc.clear();
    logDoc.to<JsonArray>();
  }
  if (!readJsonFile(TIMERS_PATH, timersDoc)) {
    timersDoc.clear();
    timersDoc.to<JsonArray>();
  }
  // Timers resume their saved remaining duration after reboot. Downtime is not deducted.
  unsigned long now = millis();
  JsonArray arr = timersDoc.as<JsonArray>();
  for (JsonObject t : arr) {
    int ch = t["ch"] | -1;
    uint32_t secs = t["seconds"] | 0;
    bool target = t["target"] | false;
    if (ch >= 1 && ch <= 16 && secs > 0 && secs <= 86400) {
      relayTimers[ch-1].active = true;
      relayTimers[ch-1].target = target;
      relayTimers[ch-1].deadline = now + (unsigned long)secs * 1000UL;
    }
  }
}

void saveTimers() {
  timersDoc.clear();
  JsonArray arr = timersDoc.to<JsonArray>();
  unsigned long now = millis();
  for (int i=0;i<16;i++) {
    if (!relayTimers[i].active) continue;
    uint32_t secs = (uint32_t)((int32_t)(relayTimers[i].deadline - now) > 0
      ? (relayTimers[i].deadline - now) / 1000UL : 0);
    if (!secs) continue;
    JsonObject t = arr.createNestedObject();
    t["ch"] = i + 1;
    t["target"] = relayTimers[i].target;
    t["seconds"] = secs;
  }
  writeJsonFile(TIMERS_PATH, timersDoc);
}

void sendJson(int code, JsonDocument& doc) {
  server.send(code, "application/json; charset=utf-8", jsonString(doc));
}

void handleState() {
  StaticJsonDocument<2048> doc;
  JsonArray states = doc.createNestedArray("states");
  JsonArray names = doc.createNestedArray("names");
  JsonArray enabled = doc.createNestedArray("enabled");
  JsonArray timers = doc.createNestedArray("timers");
  JsonArray timerTargets = doc.createNestedArray("timerTargets");
  JsonArray configNames = configDoc["names"].as<JsonArray>();
  JsonArray configEnabled = configDoc["enabled"].as<JsonArray>();
  unsigned long now = millis();

  for (int i=0;i<16;i++) {
    states.add(relayStates[i]);
    names.add(i < (int)configNames.size() ? configNames[i].as<String>() : defaultNames[i]);
    enabled.add(i < (int)configEnabled.size() ? configEnabled[i].as<bool>() : true);
    uint32_t left = 0;
    if (relayTimers[i].active && (int32_t)(relayTimers[i].deadline - now) > 0)
      left = (relayTimers[i].deadline - now) / 1000UL;
    timers.add(left);
    timerTargets.add(relayTimers[i].target);
  }
  doc["nanoOnline"] = nanoOnline;
  doc["wifiConnected"] = WiFi.status() == WL_CONNECTED;
  doc["ip"] = WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : WiFi.softAPIP().toString();
  doc["rssi"] = WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0;
  sendJson(200, doc);
}

void handleConfigPost() {
  StaticJsonDocument<3072> body;
  if (deserializeJson(body, server.arg("plain"))) {
    server.send(400, "application/json", "{\"error\":\"Invalid JSON\"}"); return;
  }
  JsonArray namesIn = body["names"].as<JsonArray>();
  JsonArray enabledIn = body["enabled"].as<JsonArray>();
  if (namesIn.size() != 16 || enabledIn.size() != 16) {
    server.send(400, "application/json", "{\"error\":\"Expected 16 names and enabled flags\"}"); return;
  }
  configDoc.clear();
  JsonArray names = configDoc.createNestedArray("names");
  JsonArray enabled = configDoc.createNestedArray("enabled");
  for (int i=0;i<16;i++) {
    String name = namesIn[i] | defaultNames[i];
    name.trim();
    if (name.length() == 0) name = defaultNames[i];
    if (name.length() > 28) name = name.substring(0,28);
    names.add(name);
    enabled.add(enabledIn[i].as<bool>());
  }
  configDoc["theme"] = "phantom";
  saveConfig();
  addEvent("Channel configuration saved");
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleRelayPost() {
  StaticJsonDocument<256> body;
  if (deserializeJson(body, server.arg("plain"))) {
    server.send(400, "application/json", "{\"error\":\"Invalid JSON\"}"); return;
  }
  int ch = body["ch"] | 0;
  if (ch < 1 || ch > 16) {
    server.send(400, "application/json", "{\"error\":\"Channel must be 1..16\"}"); return;
  }
  JsonArray en = configDoc["enabled"].as<JsonArray>();
  if (en.size() == 16 && !en[ch-1].as<bool>()) {
    server.send(409, "application/json", "{\"error\":\"Channel disabled in settings\"}"); return;
  }
  if (ch >= 5 && !nanoOnline) {
    server.send(503, "application/json", "{\"error\":\"Nano offline; command not sent\"}"); return;
  }
  bool state = body["state"] | false;
  setRelay(ch-1, state);
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleTimerPost() {
  StaticJsonDocument<256> body;
  if (deserializeJson(body, server.arg("plain"))) {
    server.send(400, "application/json", "{\"error\":\"Invalid JSON\"}"); return;
  }
  int ch = body["ch"] | 0;
  int minutes = body["minutes"] | 0;
  if (ch < 1 || ch > 16) {
    server.send(400, "application/json", "{\"error\":\"Channel must be 1..16\"}"); return;
  }
  if (body["cancel"] | false) {
    relayTimers[ch-1].active = false;
    saveTimers();
    server.send(200, "application/json", "{\"ok\":true,\"cancelled\":true}");
    return;
  }
  if (minutes < 1 || minutes > 1440) {
    server.send(400, "application/json", "{\"error\":\"Minutes must be 1..1440\"}"); return;
  }
  if (ch >= 5 && !nanoOnline) {
    server.send(503, "application/json", "{\"error\":\"Nano offline\"}"); return;
  }
  relayTimers[ch-1].active = true;
  relayTimers[ch-1].target = body["state"] | false;
  relayTimers[ch-1].deadline = millis() + (unsigned long)minutes * 60000UL;
  saveTimers();
  addEvent("Timer scheduled for relay " + String(ch));
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleScenesPost() {
  StaticJsonDocument<1024> body;
  if (deserializeJson(body, server.arg("plain"))) {
    server.send(400, "application/json", "{\"error\":\"Invalid JSON\"}"); return;
  }
  String name = body["name"] | "";
  JsonArray states = body["states"].as<JsonArray>();
  name.trim();
  if (!name.length() || name.length() > 28 || states.size() != 16) {
    server.send(400, "application/json", "{\"error\":\"Name and 16 states required\"}"); return;
  }
  JsonArray arr = scenesDoc.as<JsonArray>();
  if (arr.size() >= 20) {
    server.send(409, "application/json", "{\"error\":\"Maximum 20 scenes\"}"); return;
  }
  JsonObject scene = arr.createNestedObject();
  scene["name"] = name;
  JsonArray saved = scene.createNestedArray("states");
  for (int i=0;i<16;i++) {
    int v = states[i] | -1;
    if (v < -1 || v > 1) v = -1;
    saved.add(v);
  }
  saveScenes();
  addEvent("Scene saved: " + name);
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleSceneApply() {
  StaticJsonDocument<128> body;
  if (deserializeJson(body, server.arg("plain"))) {
    server.send(400, "application/json", "{\"error\":\"Invalid JSON\"}"); return;
  }
  int idx = body["index"] | -1;
  JsonArray arr = scenesDoc.as<JsonArray>();
  if (idx < 0 || idx >= (int)arr.size()) {
    server.send(404, "application/json", "{\"error\":\"Scene not found\"}"); return;
  }
  JsonArray states = arr[idx]["states"].as<JsonArray>();
  JsonArray en = configDoc["enabled"].as<JsonArray>();
  for (int i=0;i<16;i++) {
    int v = states[i] | -1;
    if (v == -1) continue;
    if (en.size() == 16 && !en[i].as<bool>()) continue;
    if (i >= 4 && !nanoOnline) {
      server.send(503, "application/json", "{\"error\":\"Nano offline; scene partially not applied\"}");
      return;
    }
  }
  for (int i=0;i<16;i++) {
    int v = states[i] | -1;
    if (v == -1) continue;
    if (en.size() == 16 && !en[i].as<bool>()) continue;
    setRelay(i, v == 1);
  }
  String sceneName = arr[idx]["name"].as<String>();
  addEvent("Scene applied: " + sceneName);
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleSceneDelete() {
  StaticJsonDocument<128> body;
  if (deserializeJson(body, server.arg("plain"))) {
    server.send(400, "application/json", "{\"error\":\"Invalid JSON\"}"); return;
  }
  int idx = body["index"] | -1;
  JsonArray arr = scenesDoc.as<JsonArray>();
  if (idx < 0 || idx >= (int)arr.size()) {
    server.send(404, "application/json", "{\"error\":\"Scene not found\"}"); return;
  }
  arr.remove(idx);
  saveScenes();
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleEvents() {
  sendJson(200, logDoc);
}

void setupRoutes() {
  server.on("/", HTTP_GET, []() {
    if (LittleFS.exists("/index.html")) {
      File f = LittleFS.open("/index.html", "r");
      server.streamFile(f, "text/html; charset=utf-8");
      f.close();
    } else {
      server.send(500, "text/plain", "Missing /index.html. Upload the LittleFS data folder.");
    }
  });
  server.on("/api/state", HTTP_GET, handleState);
  server.on("/api/config", HTTP_POST, handleConfigPost);
  server.on("/api/relay", HTTP_POST, handleRelayPost);
  server.on("/api/timer", HTTP_POST, handleTimerPost);
  server.on("/api/scenes", HTTP_GET, [](){ sendJson(200, scenesDoc); });
  server.on("/api/scenes", HTTP_POST, handleScenesPost);
  server.on("/api/scenes/apply", HTTP_POST, handleSceneApply);
  server.on("/api/scenes/delete", HTTP_POST, handleSceneDelete);
  server.on("/api/events", HTTP_GET, handleEvents);
  server.onNotFound([](){ server.send(404, "application/json", "{\"error\":\"Not found\"}"); });
}

void startWifi() {
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 18000) {
    delay(250);
    yield();
  }
  if (WiFi.status() != WL_CONNECTED) {
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP(SETUP_AP_NAME, SETUP_AP_PASSWORD);
    addEvent("Router Wi-Fi unavailable; setup AP active");
  } else {
    configTime(0, 0, "pool.ntp.org", "time.nist.gov");
    addEvent("Wi-Fi connected: " + WiFi.localIP().toString());
  }
}

void setup() {
  for (int i=0;i<4;i++) {
    digitalWrite(ESP_RELAY_PINS[i], ESP_RELAY_ACTIVE_LOW ? HIGH : LOW);
    pinMode(ESP_RELAY_PINS[i], OUTPUT);
    relayStates[i] = false;
  }
  Serial.begin(115200);
  nanoSerial.begin(19200);
  if (!LittleFS.begin()) {
    Serial.println("LittleFS mount failed");
  }
  loadPersistentData();
  startWifi();
  setupRoutes();
  server.begin();
  Serial.println();
  Serial.println("PHANTOM ready");
  Serial.print("STA IP: "); Serial.println(WiFi.localIP());
  if (WiFi.getMode() & WIFI_AP) {
    Serial.print("Setup AP: "); Serial.println(WiFi.softAPIP());
  }
}

void loop() {
  server.handleClient();
  pollNano();

  unsigned long now = millis();
  bool timersChanged = false;
  for (int i=0;i<16;i++) {
    if (relayTimers[i].active && (int32_t)(now - relayTimers[i].deadline) >= 0) {
      bool target = relayTimers[i].target;
      relayTimers[i].active = false;
      if (i >= 4 && !nanoOnline) {
        addEvent("Timer due but Nano offline for relay " + String(i+1));
      } else {
        setRelay(i, target);
      }
      timersChanged = true;
    }
  }
  if (timersChanged) saveTimers();

  static unsigned long wifiCheck = 0;
  if (now - wifiCheck > 10000) {
    wifiCheck = now;
    if (WiFi.status() != WL_CONNECTED && WiFi.getMode() == WIFI_STA) {
      WiFi.reconnect();
    }
  }
  yield();
}
