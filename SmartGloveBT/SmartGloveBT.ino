/*
  ESP32 Smart Glove - 5 Flex Sensors + Bluetooth Speaker Audio Events

  Hardware:
  - ESP32 board
  - 5 flex sensors
  - Each flex sensor is wired as a voltage divider powered from 3.3V.
  - Each voltage divider output goes to one ESP32 analog input.
  - All sensor grounds and ESP32 GND must be common.

  Pin summary:
  - Thumb  flex sensor output -> GPIO 32
  - Index  flex sensor output -> GPIO 33
  - Middle flex sensor output -> GPIO 26
  - Ring   flex sensor output -> GPIO 14
  - Pinky  flex sensor output -> GPIO 25

  Note:
  GPIO32/GPIO33 are ADC1 analog input pins. GPIO14/GPIO25/GPIO26 are ADC2
  analog input pins; do not enable WiFi while using ADC2 flex sensor reads.

  Wiring per sensor:
  - One end of flex sensor -> 3.3V
  - Other end of flex sensor -> analog pin and one end of fixed resistor
  - Other end of fixed resistor -> GND
  If your divider is reversed, calibration still works because the code uses
  separate straight and bent calibration values for each finger.

  Library required:
  - ESP32-A2DP by Phil Schatzmann
  - GitHub: https://github.com/pschatzmann/ESP32-A2DP
  - This sketch uses BluetoothA2DPSource to stream generated PCM tones to a
    Bluetooth speaker/headphones.

  Bluetooth audio reality:
  - ESP32 can stream audio to a Bluetooth speaker using A2DP Source.
  - ESP32 does not provide practical full text-to-speech by itself in a simple
    Arduino sketch. Full speech would need prerecorded audio, an external
    speech module, or a much larger TTS/audio system.
  - Best practical alternative used here: recognizable audio events.
    Letters are sent as Morse-like tone patterns; phrase gestures use short
    melodies. The exact recognized text is always printed to Serial.

  Sign language limitation:
  - Five flex sensors only measure finger bend.
  - Real ASL/sign language also depends on palm orientation, hand location,
    movement, two-hand interaction, and facial/body expression.
  - The gesture table below is therefore ASL-inspired and user-calibrated,
    not a complete sign-language recognizer.
*/

#include <Arduino.h>
#include <math.h>
#include <string.h>
#include "BluetoothA2DPSource.h"

// ---------------------------------------------------------------------------
// Bluetooth speaker setup
// ---------------------------------------------------------------------------

// Replace this with the exact Bluetooth speaker/headphone name you want.
// Leave as-is for compile/testing without starting Bluetooth connection.
const char TARGET_BT_SPEAKER_NAME[] = "PUT_SPEAKER_NAME_HERE";

const bool START_BLUETOOTH_ON_BOOT = true;
// With ESP32-A2DP 1.4.x source mode, volume is normally controlled on the
// Bluetooth speaker itself. Newer library versions expose more controls but
// need a newer ESP32 Arduino core.
const byte BT_VOLUME_PERCENT_NOTE_ONLY = 70;

// ---------------------------------------------------------------------------
// Finger pin mapping
// ---------------------------------------------------------------------------

enum FingerIndex {
  FINGER_THUMB = 0,
  FINGER_INDEX = 1,
  FINGER_MIDDLE = 2,
  FINGER_RING = 3,
  FINGER_PINKY = 4,
  NUM_FINGERS = 5
};

const char *FINGER_NAMES[NUM_FINGERS] = {
  "Thumb",
  "Index",
  "Middle",
  "Ring",
  "Pinky"
};

const int FINGER_PINS[NUM_FINGERS] = {
  32, // Thumb  - ADC1 pin
  33, // Index  - ADC1 pin
  26, // Middle - ADC2 pin
  14, // Ring   - ADC2 pin
  25  // Pinky  - ADC2 pin; do not enable WiFi while using this analog input
};

// ---------------------------------------------------------------------------
// Calibration values
// ---------------------------------------------------------------------------

