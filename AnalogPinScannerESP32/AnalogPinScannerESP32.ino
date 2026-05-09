/*
  Temporary ESP32 analog pin scanner for the SignaSense glove.

  Upload this only for wiring diagnosis. It prints average analog readings on
  common ESP32 ADC pins so we can find where the flex sensor voltage dividers
  are actually connected.
*/

#include <Arduino.h>

const int SCAN_PINS[] = {
  32, 33, 34, 35, 25, 26, 27, 14, 13, 12, 36, 39, 4, 2, 15
};

const byte NUM_SCAN_PINS = sizeof(SCAN_PINS) / sizeof(SCAN_PINS[0]);

int readAverage(int pin) {
  long total = 0;
  for (byte i = 0; i < 16; i++) {
    total += analogRead(pin);
    delay(2);
  }
  return total / 16;
}

void setup() {
  Serial.begin(115200);
  delay(800);

  analogReadResolution(12);
  for (byte i = 0; i < NUM_SCAN_PINS; i++) {
    pinMode(SCAN_PINS[i], INPUT);
    analogSetPinAttenuation(SCAN_PINS[i], ADC_11db);
  }

  Serial.println();
  Serial.println("ESP32 analog pin scanner ready.");
  Serial.println("Bend each flex sensor and watch which GPIO value changes.");
}

void loop() {
  for (byte i = 0; i < NUM_SCAN_PINS; i++) {
    Serial.print("GPIO");
    Serial.print(SCAN_PINS[i]);
    Serial.print("=");
    Serial.print(readAverage(SCAN_PINS[i]));
    if (i < NUM_SCAN_PINS - 1) {
      Serial.print(" | ");
    }
  }
  Serial.println();
  delay(700);
}
