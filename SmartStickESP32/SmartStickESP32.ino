/*
  SignaSense ESP32 Smart Stick - Bluetooth speaker voice mode

  Wiring:
  - Ultrasonic TRIG -> ESP32 GPIO 5
  - Ultrasonic ECHO -> ESP32 GPIO 18
  - Buzzer signal   -> ESP32 GPIO 15
  - Ultrasonic VCC  -> sensor supply voltage
  - Ultrasonic GND  -> ESP32 GND
  - Buzzer GND      -> ESP32 GND

  Important:
  If the ultrasonic sensor outputs 5V on ECHO, level-shift it or use a voltage
  divider before connecting it to ESP32 GPIO 18. ESP32 GPIO pins are not 5V
  tolerant.

  Mode:
  - No Wi-Fi.
  - No phone app.
  - ESP32 connects to either "EWA Audio A20" or "SINOBAND Book".
    Whichever accepted speaker is discovered/connects first will be used.
  - ESP32 says "Smart stick ready" after the Bluetooth speaker connects.
  - ESP32 speaks distance and guidance only when an obstacle is inside the
    warning distance. It does not keep repeating clear-path readings.
  - The buzzer is only backup when the Bluetooth speaker is not connected.

  Voice note:
  This is not cloud/live TTS. The male voice clips are embedded in flash and
  assembled into ordered phrases such as:
  "Distance is one point two meters. Obstacle ahead. Stop now."
*/

#include <Arduino.h>
#include <math.h>
#include <string.h>
#include <vector>
#include <Preferences.h>
#include "esp_a2dp_api.h"
#include "esp_gap_bt_api.h"
#include "BluetoothA2DPSource.h"
#include "SpeechClips.h"
#include "LugandaClips.h"

// Pin summary
const int trigPin = 5;
const int echoPin = 18;
const int buzzerPin = 15;

// Bluetooth speaker targets. The stick scans for all names below and connects
// to the first one that accepts the A2DP connection.
const char *ewaSpeakerName = "EWA Audio A20";
const char *sinobandSpeakerName = "SINOBAND Book";
const char *sinobandSpeakerNameAlt1 = "Sinoband Book";
const char *sinobandSpeakerNameAlt2 = "sinoband book";

// Known address fallback for the previous SINOBAND Book speaker only.
// The EWA Audio A20 will be found by Bluetooth name scan unless its address is
// added later after pairing/testing.
const uint8_t sinobandKnownSpeakerAddress[ESP_BD_ADDR_LEN] = {0xF5, 0x4E, 0xFD, 0x0E, 0x05, 0x3D};

// Adjustable distance thresholds in centimeters
const float warningDistanceCm = 150.0;
const float dangerDistanceCm = 100.0;
const float veryCloseDistanceCm = 25.0;

// Distance accuracy settings
// Ultrasonic distance depends slightly on air temperature. Adjust this to your room/outdoor temperature.
// Speed of sound: 331.3 + (0.606 * temperatureC) meters/second.
const float airTemperatureC = 25.0;
const float speedOfSoundCmPerUs = (331.3 + (0.606 * airTemperatureC)) / 10000.0;

// Fine calibration after testing against a ruler/tape measure:
// calibratedDistance = (rawDistance * distanceCalibrationScale) + distanceCalibrationOffsetCm
const float distanceCalibrationScale = 1.0;
const float distanceCalibrationOffsetCm = 0.0;

// Sensor reading settings
const unsigned long sensorReadIntervalMs = 60;
const unsigned long serialPrintIntervalMs = 500;
const unsigned long maxEchoTimeUs = 30000;
const float minValidDistanceCm = 2.0;
const float maxValidDistanceCm = 400.0;
const byte sensorSampleAttempts = 7;
const byte minimumValidSamples = 2;
const float sampleAgreementCm = 22.0;
const byte obstacleSpeechConfirmSamples = 3;
const byte closeScanConfirmSamples = 1;
const byte clearResetConfirmSamples = 6;
const unsigned int retryDelayMs = 4;
const unsigned long lastValidHoldMs = 450;

// Smart path scan settings. With one ultrasonic sensor, the user must sweep the stick left/right.
const float scanTriggerDistanceCm = dangerDistanceCm;
const unsigned long scanPromptDelayMs = 2300;
const unsigned long scanCollectMs = 3600;
const unsigned long scanResultHoldMs = 3500;
const unsigned long scanCooldownMs = 14000;
const float scanClearEnoughCm = 120.0;
const float scanDifferenceNeededCm = 25.0;
const float scanMinimumMoveCm = 65.0;
const float scanSmallAdvantageCm = 5.0;
const byte scanTopSampleCount = 3;

// Bluetooth reconnect and speech timing
const unsigned long connectionPrintIntervalMs = 3000;
const unsigned long addressConnectDelayMs = 12000;
const unsigned long addressConnectRetryMs = 7000;
const unsigned long speechObstacleQuietMs = 3200;
const int speechNearDistanceDeltaCm = 15;
const int speechFarDistanceDeltaCm = 35;

// Backup buzzer settings. The buzzer is used only when the Bluetooth speaker is not connected.
const unsigned long noReadingBeepIntervalMs = 1200;
const unsigned long noReadingBeepOnMs = 60;

// Bluetooth audio stream settings
const uint32_t a2dpSampleRate = 44100;
const uint32_t speechStepQ16 = ((uint64_t)SPEECH_CLIP_SAMPLE_RATE << 16) / a2dpSampleRate;
const uint8_t speechQueueSize = 80;

// Voice clarity filter. It removes low rumble from the male clips before
// sending audio to the Bluetooth speaker.
const int32_t voiceHighPassQ15 = 32113;  // about 0.98 in Q15 fixed point
const int32_t voiceClarityGainQ8 = 282;  // about 1.10 in Q8 fixed point

enum VoiceLanguage {
  VOICE_ENGLISH,
  VOICE_LUGANDA
};

// The stick alternates language on every boot:
// first boot English, next boot Luganda, then English again.
VoiceLanguage activeVoiceLanguage = VOICE_ENGLISH;
Preferences languagePrefs;
const char *languagePrefsNamespace = "signasense";
const char *bootCounterKey = "bootCount";
const int16_t noSpeechClip = -1;
const int16_t lugandaClipBase = 1000;

struct DistanceReading {
  float distanceCm;
  bool valid;
};

struct AlertPattern {
  unsigned long intervalMs;
  unsigned long onTimeMs;
};

