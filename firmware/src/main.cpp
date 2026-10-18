#include <Arduino.h>
#include <Wire.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <esp_system.h>
#include <vector>

const int BAD_LIMIT = 3;              // bad checks in a row before reporting a problem

std::vector<uint8_t> claimed;         // I2C addresses you declared
Preferences prefs;                    // flash storage that survives reboots
JsonDocument cfg;                     // saved list of declared components

// Pins that are safe to use (no flash pins, no I2C pins, no boot-strapping pins)
const int OUT_PINS[] = {2, 4, 5, 13, 14, 16, 17, 18, 19, 23, 25, 26, 27, 32, 33};
const int ADC_PINS[] = {32, 33, 34, 35, 36, 39};
const int IN_PINS[]  = {4, 5, 13, 14, 16, 17, 18, 19, 23, 25, 26, 27, 32, 33, 34, 35, 36, 39};

template <size_t N>
bool inList(int p, const int (&l)[N]) {
  for (size_t i = 0; i < N; i++) if (l[i] == p) return true;
  return false;
}

bool i2cPresent(uint8_t addr) {
  Wire.beginTransmission(addr);
  return Wire.endTransmission() == 0;
}

// ---------------------------------------------------------------------------
// Base class: every part is a Component
// ---------------------------------------------------------------------------
class Component {
public:
  String id;
  String status = "UNKNOWN";
  String detail = "";
  bool hasValue = false;
  int value = 0;

  Component(const char* i) : id(i) {}
  virtual ~Component() {}
  virtual void check() {}
  virtual void tick() {}
  virtual void onAction(const String&) {}
  virtual void onManual(bool) {}
};

// An I2C device you expect at a known address
class I2CDevice : public Component {
  uint8_t addr;
  int misses = 0;
public:
  I2CDevice(const char* i, uint8_t a) : Component(i), addr(a) { claimed.push_back(a); }
  void check() override {
    char buf[24];
    snprintf(buf, sizeof(buf), "address 0x%02X", addr);
    if (i2cPresent(addr)) {
      misses = 0;
      status = "OK";
      detail = buf;
    } else {
      if (misses < BAD_LIMIT) misses++;
      if (misses >= BAD_LIMIT) {
        status = "NOT_FOUND";
        detail = String(buf) + " no response";
      }
    }
  }
};

// Scans the whole I2C bus and flags devices you did not declare
class I2CBusScan : public Component {
public:
  I2CBusScan(const char* i) : Component(i) {}
  void check() override {
    String unexpected = "";
    int total = 0;
    for (uint8_t a = 1; a < 127; a++) {
      if (!i2cPresent(a)) continue;
      total++;
      bool known = false;
      for (uint8_t c : claimed) if (c == a) known = true;
      if (!known) {
        char buf[8];
        snprintf(buf, sizeof(buf), "0x%02X ", a);
        unexpected += buf;
      }
    }
    if (unexpected.length() > 0) {
      status = "FAULT";
      detail = "unexpected device: " + unexpected;
    } else {
      status = "OK";
      detail = total > 0 ? "all devices expected" : "no devices on bus";
    }
  }
};

// An analog input. Stuck at the bottom or top of the range means a problem.
class AnalogPin : public Component {
  int pin;
  int bad = 0;
public:
  AnalogPin(const char* i, int p) : Component(i), pin(p) { hasValue = true; }
  void check() override {
    value = analogRead(pin);
    bool isBad = (value <= 5 || value >= 4090);
    bad = isBad ? bad + 1 : 0;
    if (bad >= BAD_LIMIT) {
      status = "FAULT";
      detail = value <= 5 ? "stuck low" : "stuck high";
    } else {
      status = "OK";
      detail = "";
    }
  }
};

// An ultrasonic distance sensor (HC-SR04 style). No echo means a problem.
class Ultrasonic : public Component {
  int trig, echo;
  int bad = 0;
public:
  Ultrasonic(const char* i, int t, int e) : Component(i), trig(t), echo(e) {
    pinMode(trig, OUTPUT);
    pinMode(echo, INPUT);
    hasValue = true;
  }
  void check() override {
    digitalWrite(trig, LOW);
    delayMicroseconds(2);
    digitalWrite(trig, HIGH);
    delayMicroseconds(10);
    digitalWrite(trig, LOW);
    unsigned long us = pulseIn(echo, HIGH, 25000);   // 25 ms timeout
    if (us == 0) {
      bad++;
      if (bad >= BAD_LIMIT) {
        status = "FAULT";
        detail = "no echo";
        value = 0;
      }
    } else {
      bad = 0;
      value = us / 58;                               // centimetres
      status = "OK";
      detail = "distance in cm";
    }
  }
};