/*
  Adjust these after testing your actual flex sensors.

  How to calibrate:
  1. Set RUN_STARTUP_CALIBRATION to true.
  2. Upload and open Serial Monitor at 115200.
  3. Follow the prompts: hold all fingers straight, then all bent.
  4. Copy the printed arrays back into straightCalibration and bentCalibration.
  5. Set RUN_STARTUP_CALIBRATION back to false.

  Values are 12-bit ADC readings: about 0 to 4095.
*/
bool RUN_STARTUP_CALIBRATION = false;

int straightCalibration[NUM_FINGERS] = {
  1200, // Thumb straight
  1200, // Index straight
  1200, // Middle straight
  1200, // Ring straight
  1200  // Pinky straight
};

int bentCalibration[NUM_FINGERS] = {
  3000, // Thumb bent
  3000, // Index bent
  3000, // Middle bent
  3000, // Ring bent
  3000  // Pinky bent
};

// Bend percentage thresholds. Easy to tune after calibration.
const int STRAIGHT_MAX_PERCENT = 30;
const int BENT_MIN_PERCENT = 70;

// Smoothing. Higher alpha reacts faster; lower alpha is smoother.
const float SMOOTHING_ALPHA = 0.22;

// Serial print and gesture timing.
const unsigned long SENSOR_READ_INTERVAL_MS = 25;
const unsigned long SERIAL_PRINT_INTERVAL_MS = 250;
const unsigned long GESTURE_STABLE_MS = 450;
const unsigned long REPEAT_SAME_GESTURE_AUDIO_MS = 3500;
const unsigned long SENSOR_HEALTH_WARNING_INTERVAL_MS = 2500;

// Raw ADC values this close to the rails often mean a loose wire, missing
// shared ground, or a divider connected to the wrong rail.
const int RAW_LOW_WARNING = 20;
const int RAW_HIGH_WARNING = 4075;

// Simple one-finger testing mode.
// Set true to rotate the Serial Monitor focus through each finger.
const bool SENSOR_TEST_MODE = false;
const unsigned long TEST_FINGER_INTERVAL_MS = 2500;

// ---------------------------------------------------------------------------
// Finger state and gesture rules
// ---------------------------------------------------------------------------

enum FingerState : byte {
  STATE_STRAIGHT,
  STATE_HALF_BENT,
  STATE_BENT
};

enum FingerMatch : byte {
  MATCH_STRAIGHT,
  MATCH_HALF_BENT,
  MATCH_BENT,
  MATCH_STRAIGHT_OR_HALF,
  MATCH_HALF_OR_BENT,
  MATCH_ANY
};

enum AudioMode : byte {
  AUDIO_NONE,
  AUDIO_MORSE_LETTER,
  AUDIO_MELODY
};

enum MelodyId : byte {
  MELODY_NONE,
  MELODY_HELLO,
  MELODY_FIST,
  MELODY_ILOVEYOU,
  MELODY_POINT,
  MELODY_UNKNOWN
};

struct GestureRule {
  const char *gestureName;
  const char *outputText;
  char letter;
  FingerMatch match[NUM_FINGERS];
  AudioMode audioMode;
  MelodyId melody;
};

