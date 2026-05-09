/*
  SignaSense glove final-pin flex sensor test.

  This diagnostic reads only the final wiring map:
    Thumb  -> GPIO25
    Index  -> GPIO33
    Middle -> GPIO32
    Ring   -> GPIO35
    Pinky  -> GPIO34

  Correct voltage-divider wiring for every flex sensor:
    3.3V -> flex sensor -> ADC GPIO pin -> 10k resistor -> GND

  All five sensors may share the same ESP32 GND, but each sensor must have its
  own resistor to GND. Open Serial Monitor at 115200 baud and bend one finger at
  a time. The matching raw value and delta should change clearly.
*/

#include <Arduino.h>

struct FingerPin {
  const char *name;
  int pin;
};

const FingerPin FINGERS[] = {
  {"Thumb", 25},
  {"Index", 33},
  {"Middle", 32},
  {"Ring", 35},
  {"Pinky", 34},
};

const byte NUM_FINGERS = sizeof(FINGERS) / sizeof(FINGERS[0]);
const byte SAMPLES = 24;

int readAverage(int pin) {
  long total = 0;
  for (byte i = 0; i < SAMPLES; i++) {
    total += analogRead(pin);
    delay(2);
  }
  return total / SAMPLES;
}

void setup() {
  Serial.begin(115200);
  delay(900);

  analogReadResolution(12);
  for (byte i = 0; i < NUM_FINGERS; i++) {
    pinMode(FINGERS[i].pin, INPUT);
    analogSetPinAttenuation(FINGERS[i].pin, ADC_11db);
  }

  Serial.println();
  Serial.println("SignaSense final flex pin test");
  Serial.println("Final pins: Thumb GPIO25, Index GPIO33, Middle GPIO32, Ring GPIO35, Pinky GPIO34");
  Serial.println("Bend one finger at a time. A working sensor should change by about 50+ ADC counts.");
  Serial.println();
}

void loop() {
  for (byte i = 0; i < NUM_FINGERS; i++) {
    const int pin = FINGERS[i].pin;
    int low = 4095;
    int high = 0;
    long total = 0;

    for (byte sample = 0; sample < 10; sample++) {
      const int value = readAverage(pin);
      low = min(low, value);
      high = max(high, value);
      total += value;
    }

    const int avg = total / 10;
    const int delta = high - low;

    Serial.print(FINGERS[i].name);
    Serial.print("(GPIO");
    Serial.print(pin);
    Serial.print(") raw=");
    Serial.print(avg);
    Serial.print(" min=");
    Serial.print(low);
    Serial.print(" max=");
    Serial.print(high);
    Serial.print(" delta=");
    Serial.print(delta);

    if (avg <= 5) {
      Serial.print(" [LOW/ground/no divider voltage]");
    } else if (avg >= 4090) {
      Serial.print(" [HIGH/3.3V/no resistor path]");
    } else if (delta < 8) {
      Serial.print(" [stable]");
    } else {
      Serial.print(" [moving]");
    }

    if (i < NUM_FINGERS - 1) {
      Serial.print(" | ");
    }
  }

  Serial.println();
  delay(500);
}
