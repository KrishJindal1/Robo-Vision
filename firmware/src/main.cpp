#include <Arduino.h>
#include <esp_system.h>

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.printf("{\"v\":1,\"type\":\"boot\",\"reset_reason\":%d}\n",
                (int)esp_reset_reason());
}

void loop() {
  Serial.printf("{\"v\":1,\"type\":\"status\",\"t\":%lu}\n", millis());
  delay(1000);
}