/*
  ASL-inspired bend-pattern table.
  T, I, M, R, P order is Thumb, Index, Middle, Ring, Pinky.

  These are bend-only approximations. They do not check palm orientation,
  finger spread, movement, or the exact thumb position across the palm.
*/
const GestureRule GESTURES[] = {
  {
    "ASL_A_LIKE",
    "Letter A",
    'A',
    {MATCH_STRAIGHT_OR_HALF, MATCH_HALF_OR_BENT, MATCH_HALF_OR_BENT, MATCH_HALF_OR_BENT, MATCH_HALF_OR_BENT},
    AUDIO_MORSE_LETTER,
    MELODY_NONE
  },
  {
    "ASL_B_LIKE",
    "Letter B",
    'B',
    {MATCH_HALF_OR_BENT, MATCH_STRAIGHT_OR_HALF, MATCH_STRAIGHT_OR_HALF, MATCH_STRAIGHT_OR_HALF, MATCH_STRAIGHT_OR_HALF},
    AUDIO_MORSE_LETTER,
    MELODY_NONE
  },
  {
    "ASL_C_LIKE",
    "Letter C",
    'C',
    {MATCH_HALF_BENT, MATCH_HALF_BENT, MATCH_HALF_BENT, MATCH_HALF_BENT, MATCH_HALF_BENT},
    AUDIO_MORSE_LETTER,
    MELODY_NONE
  },
  {
    "ASL_I_LIKE",
    "Letter I",
    'I',
    {MATCH_HALF_OR_BENT, MATCH_HALF_OR_BENT, MATCH_HALF_OR_BENT, MATCH_HALF_OR_BENT, MATCH_STRAIGHT_OR_HALF},
    AUDIO_MORSE_LETTER,
    MELODY_NONE
  },
  {
    "ASL_L_LIKE",
    "Letter L",
    'L',
    {MATCH_STRAIGHT_OR_HALF, MATCH_STRAIGHT_OR_HALF, MATCH_HALF_OR_BENT, MATCH_HALF_OR_BENT, MATCH_HALF_OR_BENT},
    AUDIO_MORSE_LETTER,
    MELODY_NONE
  },
  {
    "ASL_V_LIKE",
    "Letter V",
    'V',
    {MATCH_HALF_OR_BENT, MATCH_STRAIGHT_OR_HALF, MATCH_STRAIGHT_OR_HALF, MATCH_HALF_OR_BENT, MATCH_HALF_OR_BENT},
    AUDIO_MORSE_LETTER,
    MELODY_NONE
  },
  {
    "ASL_Y_LIKE",
    "Letter Y",
    'Y',
    {MATCH_STRAIGHT_OR_HALF, MATCH_HALF_OR_BENT, MATCH_HALF_OR_BENT, MATCH_HALF_OR_BENT, MATCH_STRAIGHT_OR_HALF},
    AUDIO_MORSE_LETTER,
    MELODY_NONE
  },
  {
    "ILY_LIKE",
    "Phrase: I love you",
    '\0',
    {MATCH_STRAIGHT_OR_HALF, MATCH_STRAIGHT_OR_HALF, MATCH_HALF_OR_BENT, MATCH_HALF_OR_BENT, MATCH_STRAIGHT_OR_HALF},
    AUDIO_MELODY,
    MELODY_ILOVEYOU
  },
  {
    "POINT_LIKE",
    "Point / attention",
    '\0',
    {MATCH_HALF_OR_BENT, MATCH_STRAIGHT_OR_HALF, MATCH_HALF_OR_BENT, MATCH_HALF_OR_BENT, MATCH_HALF_OR_BENT},
    AUDIO_MELODY,
    MELODY_POINT
  },
  {
    "OPEN_HAND",
    "Open hand / hello",
    '\0',
    {MATCH_STRAIGHT, MATCH_STRAIGHT, MATCH_STRAIGHT, MATCH_STRAIGHT, MATCH_STRAIGHT},
    AUDIO_MELODY,
    MELODY_HELLO
  },
  {
    "CLOSED_HAND",
    "Closed hand / stop",
    '\0',
    {MATCH_BENT, MATCH_BENT, MATCH_BENT, MATCH_BENT, MATCH_BENT},
    AUDIO_MELODY,
    MELODY_FIST
  }
};

const int NUM_GESTURES = sizeof(GESTURES) / sizeof(GESTURES[0]);
const int GESTURE_UNKNOWN = -1;

// ---------------------------------------------------------------------------
// Runtime sensor state
// ---------------------------------------------------------------------------

int rawValues[NUM_FINGERS] = {0, 0, 0, 0, 0};
float smoothedValues[NUM_FINGERS] = {0, 0, 0, 0, 0};
int bendPercent[NUM_FINGERS] = {0, 0, 0, 0, 0};
FingerState fingerStates[NUM_FINGERS] = {
  STATE_STRAIGHT,
  STATE_STRAIGHT,
  STATE_STRAIGHT,
  STATE_STRAIGHT,
  STATE_STRAIGHT
};

bool smoothingReady = false;

unsigned long lastSensorReadMs = 0;
unsigned long lastSerialPrintMs = 0;
unsigned long candidateGestureSinceMs = 0;
unsigned long lastGestureAudioMs = 0;
unsigned long lastTestFingerSwitchMs = 0;
unsigned long lastSensorHealthWarningMs = 0;

int candidateGestureIndex = GESTURE_UNKNOWN;
int stableGestureIndex = GESTURE_UNKNOWN;
int lastAudioGestureIndex = GESTURE_UNKNOWN;
byte testFingerIndex = 0;

