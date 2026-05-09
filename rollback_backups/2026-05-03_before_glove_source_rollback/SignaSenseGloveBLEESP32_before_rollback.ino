/*
  SignaSense ESP32 Smart Glove - BLE app firmware

  Why this version exists:
  - The requested Wi-Fi pin map was:
      Pinky GPIO34, Ring GPIO35, Middle GPIO32, Index GPIO33, Thumb GPIO25.
  - A no-Wi-Fi diagnostic on this connected board showed GPIO32, GPIO33,
    GPIO34, GPIO35, and GPIO25 all reading 0.
  - The same diagnostic showed live analog voltages on GPIO26, GPIO27, GPIO14,
    GPIO13, and GPIO12.
  - Those live pins are ADC2 pins. Classic ESP32 cannot read ADC2 reliably while
    Wi-Fi is active, so this build uses BLE instead of Wi-Fi.

  BLE device name:
  - SignaSenseGlove

  SignaSense app:
  - Opens the Smart Glove BLE screen.
  - Scans for SignaSenseGlove.
  - Receives live JSON notifications with raw readings, bend percentages,
    detected letter, accepted word, caption text, and committed-word events.

  Sign language accuracy note:
  - Five flex sensors measure finger bend only. They do not measure palm
    direction, finger contact, hand movement, or the difference between some
    manual alphabet signs. For that reason this firmware uses a conservative
    one-hand ASL-style alphabet subset and returns UNKNOWN when bends are
    ambiguous. It does not auto-correct or guess missing letters.
  - Workflow for clean words:
      1. Hold a letter shape until it is stable.
      2. Open the hand briefly to accept that letter into the word.
      3. Press Commit Word in the app when the full word is complete.
      4. The phone speaks only that committed full word.

  Final pin selection:
  - This build is forced to the requested map only:
      Thumb 25, Index 33, Middle 32, Ring 35, Pinky 34

  Flex sensor wiring:
  - One end of each flex sensor to 3.3V.
  - Other end to the analog GPIO and to one resistor.
  - Other end of the resistor to GND.
  - All grounds common with ESP32 GND.
*/

#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>

// ---------------------------------------------------------------------------
// BLE settings.
// ---------------------------------------------------------------------------

const char BLE_DEVICE_NAME[] = "SignaSenseGlove";
const char SERVICE_UUID[] = "8b77a001-7d7c-4f47-b3d5-0f4d4902c001";
const char DATA_CHAR_UUID[] = "8b77a002-7d7c-4f47-b3d5-0f4d4902c001";

BLEServer *bleServer = nullptr;
BLECharacteristic *dataCharacteristic = nullptr;
bool bleClientConnected = false;
bool oldBleClientConnected = false;

// ---------------------------------------------------------------------------
// Finger order and pin maps.
// ---------------------------------------------------------------------------

enum FingerIndex : byte {
  FINGER_THUMB = 0,
  FINGER_INDEX = 1,
  FINGER_MIDDLE = 2,
  FINGER_RING = 3,
  FINGER_PINKY = 4,
  NUM_FINGERS = 5
};

const char *FINGER_NAMES[NUM_FINGERS] = {
  "Thumb", "Index", "Middle", "Ring", "Pinky"
};

const int REQUESTED_PINS[NUM_FINGERS] = {
  25, // Thumb
  33, // Index
  32, // Middle
  35, // Ring
  34  // Pinky
};

int fingerPins[NUM_FINGERS] = {25, 33, 32, 35, 34};
bool usingFallbackMap = false;

// ---------------------------------------------------------------------------
// Sensor and recognition tuning.
// ---------------------------------------------------------------------------

const unsigned long SENSOR_INTERVAL_MS = 25;
const unsigned long BLE_NOTIFY_INTERVAL_MS = 250;
const unsigned long SERIAL_INTERVAL_MS = 700;
const unsigned long LETTER_STABLE_MS = 1500;
const unsigned long LETTER_RELEASE_MS = 650;
const unsigned long PENDING_LETTER_TIMEOUT_MS = 2500;
const unsigned long CALIBRATION_OPEN_HAND_MS = 2200;