enum DistanceBucket {
  BUCKET_INVALID,
  BUCKET_VERY_CLOSE,
  BUCKET_CLOSE,
  BUCKET_NEAR,
  BUCKET_WARNING,
  BUCKET_CAUTION,
  BUCKET_CLEAR
};

enum MovementTrend {
  TREND_UNKNOWN,
  TREND_CLOSING,
  TREND_STABLE,
  TREND_OPENING
};

enum ScanState {
  SCAN_IDLE,
  SCAN_LEFT_PROMPT,
  SCAN_LEFT_COLLECT,
  SCAN_RIGHT_PROMPT,
  SCAN_RIGHT_COLLECT,
  SCAN_RESULT_HOLD
};

class SignaSenseA2DPSource : public BluetoothA2DPSource {
 public:
  bool connectToKnownAddress(const uint8_t address[ESP_BD_ADDR_LEN], const char *name) {
    memcpy(s_peer_bda, address, ESP_BD_ADDR_LEN);
    strncpy((char *)s_peer_bdname, name, ESP_BT_GAP_MAX_BDNAME_LEN);
    s_peer_bdname[ESP_BT_GAP_MAX_BDNAME_LEN] = '\0';

    esp_bt_gap_cancel_discovery();
    const esp_err_t result = esp_a2d_source_connect(s_peer_bda);
    if (result == ESP_OK) {
      // APP_AV_STATE_CONNECTING inside the ESP32-A2DP source implementation.
      s_a2d_state = 4;
      return true;
    }
    return false;
  }

  const char *connectedSpeakerName() const {
    return (const char *)s_peer_bdname;
  }
};

SignaSenseA2DPSource a2dpSource;

unsigned long lastSensorReadMs = 0;
unsigned long lastSerialPrintMs = 0;
unsigned long lastConnectionPrintMs = 0;
unsigned long lastAddressConnectAttemptMs = 0;
unsigned long lastValidReadingMs = 0;
unsigned long lastTrendReferenceMs = 0;
unsigned long lastSpeakerConnectedMs = 0;
unsigned long lastSpeechMs = 0;
unsigned long buzzerCycleStartMs = 0;
unsigned long scanStateStartMs = 0;
unsigned long lastScanCycleMs = 0;

bool backupBuzzerIsOn = false;
bool previousSpeakerConnected = false;
bool hasSpokenStatus = false;
bool hasSpokenNoReading = false;
bool pathClearPending = false;
byte obstacleSpeechConfirmCount = 0;
byte closeScanConfirmCount = 0;
byte clearResetConfirmCount = 0;

int lastSpokenDistanceCm = -10000;
DistanceBucket lastSpokenBucket = BUCKET_INVALID;
float trendReferenceDistanceCm = 0.0;
float leftScanBestCm = 0.0;
float rightScanBestCm = 0.0;
float leftScanTopCm[scanTopSampleCount] = {0.0, 0.0, 0.0};
float rightScanTopCm[scanTopSampleCount] = {0.0, 0.0, 0.0};
byte leftScanTopCount = 0;
byte rightScanTopCount = 0;

MovementTrend currentMovementTrend = TREND_UNKNOWN;
ScanState scanState = SCAN_IDLE;
DistanceReading latestReading = {0.0, false};
DistanceReading alertReading = {0.0, false};

portMUX_TYPE speechQueueMux = portMUX_INITIALIZER_UNLOCKED;
volatile int16_t speechQueue[speechQueueSize];
volatile uint8_t speechQueueHead = 0;
volatile uint8_t speechQueueTail = 0;
volatile bool speechResetRequested = false;
volatile bool speechOutputActive = false;

DistanceReading readDistance();
DistanceReading readSingleDistance();
float calibrateDistanceCm(float rawDistanceCm);
float distanceMeters(float distanceCm);
int wholeMeters(float distanceCm);
float remainingCentimeters(float distanceCm);
void sortDistanceSamples(float samples[], byte sampleCount);
void updateAlertReading(const DistanceReading &reading, unsigned long nowMs);
void updateMovementTrend(const DistanceReading &reading, unsigned long nowMs);
void updateObstacleConfirmations(const DistanceReading &reading);
void updateBluetoothConnection(unsigned long nowMs, bool speakerConnected);
void updateSmartScan(const DistanceReading &reading, unsigned long nowMs, bool speakerConnected);
void updateBluetoothSpeech(const DistanceReading &reading, unsigned long nowMs, bool speakerConnected);
void updateBackupBuzzer(const DistanceReading &reading, unsigned long nowMs, bool speakerConnected);
void setupBluetoothSpeaker();
void configureBootLanguage();
void runStartupBuzzerTest();
void printDistance(const DistanceReading &reading, bool speakerConnected);
void playBackupPattern(const AlertPattern &pattern, unsigned long nowMs);
void startBackupBuzzer();
void stopBackupBuzzer();
DistanceBucket distanceBucketFor(float distanceCm);
const char *bucketName(DistanceBucket bucket);
const char *trendName(MovementTrend trend);
const char *guidanceText(const DistanceReading &reading);
bool shouldSpeakStatus(const DistanceReading &reading, unsigned long nowMs);
bool shouldSpeakPathClear(const DistanceReading &reading, unsigned long nowMs);
void enqueueStatusSpeech(const DistanceReading &reading);
void enqueueDistanceSpeech(float distanceCm);
void enqueueGuidanceSpeech(const DistanceReading &reading);
void enqueueScanResultSpeech();
bool shouldMoveLeftAfterScan();
bool shouldMoveRightAfterScan();
bool hasUsableScanSide();
void enqueueNumberSpeech(int value);
void enqueueNumberUnder100Speech(int value);
void resetScanSamples();
void recordScanSample(float distanceCm, float topSamples[], byte *sampleCount);
float averageTopScanSamples(const float topSamples[], byte sampleCount);
void clearSpeechQueue();
void enqueueSpeechClip(SpeechClipId clipId);
void enqueueLugandaClip(LugandaClipId clipId);
void enqueueLocalizedClip(SpeechClipId englishClip, LugandaClipId lugandaClip);
bool popSpeechClip(int16_t *clipId);
bool resolveSpeechClip(int16_t clipId, const uint8_t **clipData, uint32_t *clipLength);
bool isSpeechBusy();
int32_t getAudioSamples(Channels *channels, int32_t channelLen);
int16_t decodeMuLaw(uint8_t value);
void speakImmediate(SpeechClipId firstClip, SpeechClipId secondClip = SPEECH_CLIP_COUNT);
void startLeftScan(unsigned long nowMs, bool speakerConnected);
void moveToRightScan(unsigned long nowMs, bool speakerConnected);
void finishScan(unsigned long nowMs, bool speakerConnected);
const char *scanStateName(ScanState state);
const char *scanResultText();

