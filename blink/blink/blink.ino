#include <Arduino.h>

const int LED_PIN = 2;

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("boot ok");
  pinMode(LED_PIN, OUTPUT);
}

void loop() {
    digitalWrite(LED_PIN, HIGH);
    delay(500);
    digitalWrite(LED_PIN, LOW);
    delay(500);
    Serial.println("tick");
}
