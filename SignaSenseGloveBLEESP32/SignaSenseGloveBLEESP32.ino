/*
  SignaSense ESP32 Smart Glove

  Purpose:
  - Reads 5 flex sensors.
  - Advertises as a BLE device named SignaSenseGlove.
  - Sends live JSON readings to the SignaSense phone app over Bluetooth LE.
  - No internet or mobile data is required.

  Phone use:
  1. Power the ESP32 glove.
  2. Open SignaSense and choose the Smart Glove BLE screen.
  3. Tap connect and select SignaSenseGlove.

  Exact wiring requested:
  - Pinky  flex sensor divider output -> GPIO34
  - Ring   flex sensor divider output -> GPIO35
  - Middle flex sensor divider output -> GPIO32
  - Index  flex sensor divider output -> GPIO33
  - Thumb  flex sensor divider output -> GPIO25
  - Each flex sensor is a voltage divider powered from 3.3V.
  - Each divider has its own resistor to GND.
  - All grounds must be common with ESP32 GND.

  Important ESP32 ADC note:
  - GPIO25 is ADC2. ADC2 is unreliable while Wi-Fi is active.
  - This Bluetooth LE version does not start Wi-Fi, so the requested pin map can
    be read without the Wi-Fi/ADC2 conflict.

  Calibration:
  - Hold the hand in a straight relaxed position during startup.
  - Tap Recalibrate on the page whenever the glove is worn differently.
  - The first valid reading after calibration becomes the straight baseline.
  - Bends are detected by movement away from that baseline, so the code works
    whether your divider voltage rises or falls when a finger bends.

  Sign language note:
  - A flex-only glove cannot fully read international/ASL letters because it
    cannot see palm direction, finger spread, contact, or motion.
  - This sketch gives practical one-hand bend-pattern letters for the app:
    A, B, C, D, E, F, I, L, V, W, X, Y.
*/

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>

// ---------------------------------------------------------------------------
// Wi-Fi page settings. Keep these aligned with the installed SignaSense app.
// ---------------------------------------------------------------------------

const char WIFI_AP_NAME[] = "SmartGlove";
const char WIFI_AP_PASSWORD[] = "12345678";
const IPAddress AP_IP(192, 168, 4, 1);
const IPAddress AP_GATEWAY(192, 168, 4, 1);
const IPAddress AP_SUBNET(255, 255, 255, 0);

WebServer server(80);

// ---------------------------------------------------------------------------
// BLE settings. Keep these aligned with the SignaSense Android app.
// ---------------------------------------------------------------------------

const char BLE_DEVICE_NAME[] = "SignaSenseGlove";
const char SERVICE_UUID[] = "8b77a001-7d7c-4f47-b3d5-0f4d4902c001";
const char DATA_CHAR_UUID[] = "8b77a002-7d7c-4f47-b3d5-0f4d4902c001";

BLEServer *bleServer = nullptr;
BLECharacteristic *dataCharacteristic = nullptr;
bool bleClientConnected = false;
bool oldBleClientConnected = false;

// ---------------------------------------------------------------------------
// Finger order and requested pin map.
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

const int FINGER_PINS[NUM_FINGERS] = {
  25, // Thumb  - ADC2, may be blocked by Wi-Fi
  33, // Index  - ADC1
  32, // Middle - ADC1
  35, // Ring   - ADC1 input-only
  34  // Pinky  - ADC1 input-only
};

// Thumb is optional because GPIO25 is ADC2 and may not read while Wi-Fi runs.
const bool OPTIONAL_FOR_RECOGNITION[NUM_FINGERS] = {
  true, false, false, false, false
};

// ---------------------------------------------------------------------------
// Sensor tuning.
// ---------------------------------------------------------------------------

const unsigned long SENSOR_INTERVAL_MS = 25;
const unsigned long SERIAL_INTERVAL_MS = 600;
const unsigned long BLE_NOTIFY_INTERVAL_MS = 250;
const unsigned long LETTER_STABLE_MS = 750;
const unsigned long SAME_LETTER_REPEAT_MS = 2800;