void setup() {
  Serial.begin(115200);
  configureBootLanguage();

  pinMode(trigPin, OUTPUT);
  pinMode(echoPin, INPUT);
  pinMode(buzzerPin, OUTPUT);

  digitalWrite(trigPin, LOW);
  stopBackupBuzzer();

  Serial.println();
  Serial.println("SignaSense ESP32 Smart Stick starting...");
  Serial.println("Mode: Bluetooth speaker voice output. Wi-Fi/app link disabled.");
  Serial.println("Bluetooth target speakers:");
  Serial.print("- ");
  Serial.println(ewaSpeakerName);
  Serial.print("- ");
  Serial.println(sinobandSpeakerName);
  Serial.println("Known SINOBAND address fallback: F5:4E:FD:0E:05:3D");
  Serial.print("Warning distance: ");
  Serial.print(warningDistanceCm);
  Serial.println(" cm");
  Serial.print("Danger distance: ");
  Serial.print(dangerDistanceCm);
  Serial.println(" cm");
  Serial.print("Very close distance: ");
  Serial.print(veryCloseDistanceCm);
  Serial.println(" cm");
  Serial.print("Distance output: centimeters, meters, and meters+centimeters.");
  Serial.println();
  Serial.print("Air temperature setting: ");
  Serial.print(airTemperatureC, 1);
  Serial.println(" C");
  Serial.print("Calibration scale: ");
  Serial.print(distanceCalibrationScale, 4);
  Serial.print(", offset: ");
  Serial.print(distanceCalibrationOffsetCm, 2);
  Serial.println(" cm");
  Serial.print("Samples per distance reading: ");
  Serial.print(sensorSampleAttempts);
  Serial.print(", minimum valid samples: ");
  Serial.println(minimumValidSamples);
  Serial.print("Embedded English male voice bytes: ");
  Serial.println(SPEECH_TOTAL_BYTES);
  Serial.print("Embedded Luganda voice bytes: ");
  Serial.println(LUGANDA_TOTAL_BYTES);
  Serial.print("Active voice language: ");
  Serial.println(activeVoiceLanguage == VOICE_LUGANDA ? "Luganda" : "English");

  runStartupBuzzerTest();
  setupBluetoothSpeaker();

  Serial.println("System ready. Reading distance...");
}

void loop() {
  const unsigned long nowMs = millis();
  const bool speakerConnected = a2dpSource.isConnected();

  updateBluetoothConnection(nowMs, speakerConnected);

  if (nowMs - lastSensorReadMs >= sensorReadIntervalMs) {
    lastSensorReadMs = nowMs;
    latestReading = readDistance();
    updateAlertReading(latestReading, nowMs);
    updateMovementTrend(alertReading, nowMs);
    updateObstacleConfirmations(latestReading);
    updateSmartScan(alertReading, nowMs, speakerConnected);
    updateBluetoothSpeech(alertReading, nowMs, speakerConnected);

    if (nowMs - lastSerialPrintMs >= serialPrintIntervalMs) {
      lastSerialPrintMs = nowMs;
      printDistance(alertReading, speakerConnected);
    }
  }

  updateBackupBuzzer(alertReading, nowMs, speakerConnected);
}

DistanceReading readDistance() {
  float validSamples[sensorSampleAttempts];
  float agreedSamples[sensorSampleAttempts];
  byte validSampleCount = 0;
  byte agreedSampleCount = 0;

  for (byte attempt = 0; attempt < sensorSampleAttempts; attempt++) {
    DistanceReading reading = readSingleDistance();
    if (reading.valid) {
      validSamples[validSampleCount++] = reading.distanceCm;
    }
    delay(retryDelayMs);
  }

  if (validSampleCount == 0) {
    return {0.0, false};
  }

  sortDistanceSamples(validSamples, validSampleCount);

  const float medianCm = validSamples[validSampleCount / 2];
  for (byte i = 0; i < validSampleCount; i++) {
    if (fabs(validSamples[i] - medianCm) <= sampleAgreementCm) {
      agreedSamples[agreedSampleCount++] = validSamples[i];
    }
  }

  if (agreedSampleCount < minimumValidSamples) {
    return {0.0, false};
  }

  const byte trimEachSide = agreedSampleCount >= 5 ? 1 : 0;
  float sumCm = 0.0;
  byte averagedCount = 0;
  for (byte i = trimEachSide; i < agreedSampleCount - trimEachSide; i++) {
    sumCm += agreedSamples[i];
    averagedCount++;
  }

  return {sumCm / averagedCount, true};
}

DistanceReading readSingleDistance() {
  digitalWrite(trigPin, LOW);
  delayMicroseconds(2);
  digitalWrite(trigPin, HIGH);
  delayMicroseconds(10);
  digitalWrite(trigPin, LOW);

  const unsigned long durationUs = pulseIn(echoPin, HIGH, maxEchoTimeUs);
  if (durationUs == 0) {
    return {0.0, false};
  }

  const float rawDistanceCm = durationUs * speedOfSoundCmPerUs / 2.0;
  const float distanceCm = calibrateDistanceCm(rawDistanceCm);

  if (distanceCm < minValidDistanceCm || distanceCm > maxValidDistanceCm) {
    return {0.0, false};
  }

  return {distanceCm, true};
}

float calibrateDistanceCm(float rawDistanceCm) {
  return (rawDistanceCm * distanceCalibrationScale) + distanceCalibrationOffsetCm;
}

float distanceMeters(float distanceCm) {
  return distanceCm / 100.0;
}

int wholeMeters(float distanceCm) {
  return (int)(distanceCm / 100.0);
}

float remainingCentimeters(float distanceCm) {
  return distanceCm - (wholeMeters(distanceCm) * 100.0);
}

void sortDistanceSamples(float samples[], byte sampleCount) {
  for (byte i = 0; i < sampleCount; i++) {
    for (byte j = i + 1; j < sampleCount; j++) {
      if (samples[j] < samples[i]) {
        const float temp = samples[i];
        samples[i] = samples[j];
        samples[j] = temp;
      }
    }
  }
}

void updateAlertReading(const DistanceReading &reading, unsigned long nowMs) {
  if (reading.valid) {
    alertReading = reading;
    lastValidReadingMs = nowMs;
    return;
  }

  if (nowMs - lastValidReadingMs > lastValidHoldMs) {
    alertReading = reading;
  }
}