// ---------------------------------------------------------------------------
// Bluetooth audio generation state
// ---------------------------------------------------------------------------

BluetoothA2DPSource a2dpSource;

const int AUDIO_SAMPLE_RATE = 44100;
const int16_t AUDIO_AMPLITUDE = 9500;
const float TWO_PI_FLOAT = 6.28318530718;

volatile uint16_t activeToneFrequencyHz = 0;

struct AudioStep {
  uint16_t frequencyHz;
  uint16_t durationMs;
};

const byte MAX_AUDIO_STEPS = 28;
AudioStep audioSteps[MAX_AUDIO_STEPS];
byte audioStepCount = 0;
byte audioStepIndex = 0;
bool audioPatternActive = false;
unsigned long audioStepStartedMs = 0;

// ---------------------------------------------------------------------------
// Function declarations
// ---------------------------------------------------------------------------

void setupPins();
void runStartupCalibration();
void sampleCalibrationPose(const char *label, int outputValues[]);
void printCalibrationArrays();
void setupBluetoothAudio();
bool isSpeakerNameConfigured();
int32_t getAudioChannels(Channels *channels, int32_t channelCount);
void readAllFingerSensors();
int normalizeBendPercent(float value, int straightValue, int bentValue);
FingerState classifyFinger(int percent);
void updateGestureDetection(int detectedGestureIndex);
int detectGesture();
bool gestureMatches(const GestureRule &rule);
bool stateMatches(FingerState actual, FingerMatch expected);
void triggerGestureAudio(int gestureIndex);
void clearAudioPattern();
bool appendAudioStep(uint16_t frequencyHz, uint16_t durationMs);
void startAudioPattern();
void updateAudioPattern();
void setActiveTone(uint16_t frequencyHz);
void playMorseLetter(char letter);
const char *morseForLetter(char letter);
void playMelody(MelodyId melody);
void printStatus(int detectedGestureIndex);
void printSensorTest();
void printSensorHealthWarnings();
const char *fingerStateName(FingerState state);
const char *fingerStateShort(FingerState state);
const char *gestureName(int gestureIndex);

// ---------------------------------------------------------------------------
// Arduino setup and loop
// ---------------------------------------------------------------------------

void setup() {
  Serial.begin(115200);
  delay(500);

  Serial.println();
  Serial.println("ESP32 Smart Glove starting...");
  Serial.println("Reading 5 flex sensors and generating Bluetooth audio events.");
  Serial.println("Serial baud: 115200");

  setupPins();

  if (RUN_STARTUP_CALIBRATION) {
    runStartupCalibration();
  }

  setupBluetoothAudio();

  Serial.println("System ready.");
  Serial.println("Raw values, bend percentages, states, and gesture names will print continuously.");
  Serial.println();
}

void loop() {
  const unsigned long nowMs = millis();

  if (nowMs - lastSensorReadMs >= SENSOR_READ_INTERVAL_MS) {
    lastSensorReadMs = nowMs;
    readAllFingerSensors();
    const int detectedGestureIndex = detectGesture();
    updateGestureDetection(detectedGestureIndex);

    if (nowMs - lastSerialPrintMs >= SERIAL_PRINT_INTERVAL_MS) {
      lastSerialPrintMs = nowMs;
      printStatus(detectedGestureIndex);
    }

    if (SENSOR_TEST_MODE) {
      printSensorTest();
    }
  }

  updateAudioPattern();
  delay(2);
}

// ---------------------------------------------------------------------------
// Setup helpers
// ---------------------------------------------------------------------------

void setupPins() {
  analogReadResolution(12);

  for (byte i = 0; i < NUM_FINGERS; i++) {
    pinMode(FINGER_PINS[i], INPUT);
    analogSetPinAttenuation(FINGER_PINS[i], ADC_11db);
  }

  Serial.println("Pin setup complete.");
  Serial.println("Thumb=GPIO32, Index=GPIO33, Middle=GPIO26, Ring=GPIO14, Pinky=GPIO25");
  Serial.println("GPIO14/GPIO25/GPIO26 are ADC2 pins; avoid WiFi while reading these sensors.");
}