const float SMOOTHING_ALPHA = 0.20f;
const int RAW_LOW_NO_SIGNAL = 2;
const int RAW_HIGH_NO_SIGNAL = 4093;
const float MIN_USEFUL_BEND_DELTA = 55.0f;
const float DEFAULT_FULL_BEND_DELTA = 850.0f;

const int STRAIGHT_MAX_PERCENT = 28;
const int BENT_MIN_PERCENT = 68;
const int MIN_REQUIRED_ACTIVE_FINGERS = 3;
const byte MAX_WORD_LENGTH = 32;
const byte MAX_SENTENCE_LENGTH = 150;

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

// ---------------------------------------------------------------------------
// Runtime state.
// ---------------------------------------------------------------------------

int rawValues[NUM_FINGERS] = {0, 0, 0, 0, 0};
float smoothedValues[NUM_FINGERS] = {0, 0, 0, 0, 0};
float baselineValues[NUM_FINGERS] = {0, 0, 0, 0, 0};
float maxObservedDelta[NUM_FINGERS] = {0, 0, 0, 0, 0};
int observedMinValues[NUM_FINGERS] = {4095, 4095, 4095, 4095, 4095};
int observedMaxValues[NUM_FINGERS] = {0, 0, 0, 0, 0};
int bendPercent[NUM_FINGERS] = {0, 0, 0, 0, 0};
FingerState fingerStates[NUM_FINGERS] = {
  STATE_NO_SIGNAL, STATE_NO_SIGNAL, STATE_NO_SIGNAL, STATE_NO_SIGNAL, STATE_NO_SIGNAL
};
bool sensorHasSignal[NUM_FINGERS] = {false, false, false, false, false};
bool baselineReady[NUM_FINGERS] = {false, false, false, false, false};
bool smoothingReady = false;

unsigned long lastSensorReadMs = 0;
unsigned long lastSerialPrintMs = 0;
unsigned long lastBleNotifyMs = 0;
int currentLetterIndex = LETTER_UNKNOWN;
int candidateLetterIndex = LETTER_UNKNOWN;
int stableLetterIndex = LETTER_UNKNOWN;
unsigned long candidateLetterSinceMs = 0;
unsigned long lastLetterAppendMs = 0;
String currentWord = "";
String currentSentence = "";
String lastCommittedWord = "";
unsigned long wordCommitCounter = 0;
size_t lastBleJsonLength = 0;

void handleCommand(const String &command);
String buildStatusJson();

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

// ---------------------------------------------------------------------------
// Local web app served to the phone.
// ---------------------------------------------------------------------------