void updateMovementTrend(const DistanceReading &reading, unsigned long nowMs) {
  if (!reading.valid) {
    currentMovementTrend = TREND_UNKNOWN;
    return;
  }

  if (lastTrendReferenceMs == 0) {
    lastTrendReferenceMs = nowMs;
    trendReferenceDistanceCm = reading.distanceCm;
    currentMovementTrend = TREND_STABLE;
    return;
  }

  if (nowMs - lastTrendReferenceMs < 650) {
    return;
  }

  const float deltaCm = reading.distanceCm - trendReferenceDistanceCm;
  if (deltaCm <= -12.0) {
    currentMovementTrend = TREND_CLOSING;
  } else if (deltaCm >= 12.0) {
    currentMovementTrend = TREND_OPENING;
  } else {
    currentMovementTrend = TREND_STABLE;
  }

  trendReferenceDistanceCm = reading.distanceCm;
  lastTrendReferenceMs = nowMs;
}

void updateObstacleConfirmations(const DistanceReading &reading) {
  if (reading.valid && reading.distanceCm <= warningDistanceCm) {
    if (obstacleSpeechConfirmCount < 255) {
      obstacleSpeechConfirmCount++;
    }
    pathClearPending = true;
    clearResetConfirmCount = 0;
  } else {
    obstacleSpeechConfirmCount = 0;
  }

  if (reading.valid && reading.distanceCm > warningDistanceCm) {
    if (clearResetConfirmCount < 255) {
      clearResetConfirmCount++;
    }
  } else if (!reading.valid) {
    clearResetConfirmCount = 0;
  }

  if (reading.valid && reading.distanceCm <= scanTriggerDistanceCm) {
    if (closeScanConfirmCount < 255) {
      closeScanConfirmCount++;
    }
  } else {
    closeScanConfirmCount = 0;
  }
}

void updateSmartScan(const DistanceReading &reading, unsigned long nowMs, bool speakerConnected) {
  if (scanState == SCAN_LEFT_COLLECT && reading.valid) {
    recordScanSample(reading.distanceCm, leftScanTopCm, &leftScanTopCount);
    leftScanBestCm = averageTopScanSamples(leftScanTopCm, leftScanTopCount);
  } else if (scanState == SCAN_RIGHT_COLLECT && reading.valid) {
    recordScanSample(reading.distanceCm, rightScanTopCm, &rightScanTopCount);
    rightScanBestCm = averageTopScanSamples(rightScanTopCm, rightScanTopCount);
  }

  if (scanState == SCAN_IDLE) {
    if (reading.valid &&
        reading.distanceCm <= scanTriggerDistanceCm &&
        closeScanConfirmCount >= closeScanConfirmSamples &&
        nowMs - lastScanCycleMs >= scanCooldownMs) {
      startLeftScan(nowMs, speakerConnected);
    }
    return;
  }

  if (scanState == SCAN_LEFT_PROMPT && nowMs - scanStateStartMs >= scanPromptDelayMs) {
    scanState = SCAN_LEFT_COLLECT;
    scanStateStartMs = nowMs;
    Serial.println("Collecting left scan now.");
    return;
  }

  if (scanState == SCAN_LEFT_COLLECT && nowMs - scanStateStartMs >= scanCollectMs) {
    moveToRightScan(nowMs, speakerConnected);
    return;
  }

  if (scanState == SCAN_RIGHT_PROMPT && nowMs - scanStateStartMs >= scanPromptDelayMs) {
    scanState = SCAN_RIGHT_COLLECT;
    scanStateStartMs = nowMs;
    Serial.println("Collecting right scan now.");
    return;
  }

  if (scanState == SCAN_RIGHT_COLLECT && nowMs - scanStateStartMs >= scanCollectMs) {
    finishScan(nowMs, speakerConnected);
    return;
  }

  if (scanState == SCAN_RESULT_HOLD && nowMs - scanStateStartMs >= scanResultHoldMs) {
    scanState = SCAN_IDLE;
  }
}

void startLeftScan(unsigned long nowMs, bool speakerConnected) {
  scanState = SCAN_LEFT_PROMPT;
  scanStateStartMs = nowMs;
  lastScanCycleMs = nowMs;
  resetScanSamples();

  Serial.println("Smart scan: immediate stop. Scan left slowly.");
  if (speakerConnected) {
    clearSpeechQueue();
    enqueueLocalizedClip(CLIP_STOP_NOW, LUGANDA_STOP_NOW);
    enqueueSpeechClip(CLIP_SILENCE_180);
    enqueueLocalizedClip(CLIP_SCAN_LEFT_SLOWLY, LUGANDA_SCAN_LEFT_SLOWLY);
    lastSpeechMs = nowMs;
  }
}

void moveToRightScan(unsigned long nowMs, bool speakerConnected) {
  scanState = SCAN_RIGHT_PROMPT;
  scanStateStartMs = nowMs;

  Serial.print("Left scan best: ");
  Serial.print(leftScanBestCm, 1);
  Serial.println(" cm. Prepare to scan right slowly.");
  if (speakerConnected) {
    if (isSpeechBusy()) {
      enqueueSpeechClip(CLIP_SILENCE_180);
      enqueueLocalizedClip(CLIP_SCAN_RIGHT_SLOWLY, LUGANDA_SCAN_RIGHT_SLOWLY);
    } else {
      clearSpeechQueue();
      enqueueLocalizedClip(CLIP_SCAN_RIGHT_SLOWLY, LUGANDA_SCAN_RIGHT_SLOWLY);
    }
    lastSpeechMs = nowMs;
  }
}

void finishScan(unsigned long nowMs, bool speakerConnected) {
  scanState = SCAN_RESULT_HOLD;
  scanStateStartMs = nowMs;

  Serial.print("Scan result left best: ");
  Serial.print(leftScanBestCm, 1);
  Serial.print(" cm / ");
  Serial.print(distanceMeters(leftScanBestCm), 2);
  Serial.print(" m, right best: ");
  Serial.print(rightScanBestCm, 1);
  Serial.print(" cm / ");
  Serial.print(distanceMeters(rightScanBestCm), 2);
  Serial.print(" m. Guidance: ");
  Serial.println(scanResultText());

  if (speakerConnected) {
    if (isSpeechBusy()) {
      enqueueSpeechClip(CLIP_SILENCE_180);
      enqueueScanResultSpeech();
    } else {
      enqueueScanResultSpeech();
    }
    lastSpeechMs = nowMs;
  }
}