const int RAW_LOW_NO_SIGNAL = 2;
const int RAW_HIGH_NO_SIGNAL = 4093;
const int STRAIGHT_MAX_PERCENT = 28;
const int BENT_MIN_PERCENT = 68;
const int MIN_REQUIRED_ACTIVE_FINGERS = 5;
const byte MAX_WORD_LENGTH = 32;
const byte MAX_SENTENCE_LENGTH = 150;
const float SMOOTHING_ALPHA = 0.20f;
const float MIN_USEFUL_BEND_DELTA = 55.0f;
const float DEFAULT_FULL_BEND_DELTA = 850.0f;

enum FingerState : byte {
  STATE_NO_SIGNAL,
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

struct LetterRule {
  const char *name;
  const char *caption;
  FingerMatch match[NUM_FINGERS];
};

const LetterRule LETTER_RULES[] = {
  {"LETTER_A", "A", {MATCH_STRAIGHT_OR_HALF, MATCH_BENT, MATCH_BENT, MATCH_BENT, MATCH_BENT}},
  {"LETTER_B", "B", {MATCH_HALF_OR_BENT, MATCH_STRAIGHT, MATCH_STRAIGHT, MATCH_STRAIGHT, MATCH_STRAIGHT}},
  {"LETTER_C", "C", {MATCH_HALF_BENT, MATCH_HALF_BENT, MATCH_HALF_BENT, MATCH_HALF_BENT, MATCH_HALF_BENT}},
  {"LETTER_D", "D", {MATCH_HALF_OR_BENT, MATCH_STRAIGHT_OR_HALF, MATCH_HALF_OR_BENT, MATCH_HALF_OR_BENT, MATCH_HALF_OR_BENT}},
  {"LETTER_E", "E", {MATCH_BENT, MATCH_BENT, MATCH_BENT, MATCH_BENT, MATCH_BENT}},
  {"LETTER_F", "F", {MATCH_HALF_OR_BENT, MATCH_HALF_OR_BENT, MATCH_STRAIGHT_OR_HALF, MATCH_STRAIGHT_OR_HALF, MATCH_STRAIGHT_OR_HALF}},
  {"LETTER_I", "I", {MATCH_HALF_OR_BENT, MATCH_HALF_OR_BENT, MATCH_HALF_OR_BENT, MATCH_HALF_OR_BENT, MATCH_STRAIGHT_OR_HALF}},
  {"LETTER_L", "L", {MATCH_STRAIGHT_OR_HALF, MATCH_STRAIGHT_OR_HALF, MATCH_HALF_OR_BENT, MATCH_HALF_OR_BENT, MATCH_HALF_OR_BENT}},
  {"LETTER_V", "V", {MATCH_HALF_OR_BENT, MATCH_STRAIGHT_OR_HALF, MATCH_STRAIGHT_OR_HALF, MATCH_HALF_OR_BENT, MATCH_HALF_OR_BENT}},
  {"LETTER_W", "W", {MATCH_HALF_OR_BENT, MATCH_STRAIGHT_OR_HALF, MATCH_STRAIGHT_OR_HALF, MATCH_STRAIGHT_OR_HALF, MATCH_HALF_OR_BENT}},
  {"LETTER_X", "X", {MATCH_HALF_OR_BENT, MATCH_HALF_BENT, MATCH_HALF_OR_BENT, MATCH_HALF_OR_BENT, MATCH_HALF_OR_BENT}},
  {"LETTER_Y", "Y", {MATCH_STRAIGHT_OR_HALF, MATCH_BENT, MATCH_BENT, MATCH_BENT, MATCH_STRAIGHT_OR_HALF}}
};

const int NUM_LETTERS = sizeof(LETTER_RULES) / sizeof(LETTER_RULES[0]);
const int LETTER_UNKNOWN = -1;

int rawValues[NUM_FINGERS] = {0, 0, 0, 0, 0};
float smoothedValues[NUM_FINGERS] = {0, 0, 0, 0, 0};
float baselineValues[NUM_FINGERS] = {0, 0, 0, 0, 0};
float maxObservedDelta[NUM_FINGERS] = {0, 0, 0, 0, 0};
int bendPercent[NUM_FINGERS] = {0, 0, 0, 0, 0};
FingerState fingerStates[NUM_FINGERS] = {
  STATE_NO_SIGNAL, STATE_NO_SIGNAL, STATE_NO_SIGNAL, STATE_NO_SIGNAL, STATE_NO_SIGNAL
};
bool sensorHasSignal[NUM_FINGERS] = {false, false, false, false, false};
bool baselineReady[NUM_FINGERS] = {false, false, false, false, false};
bool smoothingReady = false;
bool recognitionEnabled = false;
bool calibrationInProgress = false;

unsigned long lastSensorReadMs = 0;
unsigned long lastNotifyMs = 0;
unsigned long lastSerialMs = 0;
unsigned long calibrationStartedMs = 0;
int currentLetterIndex = LETTER_UNKNOWN;
int candidateLetterIndex = LETTER_UNKNOWN;
int stableLetterIndex = LETTER_UNKNOWN;
int pendingLetterIndex = LETTER_UNKNOWN;
unsigned long candidateLetterSinceMs = 0;
unsigned long pendingLetterReadyMs = 0;
unsigned long neutralSinceMs = 0;
String currentWord = "";
String currentSentence = "";
String lastCommittedWord = "";
unsigned long wordCommitCounter = 0;
char lastAcceptedLetter = '\0';
unsigned long acceptedLetterCounter = 0;
size_t lastBleJsonLength = 0;

void handleCommand(const String &command);

class ServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer *server) {
    bleClientConnected = true;
  }

  void onDisconnect(BLEServer *server) {
    bleClientConnected = false;
  }
};

class DataCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *characteristic) {
    std::string value = characteristic->getValue();
    if (value.length() == 0) {
      return;
    }

    String command = String(value.c_str());
    command.trim();
    handleCommand(command);
  }
};

void setup() {
  Serial.begin(115200);
  delay(500);

  analogReadResolution(12);
  choosePinMap();
  setupSensors();
  resetCalibration();
  setupBle();

  Serial.println();
  Serial.println("SignaSense ESP32 Smart Glove BLE ready.");
  Serial.print("BLE name: ");
  Serial.println(BLE_DEVICE_NAME);
  Serial.print("Pin map in use: ");
  Serial.println(pinMapText());
  Serial.println("Using final requested pins only.");
}

void loop() {
  const unsigned long nowMs = millis();

  if (nowMs - lastSensorReadMs >= SENSOR_INTERVAL_MS) {
    lastSensorReadMs = nowMs;
    readSensors();
    updateCalibrationState(nowMs);

    if (recognitionEnabled) {
      currentLetterIndex = detectLetter();
      updateWordBuilder(currentLetterIndex, nowMs);
    } else {
      clearLetterDetectionState(nowMs);
    }
  }

  if (bleClientConnected && nowMs - lastNotifyMs >= BLE_NOTIFY_INTERVAL_MS) {
    lastNotifyMs = nowMs;
    notifyStatus();
  }

  if (nowMs - lastSerialMs >= SERIAL_INTERVAL_MS) {
    lastSerialMs = nowMs;
    printSerialStatus();
  }

  if (!bleClientConnected && oldBleClientConnected) {
    delay(500);
    bleServer->startAdvertising();
    oldBleClientConnected = bleClientConnected;
  }

  if (bleClientConnected && !oldBleClientConnected) {
    oldBleClientConnected = bleClientConnected;
  }

  delay(2);
}