const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!doctype html>
<html>
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <title>SignaSense Smart Glove</title>
  <style>
    :root {
      font-family: Arial, sans-serif;
      color: #16191d;
      background: #f4f7f8;
      --panel: #ffffff;
      --line: #d5dde3;
      --muted: #58616b;
      --accent: #0f766e;
      --good: #1f7a43;
      --warn: #b25e00;
      --bad: #a5332a;
    }
    * { box-sizing: border-box; }
    body { margin: 0; background: #f4f7f8; }
    header {
      background: #fff;
      border-bottom: 1px solid var(--line);
      padding: 16px;
    }
    main { max-width: 1100px; margin: 0 auto; padding: 16px; }
    .top { max-width: 1100px; margin: 0 auto; display: grid; gap: 8px; }
    h1 { margin: 0; font-size: 28px; line-height: 1.1; }
    p { margin: 0; color: var(--muted); line-height: 1.45; }
    section { padding: 16px 0; border-bottom: 1px solid var(--line); }
    section:last-child { border-bottom: 0; }
    .badges, .buttons { display: flex; flex-wrap: wrap; gap: 8px; }
    .badge {
      border-radius: 6px;
      background: #eef2f4;
      color: #242930;
      padding: 8px 10px;
      font-size: 13px;
      font-weight: 700;
    }
    .badge.good { color: var(--good); background: #e8f5ed; }
    .badge.warn { color: var(--warn); background: #fff3e4; }
    .badge.bad { color: var(--bad); background: #f8e8e6; }
    .grid {
      display: grid;
      grid-template-columns: repeat(3, minmax(0, 1fr));
      gap: 12px;
    }
    .panel {
      background: var(--panel);
      border: 1px solid var(--line);
      border-radius: 6px;
      padding: 14px;
      min-height: 130px;
      display: grid;
      gap: 10px;
      align-content: start;
    }
    .label {
      color: var(--muted);
      font-size: 12px;
      font-weight: 700;
      text-transform: uppercase;
    }
    .big { font-size: 44px; font-weight: 800; line-height: 1; overflow-wrap: anywhere; }
    .word { font-size: 32px; }
    .sentence { font-size: 24px; line-height: 1.2; }
    button {
      border: 0;
      border-radius: 6px;
      background: var(--accent);
      color: white;
      padding: 11px 13px;
      font-weight: 700;
      font-size: 14px;
    }
    button.alt { background: #626b75; }
    button.bad { background: var(--bad); }
    .fingers {
      display: grid;
      grid-template-columns: repeat(5, minmax(0, 1fr));
      gap: 10px;
    }
    .finger {
      background: var(--panel);
      border: 1px solid var(--line);
      border-radius: 6px;
      padding: 12px;
      display: grid;
      gap: 8px;
    }
    .finger-title { display: flex; justify-content: space-between; gap: 6px; font-weight: 700; }
    .finger-title span:last-child { color: var(--muted); font-weight: 500; }
    .bar { height: 12px; border-radius: 6px; background: #e2e8ec; overflow: hidden; }
    .fill { height: 100%; width: 0%; background: linear-gradient(90deg, #0f766e, #c77700); }
    .raw { font-size: 13px; color: var(--muted); line-height: 1.45; overflow-wrap: anywhere; }
    .note {
      background: #e9f5f3;
      border: 1px solid #b9ddd8;
      border-radius: 6px;
      padding: 12px;
      color: #243238;
    }
    @media (max-width: 820px) {
      .grid, .fingers { grid-template-columns: repeat(2, minmax(0, 1fr)); }
      .big { font-size: 36px; }
      .word { font-size: 28px; }
      .sentence { font-size: 20px; }
    }
    @media (max-width: 560px) {
      .grid, .fingers { grid-template-columns: 1fr; }
      h1 { font-size: 24px; }
    }
  </style>
</head>
<body>
  <header>
    <div class="top">
      <h1>SignaSense Smart Glove</h1>
      <p>Live finger readings, recognized letters, words, captions, and phone audio.</p>
      <div class="badges">
        <div id="wifiBadge" class="badge">Wi-Fi: SmartGlove</div>
        <div id="sensorBadge" class="badge warn">Checking sensors</div>
        <div id="audioBadge" class="badge warn">Audio locked</div>
      </div>
    </div>
  </header>

  <main>
    <section>
      <div class="grid">
        <div class="panel">
          <div class="label">Current letter</div>
          <div id="letter" class="big">...</div>
          <p id="letterNote">Hold a stable bend pattern.</p>
        </div>
        <div class="panel">
          <div class="label">Word</div>
          <div id="word" class="big word">...</div>
          <p>Stable letters are added automatically.</p>
        </div>
        <div class="panel">
          <div class="label">Captions</div>
          <div id="sentence" class="big sentence">...</div>
          <p>Commit the word to build a sentence.</p>
        </div>
      </div>
    </section>

    <section>
      <div class="buttons">
        <button onclick="enableAudio()">Enable Audio</button>
        <button class="alt" onclick="speakNow('letter')">Speak Letter</button>
        <button class="alt" onclick="speakNow('word')">Speak Word</button>
        <button class="alt" onclick="speakNow('sentence')">Speak Caption</button>
        <button onclick="sendAction('space')">Commit Word</button>
        <button class="alt" onclick="sendAction('backspace')">Backspace</button>
        <button class="bad" onclick="sendAction('clear')">Clear Word</button>
        <button class="bad" onclick="sendAction('clearsentence')">Clear Caption</button>
        <button onclick="sendAction('recalibrate')">Recalibrate</button>
      </div>
      <p id="audioStatus" style="margin-top:10px;">Tap Enable Audio once. Inside the SignaSense app, Android TTS is used when available.</p>
    </section>

    <section>
      <p id="statusLine" class="note">Connecting to glove...</p>
    </section>

    <section>
      <div id="fingerGrid" class="fingers"></div>
    </section>
  </main>

  <script>
    const names = ["Thumb", "Index", "Middle", "Ring", "Pinky"];
    let latest = null;
    let audioEnabled = false;
    let lastSpokenLetter = "";
    const fingerGrid = document.getElementById("fingerGrid");

    names.forEach((name, index) => {
      const card = document.createElement("div");
      card.className = "finger";
      card.innerHTML = `
        <div class="finger-title"><span>${name}</span><span id="state${index}">?</span></div>
        <div class="bar"><div id="fill${index}" class="fill"></div></div>
        <div id="raw${index}" class="raw">Raw: --<br>Bend: --</div>
      `;
      fingerGrid.appendChild(card);
    });

    function text(id, value) {
      document.getElementById(id).textContent = value;
    }

    function enableAudio() {
      audioEnabled = true;
      text("audioBadge", "Audio ready");
      document.getElementById("audioBadge").className = "badge good";
      speak("Smart glove audio is ready.");
    }

    function speak(message) {
      if (!audioEnabled || !message || message === "...") return;

      if (window.SignaSenseAndroid && window.SignaSenseAndroid.speak) {
        window.SignaSenseAndroid.speak(message);
        return;
      }

      if ("speechSynthesis" in window) {
        window.speechSynthesis.cancel();
        const utterance = new SpeechSynthesisUtterance(message);
        utterance.rate = 0.95;
        utterance.pitch = 1.0;
        window.speechSynthesis.speak(utterance);
      }
    }

    function speakNow(kind) {
      if (!latest) return;
      if (kind === "letter") speak("Letter " + latest.caption);
      if (kind === "word") speak(latest.word || "No word yet.");
      if (kind === "sentence") speak(latest.sentence || latest.word || "No caption yet.");
    }

    async function sendAction(cmd) {
      await fetch("/action?cmd=" + encodeURIComponent(cmd), { cache: "no-store" });
      await poll();
    }

    function render(data) {
      latest = data;
      text("letter", data.caption || "...");
      text("word", data.word || "...");
      text("sentence", data.sentence || "...");
      text("letterNote", data.tipText || "");
      text("statusLine", data.statusText || "Ready");

      const active = data.activeCount || 0;
      const required = data.requiredCount || 5;
      if (active >= required) {
        text("sensorBadge", "Sensors ready");
        document.getElementById("sensorBadge").className = "badge good";
      } else if (active > 0) {
        text("sensorBadge", "Partial signal");
        document.getElementById("sensorBadge").className = "badge warn";
      } else {
        text("sensorBadge", "No signal");
        document.getElementById("sensorBadge").className = "badge bad";
      }

      for (let i = 0; i < names.length; i++) {
        const raw = data.raw ? data.raw[i] : 0;
        const bend = data.bend ? data.bend[i] : 0;
        const state = data.state ? data.state[i] : "?";
        const signal = data.signal ? data.signal[i] : false;
        text("state" + i, signal ? state : "No signal");
        document.getElementById("fill" + i).style.width = Math.max(0, Math.min(100, bend)) + "%";
        document.getElementById("raw" + i).innerHTML =
          "GPIO" + data.pin[i] + "<br>Raw: " + raw + "<br>Bend: " + bend + "%";
      }

      if (audioEnabled && data.caption && data.caption.length === 1 && data.caption !== lastSpokenLetter) {
        lastSpokenLetter = data.caption;
        speak("Letter " + data.caption);
      }
    }

    async function poll() {
      try {
        const response = await fetch("/data", { cache: "no-store" });
        const data = await response.json();
        render(data);
      } catch (error) {
        text("statusLine", "Waiting for the ESP32 glove page...");
      }
    }

    setInterval(poll, 250);
    poll();
  </script>
</body>
</html>
)rawliteral";

// ---------------------------------------------------------------------------
// Arduino lifecycle.
// ---------------------------------------------------------------------------

void setup() {
  Serial.begin(115200);
  delay(500);

  setupSensors();
  setupBle();
  resetCalibration();

  Serial.println();
  Serial.println("SignaSense ESP32 Smart Glove BLE starting...");
  Serial.println("Pin map: Thumb=GPIO25, Index=GPIO33, Middle=GPIO32, Ring=GPIO35, Pinky=GPIO34");
  Serial.println("Wi-Fi AP is disabled. Live glove data is sent over BLE.");
  Serial.print("BLE name: ");
  Serial.println(BLE_DEVICE_NAME);
}

void loop() {
  const unsigned long nowMs = millis();

  if (nowMs - lastSensorReadMs >= SENSOR_INTERVAL_MS) {
    lastSensorReadMs = nowMs;
    readSensors();
    currentLetterIndex = detectLetter();
    updateWordBuilder(currentLetterIndex, nowMs);
  }

  if (nowMs - lastSerialPrintMs >= SERIAL_INTERVAL_MS) {
    lastSerialPrintMs = nowMs;
    printSerialStatus();
  }

  if (bleClientConnected && nowMs - lastBleNotifyMs >= BLE_NOTIFY_INTERVAL_MS) {
    lastBleNotifyMs = nowMs;
    notifyStatus();
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

void setupSensors() {
  analogReadResolution(12);

  for (byte i = 0; i < NUM_FINGERS; i++) {
    pinMode(FINGER_PINS[i], INPUT);
    analogSetPinAttenuation(FINGER_PINS[i], ADC_11db);
  }
}

void setupWiFiPage() {
  WiFi.mode(WIFI_AP);
  WiFi.setSleep(false);
  WiFi.softAPConfig(AP_IP, AP_GATEWAY, AP_SUBNET);
  WiFi.softAP(WIFI_AP_NAME, WIFI_AP_PASSWORD);

  server.on("/", handleRoot);
  server.on("/data", handleData);
  server.on("/action", handleAction);
  server.begin();
}

void setupBle() {
  BLEDevice::init(BLE_DEVICE_NAME);

  bleServer = BLEDevice::createServer();
  bleServer->setCallbacks(new ServerCallbacks());

  BLEService *service = bleServer->createService(SERVICE_UUID);
  dataCharacteristic = service->createCharacteristic(
    DATA_CHAR_UUID,
    BLECharacteristic::PROPERTY_READ |
      BLECharacteristic::PROPERTY_NOTIFY |
      BLECharacteristic::PROPERTY_WRITE
  );

  dataCharacteristic->addDescriptor(new BLE2902());
  dataCharacteristic->setCallbacks(new DataCallbacks());
  dataCharacteristic->setValue("{\"ok\":true,\"status\":\"Starting glove\"}");

  service->start();

  BLEAdvertising *advertising = BLEDevice::getAdvertising();
  advertising->addServiceUUID(SERVICE_UUID);
  advertising->setScanResponse(true);
  advertising->setMinPreferred(0x06);
  advertising->setMinPreferred(0x12);
  BLEDevice::startAdvertising();
}

void notifyStatus() {
  if (dataCharacteristic == nullptr) {
    return;
  }

  const String json = buildStatusJson();
  lastBleJsonLength = json.length();
  dataCharacteristic->setValue((uint8_t *)json.c_str(), json.length());
  dataCharacteristic->notify();
}

// ---------------------------------------------------------------------------
// Sensor logic.
// ---------------------------------------------------------------------------

void readSensors() {
  for (byte finger = 0; finger < NUM_FINGERS; finger++) {
    rawValues[finger] = analogRead(FINGER_PINS[finger]);
    sensorHasSignal[finger] = rawHasSignal(rawValues[finger]);

    if (!sensorHasSignal[finger]) {
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

bool rawHasSignal(int rawValue) {
  return rawValue > RAW_LOW_NO_SIGNAL && rawValue < RAW_HIGH_NO_SIGNAL;
}

void updateDynamicCalibration(byte finger) {
  const int roundedValue = (int)(smoothedValues[finger] + 0.5f);

  if (!baselineReady[finger]) {
    baselineValues[finger] = smoothedValues[finger];
    maxObservedDelta[finger] = MIN_USEFUL_BEND_DELTA;
    observedMinValues[finger] = roundedValue;
    observedMaxValues[finger] = roundedValue;
    baselineReady[finger] = true;
    return;
  }

  if (roundedValue < observedMinValues[finger]) {
    observedMinValues[finger] = roundedValue;
  }

  if (roundedValue > observedMaxValues[finger]) {
    observedMaxValues[finger] = roundedValue;
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

// ---------------------------------------------------------------------------
// Letter recognition.
// ---------------------------------------------------------------------------

int detectLetter() {
  int bestIndex = LETTER_UNKNOWN;
  int bestScore = -1;

  for (int i = 0; i < NUM_LETTERS; i++) {
    int considered = 0;
    int score = 0;
    bool failed = false;

    for (byte finger = 0; finger < NUM_FINGERS; finger++) {
      if (!sensorHasSignal[finger]) {
        if (OPTIONAL_FOR_RECOGNITION[finger]) {
          continue;
        }
        failed = true;
        break;
      }

      considered++;
      if (stateMatches(fingerStates[finger], LETTER_RULES[i].match[finger])) {
        score++;
      }
    }

    if (failed || considered < MIN_REQUIRED_ACTIVE_FINGERS || score != considered) {
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

// ---------------------------------------------------------------------------
// Word and caption builder.
// ---------------------------------------------------------------------------

void updateWordBuilder(int detectedLetterIndex, unsigned long nowMs) {
  if (detectedLetterIndex == LETTER_UNKNOWN) {
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

  if (stableLetterIndex != detectedLetterIndex) {
    stableLetterIndex = detectedLetterIndex;
    lastLetterAppendMs = nowMs;
    appendLetterToWord(detectedLetterIndex);
    return;
  }

  if (nowMs - lastLetterAppendMs >= SAME_LETTER_REPEAT_MS) {
    lastLetterAppendMs = nowMs;
    appendLetterToWord(detectedLetterIndex);
  }
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
}

void commitCurrentWordToSentence() {
  if (currentWord.length() == 0) {
    return;
  }

  lastCommittedWord = currentWord;

  if (currentSentence.length() > 0) {
    currentSentence += ' ';
  }

  currentSentence += currentWord;
  trimSentenceText();
  currentWord = "";
  wordCommitCounter++;
  candidateLetterIndex = LETTER_UNKNOWN;
  stableLetterIndex = LETTER_UNKNOWN;
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
}

void resetCalibration() {
  smoothingReady = false;
  currentLetterIndex = LETTER_UNKNOWN;
  candidateLetterIndex = LETTER_UNKNOWN;
  stableLetterIndex = LETTER_UNKNOWN;
  candidateLetterSinceMs = millis();
  lastLetterAppendMs = 0;

  for (byte i = 0; i < NUM_FINGERS; i++) {
    rawValues[i] = 0;
    smoothedValues[i] = 0.0f;
    baselineValues[i] = 0.0f;
    maxObservedDelta[i] = 0.0f;
    observedMinValues[i] = 4095;
    observedMaxValues[i] = 0;
    bendPercent[i] = 0;
    fingerStates[i] = STATE_NO_SIGNAL;
    sensorHasSignal[i] = false;
    baselineReady[i] = false;
  }
}

// ---------------------------------------------------------------------------
// Web handlers.
// ---------------------------------------------------------------------------

void handleRoot() {
  server.send_P(200, "text/html", INDEX_HTML);
}

String buildStatusJson() {
  String json = "{";

  json += "\"ok\":true";
  json += ",\"caption\":";
  appendJsonString(json, captionText(currentLetterIndex));
  json += ",\"stableLetter\":";
  if (stableLetterIndex == LETTER_UNKNOWN) {
    appendJsonString(json, "");
  } else {
    appendJsonString(json, captionText(stableLetterIndex));
  }
  json += ",\"pendingLetter\":\"\"";
  json += ",\"acceptedLetter\":\"\"";
  json += ",\"acceptedLetterCounter\":0";
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
  json += ",\"calibrated\":true";
  json += ",\"calibrating\":false";
  json += ",\"activeCount\":";
  json += activeSensorCount();
  json += ",\"requiredCount\":";
  json += requiredSensorCount();

  json += ",\"pins\":";
  appendIntArrayObject(json, FINGER_PINS, NUM_FINGERS);
  json += ",\"raw\":";
  appendIntArrayObject(json, rawValues, NUM_FINGERS);
  json += ",\"bend\":";
  appendIntArrayObject(json, bendPercent, NUM_FINGERS);
  json += ",\"signal\":";
  appendBoolArrayObject(json, sensorHasSignal, NUM_FINGERS);

  json += ",\"state\":[";
  for (byte i = 0; i < NUM_FINGERS; i++) {
    appendJsonString(json, fingerStateText(fingerStates[i]));
    if (i < NUM_FINGERS - 1) {
      json += ",";
    }
  }
  json += "]";

  json += "}";

  return json;
}

void handleData() {
  const String json = buildStatusJson();

  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", json);
}

void handleAction() {
  if (!server.hasArg("cmd")) {
    server.send(400, "text/plain", "Missing cmd");
    return;
  }

  const String command = server.arg("cmd");
  handleCommand(command);

  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "text/plain", "OK");
}

void handleCommand(const String &command) {
  if (command == "clear" || command == "clearword") {
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
    resetCalibration();
  }
}

// ---------------------------------------------------------------------------
// JSON helpers.
// ---------------------------------------------------------------------------

void appendIntArrayObject(String &json, const int values[], byte count) {
  json += "[";
  for (byte i = 0; i < count; i++) {
    json += values[i];
    if (i < count - 1) {
      json += ",";
    }
  }
  json += "]";
}

void appendBoolArrayObject(String &json, const bool values[], byte count) {
  json += "[";
  for (byte i = 0; i < count; i++) {
    json += values[i] ? "true" : "false";
    if (i < count - 1) {
      json += ",";
    }
  }
  json += "]";
}

void appendBaselineArray(String &json) {
  json += "[";
  for (byte i = 0; i < NUM_FINGERS; i++) {
    json += baselineReady[i] ? (int)(baselineValues[i] + 0.5f) : 0;
    if (i < NUM_FINGERS - 1) {
      json += ",";
    }
  }
  json += "]";
}

void appendObservedArray(String &json, bool useMin) {
  json += "[";
  for (byte i = 0; i < NUM_FINGERS; i++) {
    if (!baselineReady[i]) {
      json += 0;
    } else {
      json += useMin ? observedMinValues[i] : observedMaxValues[i];
    }

    if (i < NUM_FINGERS - 1) {
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
      } else if (c == '\n') {
        json += "\\n";
      } else if (c != '\r') {
        json += c;
      }
    }
  }
  json += "\"";
}

void appendJsonString(String &json, const String &value) {
  appendJsonString(json, value.c_str());
}

// ---------------------------------------------------------------------------
// Status and serial helpers.
// ---------------------------------------------------------------------------

byte activeSensorCount() {
  byte count = 0;

  for (byte i = 0; i < NUM_FINGERS; i++) {
    if (OPTIONAL_FOR_RECOGNITION[i]) {
      continue;
    }

    if (sensorHasSignal[i]) {
      count++;
    }
  }

  return count;
}

byte requiredSensorCount() {
  return NUM_FINGERS - 1;
}

String statusText() {
  const byte active = activeSensorCount();
  const byte required = requiredSensorCount();

  if (active == 0) {
    return "Waiting for finger readings. Move your hand slowly or press Calibrate.";
  }

  if (active < required) {
    return "Some fingers are active. Keep your hand steady while the glove reads the sign.";
  }

  if (!sensorHasSignal[FINGER_THUMB]) {
    return "Main finger readings are ready. Continue signing.";
  }

  if (currentLetterIndex == LETTER_UNKNOWN) {
    return "Sensors ready. Hold one bend pattern steady to form a letter.";
  }

  return "Letter ready.";
}

String tipText() {
  if (!sensorHasSignal[FINGER_THUMB]) {
    return "Keep your hand relaxed, then hold the sign clearly.";
  }

  if (currentLetterIndex == LETTER_UNKNOWN) {
    return "Hold the same shape for about one second. Tap Recalibrate if the glove was not straight at startup.";
  }

  return "Stable letters enter the word automatically. Use Commit Word for captions.";
}

void printSerialStatus() {
  Serial.print("Pins T/I/M/R/P: 25/33/32/35/34 | Raw: ");
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
  Serial.print(" | Word: ");
  Serial.print(currentWord);
  Serial.print(" | Sentence: ");
  Serial.print(currentSentence);
  Serial.print(" | BLE: ");
  Serial.print(bleClientConnected ? "connected" : "advertising");
  Serial.print(" | JSON bytes: ");
  Serial.println(lastBleJsonLength);
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