// An LED that a person confirms (no feedback hardware needed)
class ManualLed : public Component {
  int pin;
  int blinksLeft = 0;
  unsigned long nextBlink = 0;
public:
  ManualLed(const char* i, int p) : Component(i), pin(p) {
    pinMode(pin, OUTPUT);
    digitalWrite(pin, LOW);
    detail = "not tested";
  }
  void onAction(const String& name) override {
    if (name == "test") {
      blinksLeft = 10;
      nextBlink = millis();
      status = "UNKNOWN";
      detail = "waiting for your answer";
    }
  }
  void onManual(bool pass) override {
    status = pass ? "MANUAL_PASS" : "MANUAL_FAIL";
    detail = "confirmed by you";
  }
  void tick() override {
    if (blinksLeft > 0 && millis() >= nextBlink) {
      digitalWrite(pin, !digitalRead(pin));
      blinksLeft--;
      nextBlink = millis() + 200;
      if (blinksLeft == 0) digitalWrite(pin, LOW);
    }
  }
};

// ---------------------------------------------------------------------------
// Registry, built from the saved configuration
// ---------------------------------------------------------------------------
std::vector<Component*> registry;
unsigned long nextTick = 0;
String inLine = "";

Component* build(JsonObject c) {
  String type = c["type"] | "";
  String id = c["id"] | "";
  if (type == "i2c")        return new I2CDevice(id.c_str(), c["addr"].as<int>());
  if (type == "analog")     return new AnalogPin(id.c_str(), c["pin"].as<int>());
  if (type == "led")        return new ManualLed(id.c_str(), c["pin"].as<int>());
  if (type == "ultrasonic") return new Ultrasonic(id.c_str(), c["trig"].as<int>(), c["echo"].as<int>());
  return nullptr;
}

void rebuildRegistry() {
  for (Component* c : registry) delete c;
  registry.clear();
  claimed.clear();
  registry.push_back(new I2CBusScan("i2c_bus"));     // always present
  for (JsonObject o : cfg.as<JsonArray>()) {
    Component* c = build(o);
    if (c) registry.push_back(c);
  }
}

void saveConfig() {
  String s;
  serializeJson(cfg, s);
  prefs.putString("cfg", s);
}

void loadConfig() {
  String s = prefs.getString("cfg", "");
  JsonDocument tmp;
  if (s.length() > 0 && !deserializeJson(tmp, s) && tmp.is<JsonArray>()) {
    cfg = tmp;
    return;
  }
  // First run: start with just the onboard LED
  JsonArray arr = cfg.to<JsonArray>();
  JsonObject led = arr.add<JsonObject>();
  led["id"] = "led_onboard";
  led["type"] = "led";
  led["pin"] = 2;
  saveConfig();
}

// ---------------------------------------------------------------------------
// Messages to the dashboard
// ---------------------------------------------------------------------------
const char* resetName(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON:  return "POWERON";
    case ESP_RST_EXT:      return "EXTERNAL";
    case ESP_RST_SW:       return "SOFTWARE";
    case ESP_RST_PANIC:    return "CRASH";
    case ESP_RST_INT_WDT:  return "WDT_INT";
    case ESP_RST_TASK_WDT: return "WDT_TASK";
    case ESP_RST_WDT:      return "WDT";
    case ESP_RST_BROWNOUT: return "BROWNOUT";
    default:               return "OTHER";
  }
}

void sendBoot() {
  JsonDocument d;
  d["v"] = 1;
  d["type"] = "boot";
  d["reset_reason"] = resetName(esp_reset_reason());
  serializeJson(d, Serial);
  Serial.println();
}

void sendError(const String& msg) {
  JsonDocument d;
  d["v"] = 1;
  d["type"] = "error";
  d["message"] = msg;
  serializeJson(d, Serial);
  Serial.println();
}

void sendConfig() {
  JsonDocument d;
  d["v"] = 1;
  d["type"] = "config";
  JsonArray out = d["components"].to<JsonArray>();
  for (JsonObject o : cfg.as<JsonArray>()) out.add(o);
  serializeJson(d, Serial);
  Serial.println();
}

void sendStatus() {
  JsonDocument d;
  d["v"] = 1;
  d["type"] = "status";
  d["t"] = millis();
  JsonArray comps = d["components"].to<JsonArray>();
  for (Component* c : registry) {
    c->check();
    JsonObject o = comps.add<JsonObject>();
    o["id"] = c->id;
    o["status"] = c->status;
    if (c->detail.length() > 0) o["detail"] = c->detail;
    if (c->hasValue) o["value"] = c->value;
  }
  serializeJson(d, Serial);
  Serial.println();
}