void resetScanSamples() {
  leftScanBestCm = 0.0;
  rightScanBestCm = 0.0;
  leftScanTopCount = 0;
  rightScanTopCount = 0;
  for (byte i = 0; i < scanTopSampleCount; i++) {
    leftScanTopCm[i] = 0.0;
    rightScanTopCm[i] = 0.0;
  }
}

void recordScanSample(float distanceCm, float topSamples[], byte *sampleCount) {
  if (*sampleCount < scanTopSampleCount) {
    topSamples[*sampleCount] = distanceCm;
    (*sampleCount)++;
  } else if (distanceCm <= topSamples[scanTopSampleCount - 1]) {
    return;
  } else {
    topSamples[scanTopSampleCount - 1] = distanceCm;
  }

  for (byte i = 0; i < *sampleCount; i++) {
    for (byte j = i + 1; j < *sampleCount; j++) {
      if (topSamples[j] > topSamples[i]) {
        const float temp = topSamples[i];
        topSamples[i] = topSamples[j];
        topSamples[j] = temp;
      }
    }
  }
}

float averageTopScanSamples(const float topSamples[], byte sampleCount) {
  if (sampleCount == 0) {
    return 0.0;
  }

  float sumCm = 0.0;
  for (byte i = 0; i < sampleCount; i++) {
    sumCm += topSamples[i];
  }
  return sumCm / sampleCount;
}

void updateBluetoothConnection(unsigned long nowMs, bool speakerConnected) {
  if (speakerConnected && !previousSpeakerConnected) {
    lastSpeakerConnectedMs = nowMs;
    clearSpeechQueue();
    enqueueLocalizedClip(CLIP_CONNECTED, LUGANDA_CONNECTED);
    enqueueSpeechClip(CLIP_SILENCE_180);
    hasSpokenStatus = false;
    hasSpokenNoReading = false;
    pathClearPending = false;
    Serial.println("Voice event: Smart stick ready");
    Serial.print("Bluetooth speaker connected: ");
    Serial.println(a2dpSource.connectedSpeakerName());
  } else if (!speakerConnected && previousSpeakerConnected) {
    clearSpeechQueue();
    Serial.println("Bluetooth speaker disconnected. Backup buzzer is active.");
  }
  previousSpeakerConnected = speakerConnected;

  if (!speakerConnected && nowMs - lastConnectionPrintMs >= connectionPrintIntervalMs) {
    lastConnectionPrintMs = nowMs;
    Serial.print("Scanning for Bluetooth speakers: ");
    Serial.print(ewaSpeakerName);
    Serial.print(" or ");
    Serial.println(sinobandSpeakerName);
  }

  if (!speakerConnected &&
      nowMs >= addressConnectDelayMs &&
      nowMs - lastAddressConnectAttemptMs >= addressConnectRetryMs) {
    lastAddressConnectAttemptMs = nowMs;
    Serial.println("Trying saved SINOBAND Book Bluetooth address: F5:4E:FD:0E:05:3D");
    if (!a2dpSource.connectToKnownAddress(sinobandKnownSpeakerAddress, sinobandSpeakerName)) {
      Serial.println("Saved SINOBAND address attempt could not start yet.");
    }
  }
}

void updateBluetoothSpeech(const DistanceReading &reading, unsigned long nowMs, bool speakerConnected) {
  if (!speakerConnected || nowMs - lastSpeakerConnectedMs < 2200) {
    return;
  }
  if (scanState != SCAN_IDLE) {
    return;
  }

  if (shouldSpeakPathClear(reading, nowMs)) {
    if (isSpeechBusy()) {
      return;
    }

    clearSpeechQueue();
    enqueueLocalizedClip(CLIP_PATH_CLEAR, LUGANDA_PATH_CLEAR);
    lastSpeechMs = nowMs;
    lastSpokenDistanceCm = (int)lround(reading.distanceCm);
    lastSpokenBucket = BUCKET_CLEAR;
    hasSpokenStatus = false;
    hasSpokenNoReading = false;
    pathClearPending = false;
    Serial.println("Voice event: path clear");
    return;
  }

  if (!shouldSpeakStatus(reading, nowMs)) {
    return;
  }
  if (isSpeechBusy()) {
    return;
  }

  clearSpeechQueue();
  enqueueStatusSpeech(reading);
  lastSpeechMs = nowMs;

  if (reading.valid) {
    Serial.print("Voice event: obstacle at ");
    Serial.print(reading.distanceCm, 1);
    Serial.println(" cm");
    lastSpokenDistanceCm = (int)lround(reading.distanceCm);
    lastSpokenBucket = distanceBucketFor(reading.distanceCm);
    hasSpokenStatus = true;
    hasSpokenNoReading = false;
  } else {
    hasSpokenNoReading = true;
  }
}

bool shouldSpeakPathClear(const DistanceReading &reading, unsigned long nowMs) {
  if (!reading.valid || reading.distanceCm <= warningDistanceCm) {
    return false;
  }
  if (!pathClearPending || clearResetConfirmCount < clearResetConfirmSamples) {
    return false;
  }
  if (nowMs - lastSpeechMs < 900) {
    return false;
  }
  return true;
}

bool shouldSpeakStatus(const DistanceReading &reading, unsigned long nowMs) {
  if (!reading.valid) {
    hasSpokenStatus = false;
    lastSpokenBucket = BUCKET_INVALID;
    return false;
  }

  const int distanceCm = (int)lround(reading.distanceCm);
  const DistanceBucket bucket = distanceBucketFor(reading.distanceCm);

  if (distanceCm > warningDistanceCm) {
    if (clearResetConfirmCount >= clearResetConfirmSamples) {
      hasSpokenStatus = false;
      hasSpokenNoReading = false;
      lastSpokenBucket = BUCKET_CLEAR;
      lastSpokenDistanceCm = distanceCm;
    }
    return false;
  }
  if (obstacleSpeechConfirmCount < obstacleSpeechConfirmSamples) {
    return false;
  }

  if (!hasSpokenStatus) {
    return true;
  }
  if (nowMs - lastSpeechMs < speechObstacleQuietMs) {
    return false;
  }
  if (bucket != lastSpokenBucket) {
    return true;
  }

  const int distanceDelta = abs(distanceCm - lastSpokenDistanceCm);
  const int requiredDelta = distanceCm <= warningDistanceCm ? speechNearDistanceDeltaCm : speechFarDistanceDeltaCm;
  return distanceDelta >= requiredDelta;
}

