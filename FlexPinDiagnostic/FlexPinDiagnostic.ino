/*
  Flex sensor wide pin diagnostic for ESP32.

  This sketch scans the candidate analog pins that were used in earlier glove
  wiring attempts so we can see which pins actually carry flex-sensor voltage.

  Tested pins:
  - ADC1: GPIO32, GPIO33, GPIO34, GPIO35, GPIO36, GPIO39
  - ADC2: GPIO25, GPIO26, GPIO27, GPIO14, GPIO13, GPIO12

  Important:
  - This sketch intentionally does not use Wi-Fi or Bluetooth, so ADC2 pins can
    be read here.
  - If a pin reads 0 continuously, the ESP32 is seeing that pin at ground level
    or it is not receiving the divider midpoint voltage.
*/

const int scanPins[] = {
  32, 33, 34, 35, 36, 39,
  25, 26, 27, 14, 13, 12
};

const char *scanNames[] = {
  "GPIO32", "GPIO33", "GPIO34", "GPIO35", "GPIO36", "GPIO39",
  "GPIO25", "GPIO26", "GPIO27", "GPIO14", "GPIO13", "GPIO12"
};

const int scanCount = sizeof(scanPins) / sizeof(scanPins[0]);

void setup() {
  Serial.begin(115200);
  delay(500);

  analogReadResolution(12);

  for (int i = 0; i < scanCount; i++) {
    pinMode(scanPins[i], INPUT);
    analogSetPinAttenuation(scanPins[i], ADC_11db);
  }

  Serial.println();
  Serial.println("ESP32 wide flex pin diagnostic starting...");
  Serial.println("Move each finger and watch which GPIO value changes.");
  Serial.println("Non-zero and changing values indicate the sensor divider is reaching that pin.");
}

void loop() {
  for (int i = 0; i < scanCount; i++) {
    Serial.print(scanNames[i]);
    Serial.print(":");
    Serial.print(analogRead(scanPins[i]));

    if (i < scanCount - 1) {
      Serial.print(" | ");
    }
  }

  Serial.println();
  delay(350);
}