void setupBle() {
  BLEDevice::init(BLE_DEVICE_NAME);
  BLEDevice::setMTU(517);

  bleServer = BLEDevice::createServer();
  bleServer->setCallbacks(new ServerCallbacks());

  BLEService *service = bleServer->createService(SERVICE_UUID);
  dataCharacteristic = service->createCharacteristic(
    DATA_CHAR_UUID,
    BLECharacteristic::PROPERTY_READ |
    BLECharacteristic::PROPERTY_WRITE |
    BLECharacteristic::PROPERTY_NOTIFY
  );

  dataCharacteristic->setCallbacks(new DataCallbacks());
  dataCharacteristic->addDescriptor(new BLE2902());
  const String initialStatus = buildStatusJson();
  dataCharacteristic->setValue((uint8_t *)initialStatus.c_str(), initialStatus.length());

  service->start();

  BLEAdvertising *advertising = BLEDevice::getAdvertising();
  advertising->addServiceUUID(SERVICE_UUID);
  advertising->setScanResponse(true);
  advertising->setMinPreferred(0x06);
  advertising->setMinPreferred(0x12);
  BLEDevice::startAdvertising();
}

void setupSensors() {
  for (byte i = 0; i < NUM_FINGERS; i++) {
    pinMode(fingerPins[i], INPUT);
    analogSetPinAttenuation(fingerPins[i], ADC_11db);
  }
}

void choosePinMap() {
  for (byte i = 0; i < NUM_FINGERS; i++) {
    pinMode(REQUESTED_PINS[i], INPUT);
    analogSetPinAttenuation(REQUESTED_PINS[i], ADC_11db);
    fingerPins[i] = REQUESTED_PINS[i];
  }
  usingFallbackMap = false;
}

byte scorePinMap(const int pins[]) {
  byte score = 0;

  for (byte i = 0; i < NUM_FINGERS; i++) {
    long total = 0;
    for (byte sample = 0; sample < 10; sample++) {
      total += analogRead(pins[i]);
      delay(2);
    }
    const int average = total / 10;
    if (rawHasSignal(average)) {
      score++;
    }
  }

  return score;
}

void readSensors() {
  const bool calibrationCanCapture =
    calibrationInProgress &&
    millis() - calibrationStartedMs >= CALIBRATION_OPEN_HAND_MS;

  for (byte finger = 0; finger < NUM_FINGERS; finger++) {
    rawValues[finger] = analogRead(fingerPins[finger]);
    sensorHasSignal[finger] = rawHasSignal(rawValues[finger]);

    if (!sensorHasSignal[finger]) {
      bendPercent[finger] = 0;
      fingerStates[finger] = STATE_NO_SIGNAL;
      continue;
    }

    if (!recognitionEnabled && !calibrationCanCapture) {
      smoothedValues[finger] = rawValues[finger];
      bendPercent[finger] = 0;
      fingerStates[finger] = STATE_NO_SIGNAL;
      continue;
    }

    if (!smoothingReady || !baselineReady[finger]) {
      smoothedValues[finger] = rawValues[finger];
    } else {
      smoothedValues[finger] =
        (SMOOTHING_ALPHA * rawValues[finger]) +
        ((1.0f - SMOOTHING_ALPHA) * smoothedValues[finger]);
    }

    updateDynamicCalibration(finger);
    bendPercent[finger] = calculateBendPercent(finger);
    fingerStates[finger] = classifyFinger(bendPercent[finger]);
  }

  smoothingReady = true;
}

void updateCalibrationState(unsigned long nowMs) {
  if (!calibrationInProgress) {
    return;
  }

  if (nowMs - calibrationStartedMs < CALIBRATION_OPEN_HAND_MS) {
    return;
  }

  if (activeSensorCount() < MIN_REQUIRED_ACTIVE_FINGERS || !allBaselinesReady()) {
    return;
  }

  calibrationInProgress = false;
  recognitionEnabled = true;
  clearLetterDetectionState(nowMs);
}

bool allBaselinesReady() {
  for (byte i = 0; i < NUM_FINGERS; i++) {
    if (!baselineReady[i]) {
      return false;
    }
  }
  return true;
}

bool rawHasSignal(int rawValue) {
  return rawValue > RAW_LOW_NO_SIGNAL && rawValue < RAW_HIGH_NO_SIGNAL;
}