void enqueueStatusSpeech(const DistanceReading &reading) {
  if (!reading.valid) {
    enqueueSpeechClip(CLIP_NO_SENSOR_ECHO);
    enqueueSpeechClip(CLIP_SILENCE_180);
    return;
  }

  if (activeVoiceLanguage == VOICE_LUGANDA) {
    enqueueGuidanceSpeech(reading);
    return;
  }

  enqueueSpeechClip(CLIP_DISTANCE);
  enqueueSpeechClip(CLIP_SILENCE_80);
  enqueueSpeechClip(CLIP_IS);
  enqueueSpeechClip(CLIP_SILENCE_80);
  enqueueDistanceSpeech(reading.distanceCm);
  enqueueSpeechClip(CLIP_SILENCE_180);
  enqueueGuidanceSpeech(reading);
}

void enqueueDistanceSpeech(float distanceCm) {
  const int totalCm = constrain((int)lround(distanceCm), 0, 400);
  int meters = totalCm / 100;
  int tenths = ((totalCm % 100) + 5) / 10;

  if (tenths >= 10) {
    meters++;
    tenths = 0;
  }

  enqueueNumberSpeech(meters);
  enqueueSpeechClip(CLIP_SILENCE_80);
  enqueueSpeechClip(CLIP_POINT);
  enqueueSpeechClip(CLIP_SILENCE_80);
  enqueueSpeechClip((SpeechClipId)(CLIP_ZERO + tenths));
  enqueueSpeechClip(CLIP_SILENCE_80);
  enqueueSpeechClip(CLIP_METERS);
}

void enqueueGuidanceSpeech(const DistanceReading &reading) {
  if (!reading.valid) {
    enqueueSpeechClip(CLIP_NO_SENSOR_ECHO);
    return;
  }

  if (reading.distanceCm <= 20.0) {
    enqueueLocalizedClip(CLIP_OBSTACLE_AHEAD, LUGANDA_OBSTACLE_AHEAD);
    enqueueSpeechClip(CLIP_SILENCE_180);
    enqueueSpeechClip(CLIP_SILENCE_180);
    enqueueLocalizedClip(CLIP_STOP_NOW, LUGANDA_STOP_NOW);
  } else if (reading.distanceCm <= 40.0) {
    enqueueLocalizedClip(CLIP_OBSTACLE_AHEAD, LUGANDA_OBSTACLE_AHEAD);
    enqueueSpeechClip(CLIP_SILENCE_180);
    enqueueSpeechClip(CLIP_SILENCE_180);
    enqueueLocalizedClip(CLIP_STOP_NOW, LUGANDA_STOP_NOW);
  } else if (reading.distanceCm <= dangerDistanceCm) {
    enqueueLocalizedClip(CLIP_OBSTACLE_AHEAD, LUGANDA_OBSTACLE_AHEAD);
    enqueueSpeechClip(CLIP_SILENCE_180);
    enqueueSpeechClip(CLIP_SILENCE_180);
    enqueueLocalizedClip(CLIP_STOP_NOW, LUGANDA_STOP_NOW);
  } else if (reading.distanceCm <= warningDistanceCm) {
    enqueueLocalizedClip(CLIP_OBSTACLE_AHEAD_MOVE_CAREFULLY, LUGANDA_OBSTACLE_AHEAD_MOVE_CAREFULLY);
  } else if (reading.distanceCm <= 250.0) {
    enqueueSpeechClip(CLIP_MOVE_FORWARD_CAREFULLY);
  } else {
    enqueueLocalizedClip(CLIP_PATH_CLEAR, LUGANDA_PATH_CLEAR);
  }
}

void enqueueScanResultSpeech() {
  if (shouldMoveLeftAfterScan()) {
    enqueueLocalizedClip(CLIP_LEFT_SEEMS_CLEARER, LUGANDA_LEFT_SEEMS_CLEARER);
    return;
  }

  if (shouldMoveRightAfterScan()) {
    enqueueLocalizedClip(CLIP_RIGHT_SEEMS_CLEARER, LUGANDA_RIGHT_SEEMS_CLEARER);
    return;
  }

  enqueueLocalizedClip(CLIP_NO_CLEAR_SIDE, LUGANDA_NO_CLEAR_SIDE);
}

bool shouldMoveLeftAfterScan() {
  if (!hasUsableScanSide()) {
    return false;
  }

  if (leftScanBestCm < scanMinimumMoveCm) {
    return false;
  }
  if (rightScanBestCm < scanMinimumMoveCm) {
    return true;
  }

  return leftScanBestCm + scanSmallAdvantageCm >= rightScanBestCm;
}

bool shouldMoveRightAfterScan() {
  if (!hasUsableScanSide()) {
    return false;
  }

  if (rightScanBestCm < scanMinimumMoveCm) {
    return false;
  }
  if (leftScanBestCm < scanMinimumMoveCm) {
    return true;
  }

  return rightScanBestCm > leftScanBestCm + scanSmallAdvantageCm;
}

bool hasUsableScanSide() {
  return leftScanBestCm >= scanMinimumMoveCm || rightScanBestCm >= scanMinimumMoveCm;
}

void enqueueNumberSpeech(int value) {
  value = constrain(value, 0, 400);
  if (value >= 100) {
    enqueueSpeechClip((SpeechClipId)(CLIP_ZERO + (value / 100)));
    enqueueSpeechClip(CLIP_HUNDRED);
    const int remainder = value % 100;
    if (remainder > 0) {
      enqueueNumberUnder100Speech(remainder);
    }
    return;
  }

  enqueueNumberUnder100Speech(value);
}

void enqueueNumberUnder100Speech(int value) {
  value = constrain(value, 0, 99);
  if (value < 20) {
    enqueueSpeechClip((SpeechClipId)(CLIP_ZERO + value));
    return;
  }

  const int tens = value / 10;
  const int ones = value % 10;
  enqueueSpeechClip((SpeechClipId)(CLIP_TWENTY + (tens - 2)));
  if (ones > 0) {
    enqueueSpeechClip((SpeechClipId)(CLIP_ZERO + ones));
  }
}

void updateBackupBuzzer(const DistanceReading &reading, unsigned long nowMs, bool speakerConnected) {
  if (speakerConnected) {
    stopBackupBuzzer();
    buzzerCycleStartMs = nowMs;
    return;
  }

  if (!reading.valid) {
    playBackupPattern({noReadingBeepIntervalMs, noReadingBeepOnMs}, nowMs);
  } else if (reading.distanceCm <= veryCloseDistanceCm) {
    playBackupPattern({140, 100}, nowMs);
  } else if (reading.distanceCm <= dangerDistanceCm) {
    playBackupPattern({260, 90}, nowMs);
  } else if (reading.distanceCm <= warningDistanceCm) {
    playBackupPattern({700, 100}, nowMs);
  } else {
    stopBackupBuzzer();
    buzzerCycleStartMs = nowMs;
  }
}

