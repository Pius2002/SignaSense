// Minimal Digispark / ATtiny85 test sketch.
// Built for: Digispark (Default - 16.5 MHz)
// LED is usually on P1 for common Digispark-style boards.

const uint8_t LED_PIN = 1;

void setup() {
  pinMode(LED_PIN, OUTPUT);
}

void loop() {
  digitalWrite(LED_PIN, HIGH);
  delay(300);
  digitalWrite(LED_PIN, LOW);
  delay(700);
}