void updateDynamicCalibration(byte finger) {
  if (!baselineReady[finger]) {
    baselineValues[finger] = smoothedValues[finger];
    maxObservedDelta[finger] = MIN_USEFUL_BEND_DELTA;
    baselineReady[finger] = true;
    return;
  }

  const float delta = fabsf(smoothedValues[finger] - baselineValues[finger]);
  if (delta > maxObservedDelta[finger]) {
    maxObservedDelta[finger] = delta;
  }
}

int calculateBendPercent(byte finger) {
  if (!baselineReady[finger]) {
    return 0;
  }

  const float delta = fabsf(smoothedValues[finger] - baselineValues[finger]);
  const float scale = max(DEFAULT_FULL_BEND_DELTA, maxObservedDelta[finger]);
  return constrain((int)((delta * 100.0f / scale) + 0.5f), 0, 100);
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

int detectLetter() {
  int bestIndex = LETTER_UNKNOWN;
  int bestScore = -1;

  for (int i = 0; i < NUM_LETTERS; i++) {
    int considered = 0;
    int score = 0;

    for (byte finger = 0; finger < NUM_FINGERS; finger++) {
      if (!sensorHasSignal[finger]) {
        continue;
      }

      considered++;
      if (stateMatches(fingerStates[finger], LETTER_RULES[i].match[finger])) {
        score++;
      }
    }

    if (considered < MIN_REQUIRED_ACTIVE_FINGERS || score != considered) {
      continue;
    }

    if (score > bestScore) {
      bestScore = score;
      bestIndex = i;
    }
  }

  return bestIndex;
}

bool stateMatches(FingerState actual, FingerMatch expected) {
  if (actual == STATE_NO_SIGNAL) {
    return false;
  }

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

void clearLetterDetectionState(unsigned long nowMs) {
  currentLetterIndex = LETTER_UNKNOWN;
  candidateLetterIndex = LETTER_UNKNOWN;
  stableLetterIndex = LETTER_UNKNOWN;
  pendingLetterIndex = LETTER_UNKNOWN;
  candidateLetterSinceMs = nowMs;
  pendingLetterReadyMs = 0;
  neutralSinceMs = 0;
}

void updateWordBuilder(int detectedLetterIndex, unsigned long nowMs) {
  if (handIsOpenNeutral()) {
    if (neutralSinceMs == 0) {
      neutralSinceMs = nowMs;
    }

    if (pendingLetterIndex != LETTER_UNKNOWN && nowMs - neutralSinceMs >= LETTER_RELEASE_MS) {
      appendLetterToWord(pendingLetterIndex);
      pendingLetterIndex = LETTER_UNKNOWN;
      pendingLetterReadyMs = 0;
      candidateLetterIndex = LETTER_UNKNOWN;
      stableLetterIndex = LETTER_UNKNOWN;
      candidateLetterSinceMs = nowMs;
    }

    return;
  }

  neutralSinceMs = 0;

  if (detectedLetterIndex == LETTER_UNKNOWN) {
    if (pendingLetterIndex != LETTER_UNKNOWN &&
        nowMs - pendingLetterReadyMs > PENDING_LETTER_TIMEOUT_MS) {
      pendingLetterIndex = LETTER_UNKNOWN;
    }

    candidateLetterIndex = LETTER_UNKNOWN;
    stableLetterIndex = LETTER_UNKNOWN;
    candidateLetterSinceMs = nowMs;
    return;
  }

  if (detectedLetterIndex != candidateLetterIndex) {
    candidateLetterIndex = detectedLetterIndex;
    candidateLetterSinceMs = nowMs;
    return;
  }

  if (nowMs - candidateLetterSinceMs < LETTER_STABLE_MS) {
    return;
  }

  stableLetterIndex = detectedLetterIndex;
  pendingLetterIndex = detectedLetterIndex;
  pendingLetterReadyMs = nowMs;
}

bool handIsOpenNeutral() {
  if (activeSensorCount() < MIN_REQUIRED_ACTIVE_FINGERS) {
    return false;
  }

  for (byte finger = 0; finger < NUM_FINGERS; finger++) {
    if (fingerStates[finger] != STATE_STRAIGHT) {
      return false;
    }
  }

  return true;
}

void appendLetterToWord(int letterIndex) {
  if (letterIndex == LETTER_UNKNOWN) {
    return;
  }

  const char *letter = captionText(letterIndex);
  if (strlen(letter) != 1) {
    return;
  }

  if (currentWord.length() >= MAX_WORD_LENGTH) {
    currentWord.remove(0, currentWord.length() - MAX_WORD_LENGTH + 1);
  }

  currentWord += letter[0];
  lastAcceptedLetter = letter[0];
  acceptedLetterCounter++;
}

void handleCommand(const String &command) {
  if (command.startsWith("setword:")) {
    setCurrentWordFromText(command.substring(8));
  } else if (command.startsWith("commitword:")) {
    setCurrentWordFromText(command.substring(11));
    commitCurrentWordToSentence();
  } else if (command == "clear") {
    clearCurrentWord();
  } else if (command == "space" || command == "commitword") {
    commitCurrentWordToSentence();
  } else if (command == "backspace") {
    if (currentWord.length() > 0) {
      currentWord.remove(currentWord.length() - 1);
    }
  } else if (command == "clearsentence") {
    currentSentence = "";
  } else if (command == "recalibrate") {
    startOpenHandCalibration();
  }
}

void startOpenHandCalibration() {
  resetCalibration();
  recognitionEnabled = false;
  calibrationInProgress = true;
  calibrationStartedMs = millis();
}

void setCurrentWordFromText(String text) {
  text.trim();
  text.toUpperCase();

  currentWord = "";
  for (unsigned int i = 0; i < text.length() && currentWord.length() < MAX_WORD_LENGTH; i++) {
    const char c = text.charAt(i);
    if (c >= 'A' && c <= 'Z') {
      currentWord += c;
    }
  }

  candidateLetterIndex = LETTER_UNKNOWN;
  stableLetterIndex = LETTER_UNKNOWN;
  pendingLetterIndex = LETTER_UNKNOWN;
  pendingLetterReadyMs = 0;
}

void commitCurrentWordToSentence() {
  if (currentWord.length() == 0) {
    return;
  }

  lastCommittedWord = currentWord;
  wordCommitCounter++;

  if (currentSentence.length() > 0) {
    currentSentence += ' ';
  }

  currentSentence += currentWord;
  trimSentenceText();
  currentWord = "";
  candidateLetterIndex = LETTER_UNKNOWN;
  stableLetterIndex = LETTER_UNKNOWN;
  pendingLetterIndex = LETTER_UNKNOWN;
  pendingLetterReadyMs = 0;
}

void trimSentenceText() {
  if (currentSentence.length() <= MAX_SENTENCE_LENGTH) {
    return;
  }

  currentSentence.remove(0, currentSentence.length() - MAX_SENTENCE_LENGTH);
  const int firstSpace = currentSentence.indexOf(' ');
  if (firstSpace > 0) {
    currentSentence.remove(0, firstSpace + 1);
  }
}

void clearCurrentWord() {
  currentWord = "";
  candidateLetterIndex = LETTER_UNKNOWN;
  stableLetterIndex = LETTER_UNKNOWN;
  pendingLetterIndex = LETTER_UNKNOWN;
  pendingLetterReadyMs = 0;
}

void resetCalibration() {
  smoothingReady = false;
  recognitionEnabled = false;
  calibrationInProgress = false;
  currentLetterIndex = LETTER_UNKNOWN;
  candidateLetterIndex = LETTER_UNKNOWN;
  stableLetterIndex = LETTER_UNKNOWN;
  pendingLetterIndex = LETTER_UNKNOWN;
  candidateLetterSinceMs = millis();
  pendingLetterReadyMs = 0;
  neutralSinceMs = 0;
  lastAcceptedLetter = '\0';
  acceptedLetterCounter = 0;

  for (byte i = 0; i < NUM_FINGERS; i++) {
    rawValues[i] = 0;
    smoothedValues[i] = 0.0f;
    baselineValues[i] = 0.0f;
    maxObservedDelta[i] = 0.0f;
    bendPercent[i] = 0;
    fingerStates[i] = STATE_NO_SIGNAL;
    sensorHasSignal[i] = false;
    baselineReady[i] = false;
  }
}

void notifyStatus() {
  const String json = buildStatusJson();
  lastBleJsonLength = json.length();
  dataCharacteristic->setValue((uint8_t *)json.c_str(), json.length());
  dataCharacteristic->notify();
}

String buildStatusJson() {
  String json = "{";
  json += "\"ok\":true";
  json += ",\"map\":\"";
  json += usingFallbackMap ? "fallback" : "requested";
  json += "\"";
  json += ",\"caption\":";
  appendJsonString(json, captionTextOrBlank(currentLetterIndex));
  json += ",\"stableLetter\":";
  appendJsonString(json, captionTextOrBlank(stableLetterIndex));
  json += ",\"pendingLetter\":";
  appendJsonString(json, captionTextOrBlank(pendingLetterIndex));
  json += ",\"acceptedLetter\":";
  if (lastAcceptedLetter == '\0') {
    appendJsonString(json, "");
  } else {
    char accepted[2] = {lastAcceptedLetter, '\0'};
    appendJsonString(json, accepted);
  }
  json += ",\"acceptedLetterCounter\":";
  json += acceptedLetterCounter;
  json += ",\"word\":";
  appendJsonString(json, currentWord);
  json += ",\"committedWord\":";
  appendJsonString(json, lastCommittedWord);
  json += ",\"wordCommitCounter\":";
  json += wordCommitCounter;
  json += ",\"sentence\":";
  appendJsonString(json, currentSentence);
  json += ",\"status\":";
  appendJsonString(json, statusText());
  json += ",\"calibrated\":";
  json += recognitionEnabled ? "true" : "false";
  json += ",\"calibrating\":";
  json += calibrationInProgress ? "true" : "false";
  json += ",\"pins\":";
  appendIntArray(json, fingerPins, NUM_FINGERS);
  json += ",\"raw\":";
  appendIntArray(json, rawValues, NUM_FINGERS);
  json += ",\"bend\":";
  appendIntArray(json, bendPercent, NUM_FINGERS);
  json += ",\"signal\":";
  appendBoolArray(json, sensorHasSignal, NUM_FINGERS);
  json += ",\"state\":[";
  for (byte i = 0; i < NUM_FINGERS; i++) {
    appendJsonString(json, fingerStateText(fingerStates[i]));
    if (i < NUM_FINGERS - 1) {
      json += ",";
    }
  }
  json += "]}";
  return json;
}

void appendIntArray(String &json, const int values[], byte count) {
  json += "[";
  for (byte i = 0; i < count; i++) {
    json += values[i];
    if (i < count - 1) {
      json += ",";
    }
  }
  json += "]";
}

void appendBoolArray(String &json, const bool values[], byte count) {
  json += "[";
  for (byte i = 0; i < count; i++) {
    json += values[i] ? "true" : "false";
    if (i < count - 1) {
      json += ",";
    }
  }
  json += "]";
}

void appendJsonString(String &json, const char *value) {
  json += "\"";
  if (value != nullptr) {
    for (size_t i = 0; value[i] != '\0'; i++) {
      const char c = value[i];
      if (c == '\\' || c == '\"') {
        json += "\\";
        json += c;
      } else if (c != '\r' && c != '\n') {
        json += c;
      }
    }
  }
  json += "\"";
}

void appendJsonString(String &json, const String &value) {
  appendJsonString(json, value.c_str());
}

byte activeSensorCount() {
  byte count = 0;
  for (byte i = 0; i < NUM_FINGERS; i++) {
    if (sensorHasSignal[i]) {
      count++;
    }
  }
  return count;
}

String statusText() {
  const byte active = activeSensorCount();

  if (active == 0) {
    return "No signal";
  }

  if (active < MIN_REQUIRED_ACTIVE_FINGERS) {
    String text = "Only ";
    text += active;
    text += "/5 active";
    return text;
  }

  if (calibrationInProgress) {
    const unsigned long elapsedMs = millis() - calibrationStartedMs;
    if (elapsedMs < CALIBRATION_OPEN_HAND_MS) {
      String text = "Hold hand open ";
      text += (CALIBRATION_OPEN_HAND_MS - elapsedMs + 999) / 1000;
      text += "s";
      return text;
    }
    return "Finishing calibration";
  }

  if (!recognitionEnabled) {
    return "Press Recalibrate with hand open";
  }

  if (pendingLetterIndex != LETTER_UNKNOWN) {
    String text = "Pending letter ";
    text += captionText(pendingLetterIndex);
    text += ", open hand";
    return text;
  }

  if (stableLetterIndex != LETTER_UNKNOWN) {
    String text = "Stable letter ";
    text += captionText(stableLetterIndex);
    text += ", open hand";
    return text;
  }

  if (handIsOpenNeutral()) {
    return "Open hand";
  }

  return "Ready";
}

void printSerialStatus() {
  Serial.print("Map: ");
  Serial.print(usingFallbackMap ? "fallback" : "requested");
  Serial.print(" | Pins T/I/M/R/P: ");
  Serial.print(pinMapText());
  Serial.print(" | Raw: ");
  for (byte i = 0; i < NUM_FINGERS; i++) {
    Serial.print(rawValues[i]);
    if (i < NUM_FINGERS - 1) {
      Serial.print(", ");
    }
  }
  Serial.print(" | State: ");
  for (byte i = 0; i < NUM_FINGERS; i++) {
    Serial.print(fingerStateText(fingerStates[i]));
    if (i < NUM_FINGERS - 1) {
      Serial.print("/");
    }
  }
  Serial.print(" | Bend%: ");
  for (byte i = 0; i < NUM_FINGERS; i++) {
    Serial.print(bendPercent[i]);
    if (i < NUM_FINGERS - 1) {
      Serial.print(", ");
    }
  }
  Serial.print(" | Letter: ");
  Serial.print(captionText(currentLetterIndex));
  Serial.print(" | Cal: ");
  if (recognitionEnabled) {
    Serial.print("ready");
  } else if (calibrationInProgress) {
    Serial.print("calibrating");
  } else {
    Serial.print("locked");
  }
  Serial.print(" | Pending: ");
  Serial.print(captionText(pendingLetterIndex));
  Serial.print(" | Word: ");
  Serial.print(currentWord);
  Serial.print(" | BLE bytes: ");
  Serial.print(lastBleJsonLength);
  Serial.print(" | BLE: ");
  Serial.println(bleClientConnected ? "connected" : "advertising");
}

String pinMapText() {
  String text = "";
  for (byte i = 0; i < NUM_FINGERS; i++) {
    if (i > 0) {
      text += " ";
    }
    text += FINGER_NAMES[i];
    text += "=GPIO";
    text += fingerPins[i];
  }
  return text;
}

const char *fingerStateText(FingerState state) {
  switch (state) {
    case STATE_NO_SIGNAL:
      return "NO";
    case STATE_STRAIGHT:
      return "STRAIGHT";
    case STATE_HALF_BENT:
      return "HALF";
    case STATE_BENT:
      return "BENT";
    default:
      return "?";
  }
}

const char *letterName(int letterIndex) {
  if (letterIndex == LETTER_UNKNOWN) {
    return "UNKNOWN";
  }
  return LETTER_RULES[letterIndex].name;
}

const char *captionText(int letterIndex) {
  if (letterIndex == LETTER_UNKNOWN) {
    return "No recognized letter";
  }
  return LETTER_RULES[letterIndex].caption;
}

const char *captionTextOrBlank(int letterIndex) {
  if (letterIndex == LETTER_UNKNOWN) {
    return "";
  }
  return LETTER_RULES[letterIndex].caption;
}
