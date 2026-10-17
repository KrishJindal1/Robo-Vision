#include <Arduino.h>
#include <Wire.h>
#include <ArduinoJson.h>
#include <esp_system.h>

const int ANALOG_PIN = 34;
const int BAD_LIMIT = 3;   // bad readings in a row before reporting FAULT

int badCount = 0;
unsigned long nextTick = 0;

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

  serializeJson(d, Serial);
  Serial.println();
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Wire.begin(21, 22);
  sendBoot();
}

void loop() {
  if (millis() >= nextTick) {
    nextTick += 1000;
    sendStatus();
  }
}