// ---------------------------------------------------------------------------
// Commands from the dashboard
// ---------------------------------------------------------------------------
bool validate(JsonObject c, String& err) {
  String id = c["id"] | "";
  String type = c["type"] | "";
  if (id.length() < 1 || id.length() > 32) { err = "id must be 1 to 32 characters"; return false; }
  for (size_t i = 0; i < id.length(); i++) {
    char ch = id[i];
    if (!((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_')) {
      err = "id may only use a-z, 0-9 and _";
      return false;
    }
  }
  if (id == "i2c_bus") { err = "that id is reserved"; return false; }
  JsonArray arr = cfg.as<JsonArray>();
  for (JsonObject o : arr) {
    if (String(o["id"] | "") == id) { err = "id already exists"; return false; }
  }
  if (type == "i2c") {
    int a = c["addr"] | -1;
    if (a < 1 || a > 126) { err = "address must be 1 to 126"; return false; }
    for (JsonObject o : arr) {
      if (String(o["type"] | "") == "i2c" && o["addr"].as<int>() == a) { err = "address already declared"; return false; }
    }
    return true;
  }
  if (type == "analog") {
    int p = c["pin"] | -1;
    if (!inList(p, ADC_PINS)) { err = "analog pin must be 32, 33, 34, 35, 36 or 39"; return false; }
    return true;
  }
  if (type == "led") {
    int p = c["pin"] | -1;
    if (!inList(p, OUT_PINS)) { err = "that pin cannot be used as an output"; return false; }
    return true;
  }
  if (type == "ultrasonic") {
    int t = c["trig"] | -1;
    int e = c["echo"] | -1;
    if (!inList(t, OUT_PINS)) { err = "trig pin cannot be used as an output"; return false; }
    if (!inList(e, IN_PINS)) { err = "echo pin cannot be used as an input"; return false; }
    if (t == e) { err = "trig and echo must be different pins"; return false; }
    return true;
  }
  err = "unknown type";
  return false;
}

void addToConfig(JsonObject c) {
  String type = c["type"] | "";
  JsonObject n = cfg.as<JsonArray>().add<JsonObject>();
  n["id"] = String(c["id"] | "");
  n["type"] = type;
  if (type == "i2c") n["addr"] = c["addr"].as<int>();
  if (type == "analog" || type == "led") n["pin"] = c["pin"].as<int>();
  if (type == "ultrasonic") {
    n["trig"] = c["trig"].as<int>();
    n["echo"] = c["echo"].as<int>();
  }
}

void applyChange() {
  saveConfig();
  rebuildRegistry();
  sendConfig();
  sendStatus();
}

void handleCommand(const String& line) {
  JsonDocument d;
  if (deserializeJson(d, line)) return;            // not valid JSON: ignore
  String type = d["type"] | "";

  if (type == "selftest")   { sendStatus(); return; }
  if (type == "get_config") { sendConfig(); return; }

  if (type == "add") {
    JsonObject comp = d["comp"];
    String err;
    if (comp.isNull()) { sendError("missing comp"); return; }
    if (!validate(comp, err)) { sendError(err); return; }
    addToConfig(comp);
    applyChange();
    return;
  }

  if (type == "remove") {
    String id = d["id"] | "";
    JsonArray arr = cfg.as<JsonArray>();
    bool found = false;
    for (size_t i = 0; i < arr.size(); i++) {
      if (String(arr[i]["id"] | "") == id) { arr.remove(i); found = true; break; }
    }
    if (!found) { sendError("no such component"); return; }
    applyChange();
    return;
  }

  if (type == "clear_config") {
    cfg.to<JsonArray>();
    applyChange();
    return;
  }

  String id = d["id"] | "";
  for (Component* c : registry) {
    if (c->id != id) continue;
    if (type == "action") c->onAction(String(d["name"] | ""));
    else if (type == "manual_result") c->onManual(d["pass"] | false);
  }
}

void readCommands() {
  while (Serial.available()) {
    char ch = Serial.read();
    if (ch == '\n') {
      handleCommand(inLine);
      inLine = "";
    } else if (ch != '\r' && inLine.length() < 256) {
      inLine += ch;
    }
  }
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Wire.begin(21, 22);                              // SDA, SCL
  prefs.begin("robotdoctor", false);
  loadConfig();
  rebuildRegistry();
  sendBoot();
  sendConfig();
}

void loop() {
  readCommands();
  for (Component* c : registry) c->tick();
  if (millis() >= nextTick) {
    nextTick += 1000;
    sendStatus();
  }
}