void runStartupCalibration() {
  Serial.println();
  Serial.println("STARTUP CALIBRATION ENABLED");
  Serial.println("Hold all fingers STRAIGHT...");
  delay(3000);
  sampleCalibrationPose("straight", straightCalibration);

  Serial.println("Now hold all fingers fully BENT...");
  delay(3000);
  sampleCalibrationPose("bent", bentCalibration);

  printCalibrationArrays();
  Serial.println("Calibration values are active in RAM for this run.");
  Serial.println("Copy the arrays above into the sketch for permanent use.");
  Serial.println();
}

void sampleCalibrationPose(const char *label, int outputValues[]) {
  const int sampleCount = 80;
  long totals[NUM_FINGERS] = {0, 0, 0, 0, 0};

  for (int sample = 0; sample < sampleCount; sample++) {
    for (byte finger = 0; finger < NUM_FINGERS; finger++) {
      totals[finger] += analogRead(FINGER_PINS[finger]);
    }
    delay(20);
  }

  Serial.print("Captured ");
  Serial.print(label);
  Serial.println(" calibration:");

  for (byte finger = 0; finger < NUM_FINGERS; finger++) {
    outputValues[finger] = totals[finger] / sampleCount;
    Serial.print("  ");
    Serial.print(FINGER_NAMES[finger]);
    Serial.print(": ");
    Serial.println(outputValues[finger]);
  }
}

void printCalibrationArrays() {
  Serial.println();
  Serial.print("int straightCalibration[NUM_FINGERS] = {");
  for (byte i = 0; i < NUM_FINGERS; i++) {
    Serial.print(straightCalibration[i]);
    if (i < NUM_FINGERS - 1) {
      Serial.print(", ");
    }
  }
  Serial.println("};");

  Serial.print("int bentCalibration[NUM_FINGERS] = {");
  for (byte i = 0; i < NUM_FINGERS; i++) {
    Serial.print(bentCalibration[i]);
    if (i < NUM_FINGERS - 1) {
      Serial.print(", ");
    }
  }
  Serial.println("};");
  Serial.println();
}

void setupBluetoothAudio() {
  if (!START_BLUETOOTH_ON_BOOT) {
    Serial.println("Bluetooth audio disabled by START_BLUETOOTH_ON_BOOT.");
    return;
  }

  if (!isSpeakerNameConfigured()) {
    Serial.println("Bluetooth speaker name is not set.");
    Serial.println("Set TARGET_BT_SPEAKER_NAME before expecting Bluetooth audio output.");
    return;
  }

  Serial.print("Starting Bluetooth A2DP source. Target speaker: ");
  Serial.println(TARGET_BT_SPEAKER_NAME);

  Serial.println("Speaker volume is controlled on the speaker for this library/core version.");
  Serial.print("Recommended starting speaker volume: ");
  Serial.print(BT_VOLUME_PERCENT_NOTE_ONLY);
  Serial.println("%");

  a2dpSource.start(TARGET_BT_SPEAKER_NAME, getAudioChannels);
}

// ---------------------------------------------------------------------------
// Audio generation
// ---------------------------------------------------------------------------

bool isSpeakerNameConfigured() {
  return strlen(TARGET_BT_SPEAKER_NAME) > 0 &&
         strcmp(TARGET_BT_SPEAKER_NAME, "PUT_SPEAKER_NAME_HERE") != 0;
}

int32_t getAudioChannels(Channels *channels, int32_t channelCount) {
  static float phase = 0.0;
  const uint16_t frequency = activeToneFrequencyHz;
  const float delta = frequency > 0 ? (TWO_PI_FLOAT * frequency) / AUDIO_SAMPLE_RATE : 0.0;

  for (int sample = 0; sample < channelCount; sample++) {
    int16_t value = 0;

    if (frequency > 0) {
      value = (int16_t)(AUDIO_AMPLITUDE * sinf(phase));
      phase += delta;
      if (phase >= TWO_PI_FLOAT) {
        phase -= TWO_PI_FLOAT;
      }
    }

    channels[sample].channel1 = value;
    channels[sample].channel2 = value;
  }

  delay(1);
  return channelCount;
}

