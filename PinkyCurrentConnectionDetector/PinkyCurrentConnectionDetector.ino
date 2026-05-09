/*
  Pinky GPIO34 detector for the current high-side connection.

  This is a temporary diagnostic. It tries to detect bend from a GPIO34 raw drop
  below the observed straight/high baseline. A proper voltage divider is still
  recommended:
    3V3 -> flex sensor -> GPIO34 -> 10k resistor -> GND

  If this sketch stays near 4095 with only tiny changes, the current connection
  cannot provide reliable bend detection.
*/

#include <Arduino.h>

const int PINKY_PIN = 34;
const int SAMPLE_COUNT = 32;
const int BENT_DROP_THRESHOLD = 45;
const int HALF_BENT_DROP_THRESHOLD = 20;

float smoothedRaw = 0.0f;
int straightBaseline = 0;
bool baselineReady = false;

int readAverage() {
  long total = 0;
  for (int i = 0; i < SAMPLE_COUNT; i++) {
    total += analogRead(PINKY_PIN);
    delay(2);
  }
  return total / SAMPLE_COUNT;
}

void setup() {
  Serial.begin(115200);
  delay(800);

  analogReadResolution(12);
  pinMode(PINKY_PIN, INPUT);
  analogSetPinAttenuation(PINKY_PIN, ADC_11db);

  Serial.println();
  Serial.println("Pinky GPIO34 current-connection bend detector");
  Serial.println("Hold pinky straight for 3 seconds, then bend and release slowly.");
}

void loop() {
  const int raw = readAverage();

  if (!baselineReady) {
    smoothedRaw = raw;
    straightBaseline = raw;
    baselineReady = true;
  } else {
    smoothedRaw = (0.20f * raw) + (0.80f * smoothedRaw);
    if (raw > straightBaseline) {
      straightBaseline = raw;
    }
  }

  const int filtered = (int)(smoothedRaw + 0.5f);
  const int drop = straightBaseline - filtered;
  const char *state = "STRAIGHT";

  if (drop >= BENT_DROP_THRESHOLD) {
    state = "BENT";
  } else if (drop >= HALF_BENT_DROP_THRESHOLD) {
    state = "HALF";
  }

  Serial.print("GPIO34 raw=");
  Serial.print(raw);
  Serial.print(" filtered=");
  Serial.print(filtered);
  Serial.print(" baseline=");
  Serial.print(straightBaseline);
  Serial.print(" drop=");
  Serial.print(drop);
  Serial.print(" state=");
  Serial.println(state);

  delay(160);
}
