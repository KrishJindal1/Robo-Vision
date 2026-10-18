#include <Arduino.h>
#include <Wire.h>
#include <ArduinoJson.h>
#include <esp_system.h>

const int ANALOG_PIN = 34;
const int LED_PIN = 2;      // onboard LED on most ESP32 DevKit boards
const int BAD_LIMIT = 3;    // bad readings in a row before reporting FAULT

int badCount = 0;
unsigned long nextTick = 0;

// LED guided test state (the robot owns this, the page only displays it)
String ledStatus = "UNKNOWN";
String ledDetail = "not tested";
int blinksLeft = 0;
unsigned long nextBlink = 0;

String inLine = "";         // command line being received

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

void sendStatus() {
  JsonDocument d;
  d["v"] = 1;
  d["type"] = "status";
  d["t"] = millis();
  JsonArray comps = d["components"].to<JsonArray>();

  // I2C bus scan
  String found = "";
  int count = 0;
  for (int addr = 1; addr < 127; addr++) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) {
      char buf[8];
      snprintf(buf, sizeof(buf), "0x%02X ", addr);
      found += buf;
      count++;
    }
  }
  JsonObject i2c = comps.add<JsonObject>();
  i2c["id"] = "i2c_bus";
  i2c["status"] = count > 0 ? "OK" : "NOT_FOUND";
  i2c["detail"] = count > 0 ? found : String("no devices");

  // Analog pin with debounce
  int raw = analogRead(ANALOG_PIN);
  bool bad = (raw <= 5 || raw >= 4090);
  badCount = bad ? badCount + 1 : 0;
  JsonObject an = comps.add<JsonObject>();
  an["id"] = "analog_34";
  an["value"] = raw;
  if (badCount >= BAD_LIMIT) {
    an["status"] = "FAULT";
    an["detail"] = raw <= 5 ? "stuck low" : "stuck high";
  } else {
    an["status"] = "OK";
  }

  // Onboard LED (verified by a human)
  JsonObject led = comps.add<JsonObject>();
  led["id"] = "led_onboard";
  led["status"] = ledStatus;
  led["detail"] = ledDetail;

  serializeJson(d, Serial);
  Serial.println();
}

void handleCommand(const String& line) {
  JsonDocument d;
  if (deserializeJson(d, line)) return;          // not valid JSON: ignore
  String type = d["type"] | "";
  String id = d["id"] | "";

  if (type == "action" && id == "led_onboard") {
    String name = d["name"] | "";
    if (name == "test") {
      blinksLeft = 10;                           // 10 toggles = 5 blinks
      nextBlink = millis();
      ledStatus = "UNKNOWN";
      ledDetail = "waiting for your answer";
    }
  } else if (type == "manual_result" && id == "led_onboard") {
    bool pass = d["pass"] | false;
    ledStatus = pass ? "MANUAL_PASS" : "MANUAL_FAIL";
    ledDetail = "confirmed by you";
  }
}

void readCommands() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n') {
      handleCommand(inLine);
      inLine = "";
    } else if (c != '\r' && inLine.length() < 256) {
      inLine += c;
    }
  }
}

void updateBlink() {
  if (blinksLeft > 0 && millis() >= nextBlink) {
    digitalWrite(LED_PIN, !digitalRead(LED_PIN));
    blinksLeft--;
    nextBlink = millis() + 200;
    if (blinksLeft == 0) digitalWrite(LED_PIN, LOW);
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);
  delay(500);
  Wire.begin(21, 22);
  sendBoot();
}

void loop() {
  readCommands();
  updateBlink();
  if (millis() >= nextTick) {
    nextTick += 1000;
    sendStatus();
  }
}