void triggerGestureAudio(int gestureIndex) {
  if (gestureIndex == GESTURE_UNKNOWN) {
    return;
  }

  const GestureRule &gesture = GESTURES[gestureIndex];

  Serial.print("Audio event: ");
  Serial.print(gesture.outputText);
  Serial.print(" (");
  Serial.print(gesture.gestureName);
  Serial.println(")");

  if (gesture.audioMode == AUDIO_MORSE_LETTER) {
    playMorseLetter(gesture.letter);
  } else if (gesture.audioMode == AUDIO_MELODY) {
    playMelody(gesture.melody);
  }
}

void clearAudioPattern() {
  audioStepCount = 0;
  audioStepIndex = 0;
  audioPatternActive = false;
  setActiveTone(0);
}

bool appendAudioStep(uint16_t frequencyHz, uint16_t durationMs) {
  if (audioStepCount >= MAX_AUDIO_STEPS) {
    return false;
  }

  audioSteps[audioStepCount].frequencyHz = frequencyHz;
  audioSteps[audioStepCount].durationMs = durationMs;
  audioStepCount++;
  return true;
}

void startAudioPattern() {
  if (audioStepCount == 0) {
    clearAudioPattern();
    return;
  }

  audioPatternActive = true;
  audioStepIndex = 0;
  audioStepStartedMs = millis();
  setActiveTone(audioSteps[0].frequencyHz);
}

void updateAudioPattern() {
  if (!audioPatternActive) {
    return;
  }

  const unsigned long nowMs = millis();

  if (nowMs - audioStepStartedMs < audioSteps[audioStepIndex].durationMs) {
    return;
  }

  audioStepIndex++;
  audioStepStartedMs = nowMs;

  if (audioStepIndex >= audioStepCount) {
    clearAudioPattern();
    return;
  }

  setActiveTone(audioSteps[audioStepIndex].frequencyHz);
}

void setActiveTone(uint16_t frequencyHz) {
  activeToneFrequencyHz = frequencyHz;
}

void playMorseLetter(char letter) {
  const char *morse = morseForLetter(letter);

  clearAudioPattern();

  if (morse == nullptr) {
    playMelody(MELODY_UNKNOWN);
    return;
  }

  const uint16_t toneFrequency = 880;
  const uint16_t dotMs = 120;
  const uint16_t dashMs = dotMs * 3;
  const uint16_t symbolGapMs = dotMs;
  const uint16_t endGapMs = dotMs * 5;

  for (byte i = 0; morse[i] != '\0'; i++) {
    if (morse[i] == '.') {
      appendAudioStep(toneFrequency, dotMs);
    } else if (morse[i] == '-') {
      appendAudioStep(toneFrequency, dashMs);
    }

    appendAudioStep(0, symbolGapMs);
  }

  appendAudioStep(0, endGapMs);
  startAudioPattern();
}

const char *morseForLetter(char letter) {
  switch (letter) {
    case 'A': return ".-";
    case 'B': return "-...";
    case 'C': return "-.-.";
    case 'D': return "-..";
    case 'E': return ".";
    case 'F': return "..-.";
    case 'G': return "--.";
    case 'H': return "....";
    case 'I': return "..";
    case 'J': return ".---";
    case 'K': return "-.-";
    case 'L': return ".-..";
    case 'M': return "--";
    case 'N': return "-.";
    case 'O': return "---";
    case 'P': return ".--.";
    case 'Q': return "--.-";
    case 'R': return ".-.";
    case 'S': return "...";
    case 'T': return "-";
    case 'U': return "..-";
    case 'V': return "...-";
    case 'W': return ".--";
    case 'X': return "-..-";
    case 'Y': return "-.--";
    case 'Z': return "--..";
    default: return nullptr;
  }
}