void playBackupPattern(const AlertPattern &pattern, unsigned long nowMs) {
  if (nowMs - buzzerCycleStartMs >= pattern.intervalMs) {
    buzzerCycleStartMs = nowMs;
    startBackupBuzzer();
  }

  if (backupBuzzerIsOn && nowMs - buzzerCycleStartMs >= pattern.onTimeMs) {
    stopBackupBuzzer();
  }
}

void startBackupBuzzer() {
  digitalWrite(buzzerPin, HIGH);
  backupBuzzerIsOn = true;
}

void stopBackupBuzzer() {
  digitalWrite(buzzerPin, LOW);
  backupBuzzerIsOn = false;
}

void runStartupBuzzerTest() {
  Serial.println("Startup backup buzzer test...");
  for (byte i = 0; i < 2; i++) {
    startBackupBuzzer();
    delay(80);
    stopBackupBuzzer();
    delay(120);
  }
}

void configureBootLanguage() {
  languagePrefs.begin(languagePrefsNamespace, false);
  const uint32_t bootCount = languagePrefs.getUInt(bootCounterKey, 0);
  activeVoiceLanguage = (bootCount % 2 == 0) ? VOICE_ENGLISH : VOICE_LUGANDA;
  languagePrefs.putUInt(bootCounterKey, bootCount + 1);
  languagePrefs.end();
}

void setupBluetoothSpeaker() {
  std::vector<const char *> speakerNames = {
      ewaSpeakerName,
      sinobandSpeakerName,
      sinobandSpeakerNameAlt1,
      sinobandSpeakerNameAlt2,
  };

  a2dpSource.setPinCode("0000");
  a2dpSource.start(speakerNames, getAudioSamples, true);
  Serial.println("Bluetooth A2DP source started. Keep EWA Audio A20 or SINOBAND Book on, close, and not connected to phone/PC.");
}

void clearSpeechQueue() {
  portENTER_CRITICAL(&speechQueueMux);
  speechQueueHead = 0;
  speechQueueTail = 0;
  speechResetRequested = true;
  speechOutputActive = false;
  portEXIT_CRITICAL(&speechQueueMux);
}

void speakImmediate(SpeechClipId firstClip, SpeechClipId secondClip) {
  clearSpeechQueue();
  enqueueSpeechClip(firstClip);
  if (secondClip != SPEECH_CLIP_COUNT) {
    enqueueSpeechClip(CLIP_SILENCE_180);
    enqueueSpeechClip(secondClip);
  }
}

void enqueueSpeechClip(SpeechClipId clipId) {
  portENTER_CRITICAL(&speechQueueMux);
  const uint8_t nextTail = (speechQueueTail + 1) % speechQueueSize;
  if (nextTail != speechQueueHead) {
    speechQueue[speechQueueTail] = (int16_t)clipId;
    speechQueueTail = nextTail;
  }
  portEXIT_CRITICAL(&speechQueueMux);
}

void enqueueLugandaClip(LugandaClipId clipId) {
  portENTER_CRITICAL(&speechQueueMux);
  const uint8_t nextTail = (speechQueueTail + 1) % speechQueueSize;
  if (nextTail != speechQueueHead) {
    speechQueue[speechQueueTail] = lugandaClipBase + (int16_t)clipId;
    speechQueueTail = nextTail;
  }
  portEXIT_CRITICAL(&speechQueueMux);
}

void enqueueLocalizedClip(SpeechClipId englishClip, LugandaClipId lugandaClip) {
  if (activeVoiceLanguage == VOICE_LUGANDA) {
    enqueueLugandaClip(lugandaClip);
  } else {
    enqueueSpeechClip(englishClip);
  }
}

bool popSpeechClip(int16_t *clipId) {
  bool hasClip = false;
  portENTER_CRITICAL(&speechQueueMux);
  if (speechQueueHead != speechQueueTail) {
    *clipId = speechQueue[speechQueueHead];
    speechQueueHead = (speechQueueHead + 1) % speechQueueSize;
    hasClip = true;
  }
  portEXIT_CRITICAL(&speechQueueMux);
  return hasClip;
}

bool resolveSpeechClip(int16_t clipId, const uint8_t **clipData, uint32_t *clipLength) {
  if (clipId >= lugandaClipBase) {
    const int16_t lugandaIndex = clipId - lugandaClipBase;
    if (lugandaIndex < 0 || lugandaIndex >= LUGANDA_CLIP_COUNT) {
      return false;
    }
    *clipData = LUGANDA_CLIPS[lugandaIndex].data;
    *clipLength = LUGANDA_CLIPS[lugandaIndex].length;
    return true;
  }

  if (clipId < 0 || clipId >= SPEECH_CLIP_COUNT) {
    return false;
  }
  *clipData = SPEECH_CLIPS[clipId].data;
  *clipLength = SPEECH_CLIPS[clipId].length;
  return true;
}

bool isSpeechBusy() {
  bool queueHasPending = false;
  portENTER_CRITICAL(&speechQueueMux);
  queueHasPending = speechQueueHead != speechQueueTail;
  portEXIT_CRITICAL(&speechQueueMux);
  return speechOutputActive || queueHasPending;
}