void playMelody(MelodyId melody) {
  clearAudioPattern();

  switch (melody) {
    case MELODY_HELLO:
      appendAudioStep(660, 140);
      appendAudioStep(0, 60);
      appendAudioStep(880, 140);
      appendAudioStep(0, 60);
      appendAudioStep(990, 220);
      break;

    case MELODY_FIST:
      appendAudioStep(260, 180);
      appendAudioStep(0, 90);
      appendAudioStep(260, 180);
      appendAudioStep(0, 90);
      appendAudioStep(260, 260);
      break;

    case MELODY_ILOVEYOU:
      appendAudioStep(523, 130);
      appendAudioStep(0, 50);
      appendAudioStep(659, 130);
      appendAudioStep(0, 50);
      appendAudioStep(784, 130);
      appendAudioStep(0, 50);
      appendAudioStep(1047, 260);
      break;

    case MELODY_POINT:
      appendAudioStep(1047, 90);
      appendAudioStep(0, 70);
      appendAudioStep(1047, 90);
      break;

    default:
      appendAudioStep(180, 80);
      appendAudioStep(0, 60);
      appendAudioStep(180, 80);
      break;
  }

  appendAudioStep(0, 300);
  startAudioPattern();
}

// ---------------------------------------------------------------------------
// Sensor reading and classification
// ---------------------------------------------------------------------------

void readAllFingerSensors() {
  for (byte finger = 0; finger < NUM_FINGERS; finger++) {
    rawValues[finger] = analogRead(FINGER_PINS[finger]);

    if (!smoothingReady) {
      smoothedValues[finger] = rawValues[finger];
    } else {
      smoothedValues[finger] =
        (SMOOTHING_ALPHA * rawValues[finger]) +
        ((1.0 - SMOOTHING_ALPHA) * smoothedValues[finger]);
    }

    bendPercent[finger] = normalizeBendPercent(
      smoothedValues[finger],
      straightCalibration[finger],
      bentCalibration[finger]
    );

    fingerStates[finger] = classifyFinger(bendPercent[finger]);
  }

  smoothingReady = true;
}

int normalizeBendPercent(float value, int straightValue, int bentValue) {
  const float span = bentValue - straightValue;

  if (fabsf(span) < 10.0) {
    return 0;
  }

  const float percent = ((value - straightValue) * 100.0) / span;
  return constrain((int)(percent + 0.5), 0, 100);
}

FingerState classifyFinger(int percent) {
  if (percent <= STRAIGHT_MAX_PERCENT) {
    return STATE_STRAIGHT;
  }

  if (percent >= BENT_MIN_PERCENT) {
    return STATE_BENT;
  }

  return STATE_HALF_BENT;
}

// ---------------------------------------------------------------------------
// Gesture detection
// ---------------------------------------------------------------------------

void updateGestureDetection(int detectedGestureIndex) {
  const unsigned long nowMs = millis();

  if (detectedGestureIndex != candidateGestureIndex) {
    candidateGestureIndex = detectedGestureIndex;
    candidateGestureSinceMs = nowMs;
    return;
  }

  if (stableGestureIndex != candidateGestureIndex &&
      nowMs - candidateGestureSinceMs >= GESTURE_STABLE_MS) {
    stableGestureIndex = candidateGestureIndex;
    lastAudioGestureIndex = stableGestureIndex;
    lastGestureAudioMs = nowMs;
    triggerGestureAudio(stableGestureIndex);
    return;
  }

  if (stableGestureIndex != GESTURE_UNKNOWN &&
      stableGestureIndex == candidateGestureIndex &&
      stableGestureIndex == lastAudioGestureIndex &&
      nowMs - lastGestureAudioMs >= REPEAT_SAME_GESTURE_AUDIO_MS) {
    lastGestureAudioMs = nowMs;
    triggerGestureAudio(stableGestureIndex);
  }
}

int detectGesture() {
  for (int i = 0; i < NUM_GESTURES; i++) {
    if (gestureMatches(GESTURES[i])) {
      return i;
    }
  }

  return GESTURE_UNKNOWN;
}

bool gestureMatches(const GestureRule &rule) {
  for (byte finger = 0; finger < NUM_FINGERS; finger++) {
    if (!stateMatches(fingerStates[finger], rule.match[finger])) {
      return false;
    }
  }

  return true;
}

bool stateMatches(FingerState actual, FingerMatch expected) {
  switch (expected) {
    case MATCH_STRAIGHT:
      return actual == STATE_STRAIGHT;
    case MATCH_HALF_BENT:
      return actual == STATE_HALF_BENT;
    case MATCH_BENT:
      return actual == STATE_BENT;
    case MATCH_STRAIGHT_OR_HALF:
      return actual == STATE_STRAIGHT || actual == STATE_HALF_BENT;
    case MATCH_HALF_OR_BENT:
      return actual == STATE_HALF_BENT || actual == STATE_BENT;
    case MATCH_ANY:
      return true;
    default:
      return false;
  }
}

// ---------------------------------------------------------------------------
// Serial output and testing mode
// ---------------------------------------------------------------------------

void printStatus(int detectedGestureIndex) {
  Serial.print("Raw T/I/M/R/P: ");
  for (byte i = 0; i < NUM_FINGERS; i++) {
    Serial.print(rawValues[i]);
    if (i < NUM_FINGERS - 1) {
      Serial.print(", ");
    }
  }

  Serial.print(" | Bend%: ");
  for (byte i = 0; i < NUM_FINGERS; i++) {
    Serial.print(bendPercent[i]);
    if (i < NUM_FINGERS - 1) {
      Serial.print(", ");
    }
  }

  Serial.print(" | State: ");
  for (byte i = 0; i < NUM_FINGERS; i++) {
    Serial.print(fingerStateShort(fingerStates[i]));
    if (i < NUM_FINGERS - 1) {
      Serial.print("/");
    }
  }

  Serial.print(" | Detecting: ");
  Serial.print(gestureName(detectedGestureIndex));
  Serial.print(" | Stable: ");
  Serial.print(gestureName(stableGestureIndex));

  if (stableGestureIndex != GESTURE_UNKNOWN) {
    Serial.print(" | Output: ");
    Serial.print(GESTURES[stableGestureIndex].outputText);
  }

  Serial.println();
  printSensorHealthWarnings();
}

void printSensorTest() {
  const unsigned long nowMs = millis();

  if (nowMs - lastTestFingerSwitchMs < TEST_FINGER_INTERVAL_MS) {
    return;
  }

  lastTestFingerSwitchMs = nowMs;
  testFingerIndex = (testFingerIndex + 1) % NUM_FINGERS;

  Serial.println();
  Serial.print("TEST FINGER: ");
  Serial.print(FINGER_NAMES[testFingerIndex]);
  Serial.print(" on GPIO ");
  Serial.println(FINGER_PINS[testFingerIndex]);
  Serial.print("Raw=");
  Serial.print(rawValues[testFingerIndex]);
  Serial.print(" Smoothed=");
  Serial.print(smoothedValues[testFingerIndex], 1);
  Serial.print(" Bend%=");
  Serial.print(bendPercent[testFingerIndex]);
  Serial.print(" State=");
  Serial.println(fingerStateName(fingerStates[testFingerIndex]));
  Serial.println("Bend only this finger and confirm the raw value changes clearly.");
  Serial.println();
}

void printSensorHealthWarnings() {
  const unsigned long nowMs = millis();

  if (nowMs - lastSensorHealthWarningMs < SENSOR_HEALTH_WARNING_INTERVAL_MS) {
    return;
  }

  lastSensorHealthWarningMs = nowMs;

  for (byte finger = 0; finger < NUM_FINGERS; finger++) {
    if (rawValues[finger] <= RAW_LOW_WARNING || rawValues[finger] >= RAW_HIGH_WARNING) {
      Serial.print("Sensor warning: ");
      Serial.print(FINGER_NAMES[finger]);
      Serial.print(" on GPIO ");
      Serial.print(FINGER_PINS[finger]);
      Serial.print(" is reading near the ADC rail: ");
      Serial.print(rawValues[finger]);
      Serial.println(". Check 3.3V, GND, divider resistor, and analog signal wire.");
    }
  }
}

const char *fingerStateName(FingerState state) {
  switch (state) {
    case STATE_STRAIGHT:
      return "straight";
    case STATE_HALF_BENT:
      return "half-bent";
    case STATE_BENT:
      return "bent";
    default:
      return "unknown";
  }
}

const char *fingerStateShort(FingerState state) {
  switch (state) {
    case STATE_STRAIGHT:
      return "S";
    case STATE_HALF_BENT:
      return "H";
    case STATE_BENT:
      return "B";
    default:
      return "?";
  }
}

const char *gestureName(int gestureIndex) {
  if (gestureIndex == GESTURE_UNKNOWN) {
    return "UNKNOWN";
  }

  return GESTURES[gestureIndex].gestureName;
}