int32_t getAudioSamples(Channels *channels, int32_t channelLen) {
  static int16_t currentClipId = noSpeechClip;
  static const uint8_t *currentClipData = nullptr;
  static uint32_t currentClipLength = 0;
  static uint32_t currentClipPosQ16 = 0;
  static int32_t previousVoiceInput = 0;
  static int32_t previousVoiceOutput = 0;

  for (int32_t i = 0; i < channelLen; i++) {
    int16_t sampleValue = 0;

    if (speechResetRequested) {
      currentClipId = noSpeechClip;
      currentClipData = nullptr;
      currentClipLength = 0;
      currentClipPosQ16 = 0;
      previousVoiceInput = 0;
      previousVoiceOutput = 0;
      portENTER_CRITICAL(&speechQueueMux);
      speechResetRequested = false;
      portEXIT_CRITICAL(&speechQueueMux);
    }

    for (byte attempt = 0; attempt < 4; attempt++) {
      if (currentClipId == noSpeechClip) {
        int16_t nextClip;
        if (!popSpeechClip(&nextClip)) {
          break;
        }

        currentClipId = nextClip;
        if (!resolveSpeechClip(nextClip, &currentClipData, &currentClipLength)) {
          currentClipId = noSpeechClip;
          currentClipData = nullptr;
          currentClipLength = 0;
          continue;
        }
        currentClipPosQ16 = 0;
      }

      const uint32_t clipIndex = currentClipPosQ16 >> 16;
      if (clipIndex >= currentClipLength) {
        currentClipId = noSpeechClip;
        currentClipData = nullptr;
        currentClipLength = 0;
        currentClipPosQ16 = 0;
        continue;
      }

      sampleValue = decodeMuLaw(pgm_read_byte(currentClipData + clipIndex));
      currentClipPosQ16 += speechStepQ16;
      break;
    }

    int32_t filteredSample =
        (voiceHighPassQ15 * (previousVoiceOutput + sampleValue - previousVoiceInput)) >> 15;
    previousVoiceInput = sampleValue;
    previousVoiceOutput = filteredSample;
    filteredSample = (filteredSample * voiceClarityGainQ8) >> 8;
    if (filteredSample > 32767) {
      filteredSample = 32767;
    } else if (filteredSample < -32768) {
      filteredSample = -32768;
    }
    sampleValue = (int16_t)filteredSample;

    channels[i].channel1 = sampleValue;
    channels[i].channel2 = sampleValue;
  }

  portENTER_CRITICAL(&speechQueueMux);
  speechOutputActive = currentClipId != noSpeechClip || speechQueueHead != speechQueueTail;
  portEXIT_CRITICAL(&speechQueueMux);

  return channelLen;
}

int16_t decodeMuLaw(uint8_t value) {
  value = ~value;
  int16_t decoded = ((value & 0x0F) << 3) + 0x84;
  decoded <<= (value & 0x70) >> 4;
  return (value & 0x80) ? (0x84 - decoded) : (decoded - 0x84);
}

DistanceBucket distanceBucketFor(float distanceCm) {
  if (distanceCm <= veryCloseDistanceCm) {
    return BUCKET_VERY_CLOSE;
  }
  if (distanceCm <= 40.0) {
    return BUCKET_CLOSE;
  }
  if (distanceCm <= dangerDistanceCm) {
    return BUCKET_NEAR;
  }
  if (distanceCm <= warningDistanceCm) {
    return BUCKET_WARNING;
  }
  if (distanceCm <= 250.0) {
    return BUCKET_CAUTION;
  }
  return BUCKET_CLEAR;
}

const char *guidanceText(const DistanceReading &reading) {
  if (scanState == SCAN_LEFT_PROMPT) {
    return "Prepare to scan left slowly";
  }
  if (scanState == SCAN_LEFT_COLLECT) {
    return "Scan left slowly";
  }
  if (scanState == SCAN_RIGHT_PROMPT) {
    return "Prepare to scan right slowly";
  }
  if (scanState == SCAN_RIGHT_COLLECT) {
    return "Scan right slowly";
  }
  if (scanState == SCAN_RESULT_HOLD) {
    return scanResultText();
  }

  if (!reading.valid) {
    return "No sensor echo";
  }

  if (reading.distanceCm <= 20.0) {
    return "Stop now";
  }
  if (reading.distanceCm <= 40.0) {
    return "Obstacle very close. Stop";
  }
  if (reading.distanceCm <= dangerDistanceCm) {
    return "Obstacle close. Stop";
  }
  if (reading.distanceCm <= warningDistanceCm) {
    return "Obstacle detected. Move carefully";
  }
  if (reading.distanceCm <= 250.0) {
    return "Clear beyond warning range";
  }
  return "Path clear";
}

const char *scanResultText() {
  if (shouldMoveRightAfterScan()) {
    return "Right seems clearer";
  }

  if (shouldMoveLeftAfterScan()) {
    return "Left seems clearer";
  }

  return "No clear side";
}

const char *bucketName(DistanceBucket bucket) {
  switch (bucket) {
    case BUCKET_VERY_CLOSE:
      return "very_close";
    case BUCKET_CLOSE:
      return "close";
    case BUCKET_NEAR:
      return "near";
    case BUCKET_WARNING:
      return "warning";
    case BUCKET_CAUTION:
      return "caution";
    case BUCKET_CLEAR:
      return "clear";
    default:
      return "invalid";
  }
}

const char *trendName(MovementTrend trend) {
  switch (trend) {
    case TREND_CLOSING:
      return "closing";
    case TREND_STABLE:
      return "stable";
    case TREND_OPENING:
      return "opening";
    default:
      return "unknown";
  }
}

const char *scanStateName(ScanState state) {
  switch (state) {
    case SCAN_LEFT_PROMPT:
      return "left_prompt";
    case SCAN_LEFT_COLLECT:
      return "left";
    case SCAN_RIGHT_PROMPT:
      return "right_prompt";
    case SCAN_RIGHT_COLLECT:
      return "right";
    case SCAN_RESULT_HOLD:
      return "result";
    default:
      return "idle";
  }
}

void printDistance(const DistanceReading &reading, bool speakerConnected) {
  Serial.print("BT speaker: ");
  Serial.print(speakerConnected ? "connected" : "scanning");
  Serial.print(" | ");

  if (!reading.valid) {
    Serial.println("Distance: invalid/no echo | guidance: No sensor echo");
    return;
  }

  const DistanceBucket bucket = distanceBucketFor(reading.distanceCm);
  Serial.print("Distance: ");
  Serial.print(reading.distanceCm, 1);
  Serial.print(" cm | ");
  Serial.print(distanceMeters(reading.distanceCm), 2);
  Serial.print(" m | ");
  Serial.print(wholeMeters(reading.distanceCm));
  Serial.print(" m ");
  Serial.print(remainingCentimeters(reading.distanceCm), 1);
  Serial.print(" cm | range: ");
  Serial.print(bucketName(bucket));
  Serial.print(" | trend: ");
  Serial.print(trendName(currentMovementTrend));
  Serial.print(" | scan: ");
  Serial.print(scanStateName(scanState));
  if (scanState != SCAN_IDLE) {
    Serial.print(" | left best: ");
    Serial.print(leftScanBestCm, 1);
    Serial.print(" cm, right best: ");
    Serial.print(rightScanBestCm, 1);
    Serial.print(" cm");
  }
  Serial.print(" | guidance: ");
  Serial.println(guidanceText(reading));
}
