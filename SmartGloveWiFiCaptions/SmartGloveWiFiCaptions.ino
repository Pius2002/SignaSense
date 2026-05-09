/*
  ESP32 Smart Glove - Local Wi-Fi Voice Site

  This version does not use Bluetooth and does not need mobile data/internet.
  The ESP32 creates its own Wi-Fi network and hosts a local web page.

  Phone use:
  1. Power the ESP32.
  2. On the phone, connect to Wi-Fi network: SmartGlove
  3. Password: 12345678
  4. Open browser: http://192.168.4.1

  Local audio note:
  - The web page uses the browser's local speech engine through the Web Speech
    API. That means the sound comes from the phone/laptop speaker, not from the
    ESP32 itself.
  - No phone app is required.
  - No internet is required if the browser already has a local voice installed.
  - Some phones need one user tap on "Enable Audio" before speech is allowed.

  Important ADC note:
  ESP32 Wi-Fi uses ADC2 internally, so flex sensors should be on ADC1 pins only.
  Do not use ADC2 pins like GPIO12, GPIO13, GPIO14, GPIO25, GPIO26, or GPIO27
  for flex sensors in this Wi-Fi version.

  Current configured pin summary:
  - Thumb  flex sensor output -> GPIO32
  - Index  flex sensor output -> GPIO33
  - Middle flex sensor output -> GPIO34
  - Ring   flex sensor output -> GPIO35
  - Pinky  flex sensor output -> GPIO25

  Limitation:
  - GPIO25 is ADC2 on ESP32.
  - Official Espressif docs state ADC2 cannot be read reliably while Wi-Fi is
    running on ESP32.
  - So this Wi-Fi site build can honor the pin map in code, but the pinky on
    GPIO25 may still show no live reading until that sensor is moved to ADC1.

  Current recognition mode:
  - Pinky stays on GPIO25 in code and diagnostics.
  - Pinky is ignored for letter recognition and readiness checks.
  - The active recognition set is Thumb, Index, Middle, and Ring.

  Wiring per flex sensor:
  - One end of flex sensor -> 3.3V
  - Other end of flex sensor -> analog GPIO signal pin and one end of resistor
  - Other end of resistor -> GND
  - ESP32 GND shared with all sensor grounds

  Letter recognition:
  This is a one-hand bend-only approximation. Five flex sensors can detect
  straight, half-bent, or bent finger states, but cannot detect palm
  orientation, finger spread, fingertip contact, or motion.

  For that reason, this sketch outputs only letters that are reasonably
  distinguishable from bend patterns:
  A, B, C, D, E, F, I, L, V, W, X, Y.

  Calibration:
  - Dynamic calibration is used.
  - The first valid reading from each sensor becomes its straight baseline.
  - Bend is measured as movement away from that baseline, so it still works if
    your divider makes the raw value rise or fall when the finger bends.
  - If a pin reads near 0 or 4095, the sketch marks that finger as "no signal".
*/

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>

// ---------------------------------------------------------------------------
// Local Wi-Fi page settings
// ---------------------------------------------------------------------------

const char WIFI_AP_NAME[] = "SmartGlove";
const char WIFI_AP_PASSWORD[] = "12345678"; // Must be 8+ characters
const IPAddress AP_IP(192, 168, 4, 1);
const IPAddress AP_GATEWAY(192, 168, 4, 1);
const IPAddress AP_SUBNET(255, 255, 255, 0);

WebServer server(80);

// ---------------------------------------------------------------------------
// Finger pin mapping - ADC1 only for Wi-Fi compatibility
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
  "Thumb", "Index", "Middle", "Ring", "Pinky"
};

const int DEFAULT_FINGER_PINS[NUM_FINGERS] = {
  32, // Thumb  - ADC1
  33, // Index  - ADC1
  34, // Middle - ADC1 input-only
  35, // Ring   - ADC1 input-only
  25  // Pinky  - ADC2, not reliable while Wi-Fi is active
};

const int ADC1_CANDIDATE_PINS[] = {
  32, 33, 34, 35, 36, 39
};

const byte NUM_ADC1_CANDIDATES = sizeof(ADC1_CANDIDATE_PINS) / sizeof(ADC1_CANDIDATE_PINS[0]);

// ---------------------------------------------------------------------------
// Calibration and classification
// ---------------------------------------------------------------------------

const int STRAIGHT_MAX_PERCENT = 30;
const int BENT_MIN_PERCENT = 70;
const float SMOOTHING_ALPHA = 0.22f;
const unsigned long SENSOR_READ_INTERVAL_MS = 25;
const unsigned long SERIAL_PRINT_INTERVAL_MS = 500;
const unsigned long LETTER_STABLE_MS = 850;
const unsigned long SAME_LETTER_REPEAT_MS = 3000;

const int RAW_LOW_NO_SIGNAL = 2;
const int RAW_HIGH_NO_SIGNAL = 4093;

const float DEFAULT_FULL_BEND_DELTA = 900.0f;
const float MIN_USEFUL_BEND_DELTA = 80.0f;
const int MAX_WORD_LENGTH = 32;
const int MAX_SENTENCE_LENGTH = 140;
const bool IGNORE_PINKY_IN_RECOGNITION = true;

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
  const char *letterName;
  const char *caption;
  FingerMatch match[NUM_FINGERS];
};

const LetterRule LETTERS[] = {
  {
    "LETTER_A",
    "A",
    {MATCH_STRAIGHT_OR_HALF, MATCH_BENT, MATCH_BENT, MATCH_BENT, MATCH_BENT}
  },
  {
    "LETTER_B",
    "B",
    {MATCH_HALF_OR_BENT, MATCH_STRAIGHT, MATCH_STRAIGHT, MATCH_STRAIGHT, MATCH_STRAIGHT}
  },
  {
    "LETTER_C",
    "C",
    {MATCH_HALF_BENT, MATCH_HALF_BENT, MATCH_HALF_BENT, MATCH_HALF_BENT, MATCH_HALF_BENT}
  },
  {
    "LETTER_D",
    "D",
    {MATCH_HALF_OR_BENT, MATCH_STRAIGHT_OR_HALF, MATCH_HALF_OR_BENT, MATCH_HALF_OR_BENT, MATCH_HALF_OR_BENT}
  },
  {
    "LETTER_E",
    "E",
    {MATCH_BENT, MATCH_BENT, MATCH_BENT, MATCH_BENT, MATCH_BENT}
  },
  {
    "LETTER_F",
    "F",
    {MATCH_HALF_OR_BENT, MATCH_HALF_OR_BENT, MATCH_STRAIGHT_OR_HALF, MATCH_STRAIGHT_OR_HALF, MATCH_STRAIGHT_OR_HALF}
  },
  {
    "LETTER_I",
    "I",
    {MATCH_HALF_OR_BENT, MATCH_HALF_OR_BENT, MATCH_HALF_OR_BENT, MATCH_HALF_OR_BENT, MATCH_STRAIGHT_OR_HALF}
  },
  {
    "LETTER_L",
    "L",
    {MATCH_STRAIGHT_OR_HALF, MATCH_STRAIGHT_OR_HALF, MATCH_HALF_OR_BENT, MATCH_HALF_OR_BENT, MATCH_HALF_OR_BENT}
  },
  {
    "LETTER_V",
    "V",
    {MATCH_HALF_OR_BENT, MATCH_STRAIGHT_OR_HALF, MATCH_STRAIGHT_OR_HALF, MATCH_HALF_OR_BENT, MATCH_HALF_OR_BENT}
  },
  {
    "LETTER_W",
    "W",
    {MATCH_HALF_OR_BENT, MATCH_STRAIGHT_OR_HALF, MATCH_STRAIGHT_OR_HALF, MATCH_STRAIGHT_OR_HALF, MATCH_HALF_OR_BENT}
  },
  {
    "LETTER_X",
    "X",
    {MATCH_HALF_OR_BENT, MATCH_HALF_BENT, MATCH_HALF_OR_BENT, MATCH_HALF_OR_BENT, MATCH_HALF_OR_BENT}
  },
  {
    "LETTER_Y",
    "Y",
    {MATCH_STRAIGHT_OR_HALF, MATCH_BENT, MATCH_BENT, MATCH_BENT, MATCH_STRAIGHT_OR_HALF}
  }
};

const int NUM_LETTERS = sizeof(LETTERS) / sizeof(LETTERS[0]);
const int LETTER_UNKNOWN = -1;

// ---------------------------------------------------------------------------
// Runtime state
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
int fingerPins[NUM_FINGERS] = {32, 33, 34, 35, 36};
bool fingerPinAutoMapped[NUM_FINGERS] = {false, false, false, false, false};
bool fingerPinLikelyConnected[NUM_FINGERS] = {false, false, false, false, false};
int candidatePinAverage[NUM_ADC1_CANDIDATES] = {0, 0, 0, 0, 0, 0};
int candidatePinMin[NUM_ADC1_CANDIDATES] = {0, 0, 0, 0, 0, 0};
int candidatePinMax[NUM_ADC1_CANDIDATES] = {0, 0, 0, 0, 0, 0};
byte connectedCandidateCount = 0;

bool smoothingReady = false;
unsigned long lastSensorReadMs = 0;
unsigned long lastSerialPrintMs = 0;
int currentLetterIndex = LETTER_UNKNOWN;
int candidateLetterIndex = LETTER_UNKNOWN;
int stableLetterIndex = LETTER_UNKNOWN;
unsigned long candidateLetterSinceMs = 0;
unsigned long lastLetterAppendMs = 0;
String currentWord = "";
String currentSentence = "";

// ---------------------------------------------------------------------------
// HTML page
// ---------------------------------------------------------------------------

const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!doctype html>
<html>
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <title>Smart Glove Voice Console</title>
  <style>
    :root {
      color-scheme: light;
      font-family: Arial, sans-serif;
      --bg: #f4f6f7;
      --panel: #ffffff;
      --line: #d8dde3;
      --text: #1e2227;
      --muted: #5b6168;
      --good: #1d7a46;
      --warn: #c96b1a;
      --bad: #b13a33;
      --accent: #0f766e;
      --accent-soft: #e6f5f3;
      --ink-soft: #eff2f4;
    }

    * { box-sizing: border-box; }

    body {
      margin: 0;
      background: var(--bg);
      color: var(--text);
    }

    header {
      background: var(--panel);
      border-bottom: 1px solid var(--line);
    }

    .shell {
      max-width: 1120px;
      margin: 0 auto;
      padding: 18px;
    }

    .headline {
      display: flex;
      justify-content: space-between;
      gap: 14px;
      align-items: flex-start;
      flex-wrap: wrap;
    }

    h1 {
      margin: 0;
      font-size: 30px;
      line-height: 1.1;
    }

    .subline {
      margin: 8px 0 0;
      color: var(--muted);
      font-size: 15px;
      line-height: 1.5;
    }

    .badge-row {
      display: flex;
      gap: 8px;
      flex-wrap: wrap;
      justify-content: flex-end;
    }

    .badge {
      padding: 9px 12px;
      border-radius: 6px;
      font-size: 13px;
      font-weight: 700;
      background: var(--ink-soft);
      color: var(--text);
      white-space: nowrap;
    }

    .badge.good {
      background: #e7f5ec;
      color: var(--good);
    }

    .badge.warn {
      background: #fff3e8;
      color: var(--warn);
    }

    .badge.bad {
      background: #f9e7e6;
      color: var(--bad);
    }

    main {
      max-width: 1120px;
      margin: 0 auto;
      padding: 0 18px 28px;
    }

    section {
      padding: 18px 0;
      border-bottom: 1px solid var(--line);
    }

    section:last-child {
      border-bottom: 0;
    }

    .section-title {
      margin: 0 0 12px;
      font-size: 18px;
    }

    .section-note {
      margin: 0 0 16px;
      color: var(--muted);
      font-size: 14px;
      line-height: 1.5;
    }

    .live-grid {
      display: grid;
      grid-template-columns: repeat(3, minmax(0, 1fr));
      gap: 12px;
    }

    .metric {
      background: var(--panel);
      border: 1px solid var(--line);
      border-radius: 6px;
      min-height: 154px;
      padding: 16px;
      display: flex;
      flex-direction: column;
      justify-content: space-between;
      gap: 12px;
    }

    .metric-label {
      font-size: 12px;
      color: var(--muted);
      font-weight: 700;
      letter-spacing: 0.04em;
      text-transform: uppercase;
    }

    .metric-value {
      font-size: 52px;
      font-weight: 700;
      line-height: 1;
      overflow-wrap: anywhere;
    }

    .metric-value.word {
      font-size: 34px;
      line-height: 1.15;
    }

    .metric-value.sentence {
      font-size: 26px;
      line-height: 1.25;
      min-height: 64px;
    }

    .metric-foot {
      font-size: 14px;
      color: var(--muted);
      line-height: 1.45;
    }

    .audio-grid,
    .control-grid {
      display: grid;
      grid-template-columns: repeat(2, minmax(0, 1fr));
      gap: 18px;
    }

    .stack {
      display: grid;
      gap: 12px;
      align-content: start;
    }

    .control-line {
      display: grid;
      gap: 6px;
    }

    label {
      font-size: 14px;
      font-weight: 700;
    }

    select,
    input[type="range"] {
      width: 100%;
    }

    input[type="range"] {
      accent-color: var(--accent);
    }

    .check-row {
      display: flex;
      flex-wrap: wrap;
      gap: 14px;
      align-items: center;
    }

    .check {
      display: inline-flex;
      gap: 8px;
      align-items: center;
      font-size: 14px;
      color: var(--text);
    }

    .button-row {
      display: flex;
      flex-wrap: wrap;
      gap: 8px;
    }

    button {
      border: 0;
      border-radius: 6px;
      padding: 10px 14px;
      font-size: 14px;
      font-weight: 700;
      color: white;
      background: var(--accent);
    }

    button.alt {
      background: #6b7280;
    }

    button.warn {
      background: var(--warn);
    }

    button.bad {
      background: var(--bad);
    }

    .status-line {
      margin: 0;
      font-size: 14px;
      color: var(--muted);
      line-height: 1.5;
    }

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
      min-height: 152px;
      display: grid;
      gap: 10px;
      align-content: start;
    }

    .finger-top {
      display: flex;
      justify-content: space-between;
      gap: 8px;
      align-items: baseline;
    }

    .finger-name {
      font-size: 15px;
      font-weight: 700;
    }

    .finger-state {
      font-size: 13px;
      color: var(--muted);
      text-align: right;
    }

    .finger-state.warn {
      color: var(--bad);
      font-weight: 700;
    }

    .bar {
      width: 100%;
      height: 12px;
      border-radius: 6px;
      background: #e5eaee;
      overflow: hidden;
    }

    .fill {
      height: 100%;
      width: 0;
      background: linear-gradient(90deg, #0f766e 0%, #d97706 100%);
    }

    .finger-raw {
      font-size: 13px;
      color: var(--muted);
      line-height: 1.45;
      overflow-wrap: anywhere;
    }

    .hint-band {
      background: var(--accent-soft);
      border: 1px solid #b7e0da;
      border-radius: 6px;
      padding: 14px;
      font-size: 14px;
      line-height: 1.6;
    }

    @media (max-width: 900px) {
      .live-grid,
      .audio-grid,
      .control-grid,
      .fingers {
        grid-template-columns: repeat(2, minmax(0, 1fr));
      }

      .metric-value {
        font-size: 40px;
      }

      .metric-value.word {
        font-size: 30px;
      }

      .metric-value.sentence {
        font-size: 22px;
      }
    }

    @media (max-width: 640px) {
      .live-grid,
      .audio-grid,
      .control-grid,
      .fingers {
        grid-template-columns: 1fr;
      }

      h1 {
        font-size: 26px;
      }

      .metric {
        min-height: 132px;
      }

      .metric-value {
        font-size: 34px;
      }

      .metric-value.word {
        font-size: 28px;
      }

      .metric-value.sentence {
        font-size: 20px;
      }
    }
  </style>
</head>
<body>
  <header>
    <div class="shell">
      <div class="headline">
        <div>
          <h1>Smart Glove Voice Console</h1>
          <p class="subline">
            Live letters, live word building, local browser speech, and full sensor diagnostics on
            the glove's own Wi-Fi page.
          </p>
        </div>
        <div class="badge-row">
          <div id="sensorBadge" class="badge warn">Checking sensors</div>
          <div id="audioBadge" class="badge">Audio locked</div>
          <div class="badge">Wi-Fi: SmartGlove</div>
          <div class="badge">Open: 192.168.4.1</div>
        </div>
      </div>
    </div>
  </header>

  <main>
    <section>
      <h2 class="section-title">Live Output</h2>
      <p id="metaLine" class="section-note">Connecting to glove...</p>
      <div class="live-grid">
        <article class="metric">
          <div class="metric-label">Current Letter</div>
          <div id="caption" class="metric-value">Waiting...</div>
          <div id="captionNote" class="metric-foot">Hold a finger shape steady to lock a letter.</div>
        </article>
        <article class="metric">
          <div class="metric-label">Live Word</div>
          <div id="word" class="metric-value word">...</div>
          <div class="metric-foot">Stable letters are added here automatically.</div>
        </article>
        <article class="metric">
          <div class="metric-label">Sentence Line</div>
          <div id="sentence" class="metric-value sentence">...</div>
          <div class="metric-foot">Use Commit Word to move the live word into the sentence line.</div>
        </article>
      </div>
    </section>

    <section>
      <div class="audio-grid">
        <div class="stack">
          <h2 class="section-title">Audio</h2>
          <p class="section-note">
            The browser speaks through this phone or laptop. Tap Enable Audio once, then choose how much
            the page should say automatically.
          </p>
          <div class="button-row">
            <button onclick="enableAudio()">Enable Audio</button>
            <button class="alt" onclick="speakLetterNow()">Speak Letter</button>
            <button class="alt" onclick="speakWordNow()">Speak Word</button>
            <button class="alt" onclick="speakSentenceNow()">Speak Sentence</button>
            <button class="warn" onclick="testAudio()">Test Voice</button>
          </div>
          <p id="audioStatus" class="status-line">Audio is waiting for a user tap.</p>
          <div class="check-row">
            <label class="check"><input id="autoLetter" type="checkbox" checked> Auto letter</label>
            <label class="check"><input id="autoWord" type="checkbox"> Auto word</label>
            <label class="check"><input id="autoSentence" type="checkbox"> Auto sentence</label>
          </div>
        </div>

        <div class="stack">
          <h2 class="section-title">Voice Controls</h2>
          <div class="control-line">
            <label for="voiceSelect">Voice</label>
            <select id="voiceSelect"></select>
          </div>
          <div class="control-line">
            <label for="rateRange">Rate</label>
            <input id="rateRange" type="range" min="0.6" max="1.4" step="0.05" value="1.0">
          </div>
          <div class="control-line">
            <label for="pitchRange">Pitch</label>
            <input id="pitchRange" type="range" min="0.7" max="1.5" step="0.05" value="1.0">
          </div>
          <div class="control-line">
            <label for="volumeRange">Volume</label>
            <input id="volumeRange" type="range" min="0.1" max="1.0" step="0.05" value="1.0">
          </div>
        </div>
      </div>
    </section>

    <section>
      <div class="control-grid">
        <div class="stack">
          <h2 class="section-title">Text Controls</h2>
          <div class="button-row">
            <button onclick="sendAction('space')">Commit Word</button>
            <button class="alt" onclick="sendAction('backspace')">Backspace</button>
            <button class="alt" onclick="sendAction('clear')">Clear Word</button>
            <button class="bad" onclick="sendAction('clearsentence')">Clear Sentence</button>
          </div>
        </div>
        <div class="stack">
          <h2 class="section-title">System</h2>
          <div class="button-row">
            <button class="warn" onclick="sendAction('recalibrate')">Recalibrate Sensors</button>
            <button class="alt" onclick="poll()">Refresh Now</button>
          </div>
          <p id="tipLine" class="status-line">Waiting for glove tips...</p>
        </div>
      </div>
    </section>

    <section>
      <h2 class="section-title">Finger Diagnostics</h2>
      <p class="section-note">
        These bars show the live bend estimate for each finger. Pinky stays visible for diagnostics on
        GPIO25, but recognition uses Thumb, Index, Middle, and Ring only.
      </p>
      <div id="fingers" class="fingers"></div>
    </section>

    <section>
      <div class="hint-band">
        This page stays local to the ESP32. For full five-finger Wi-Fi reading on ESP32, use ADC1 pins
        32, 33, 34, 35, and 36 only.
      </div>
    </section>
  </main>

  <script>
    const fingerNames = ["Thumb", "Index", "Middle", "Ring", "Pinky"];
    let latestData = null;
    let audioReady = false;
    let voicesLoaded = false;
    let voiceCache = [];
    let lastSpokenLetter = "";
    let lastSpokenWord = "";
    let lastSpokenSentence = "";
    let audioContext = null;

    function byId(id) {
      return document.getElementById(id);
    }

    function stateName(shortName) {
      if (shortName === "N") return "no signal";
      if (shortName === "S") return "straight";
      if (shortName === "H") return "half-bent";
      if (shortName === "B") return "bent";
      return "unknown";
    }

    function escapeHtml(value) {
      return String(value)
        .replaceAll("&", "&amp;")
        .replaceAll("<", "&lt;")
        .replaceAll(">", "&gt;");
    }

    function trimText(value, fallback) {
      const text = (value || "").trim();
      return text.length ? text : fallback;
    }

    function preferredVoice(voices) {
      for (const voice of voices) {
        const name = voice.name.toLowerCase();
        const lang = voice.lang.toLowerCase();
        if (lang.startsWith("en") && name.includes("google")) return voice;
      }

      for (const voice of voices) {
        if (voice.lang.toLowerCase().startsWith("en")) return voice;
      }

      return voices.length ? voices[0] : null;
    }

    function refreshVoiceList() {
      if (!("speechSynthesis" in window)) {
        byId("audioStatus").textContent = "Speech synthesis is not available in this browser.";
        return;
      }

      const voices = window.speechSynthesis.getVoices();
      if (!voices.length) {
        return;
      }

      voiceCache = voices.slice();
      voicesLoaded = true;

      const select = byId("voiceSelect");
      const oldValue = select.value;
      select.innerHTML = "";

      voices.forEach((voice) => {
        const option = document.createElement("option");
        option.value = voice.name;
        option.textContent = voice.name + " (" + voice.lang + ")";
        select.appendChild(option);
      });

      if (oldValue && voices.some((voice) => voice.name === oldValue)) {
        select.value = oldValue;
      } else {
        const best = preferredVoice(voices);
        if (best) {
          select.value = best.name;
        }
      }
    }

    function selectedVoice() {
      const selectedName = byId("voiceSelect").value;
      return voiceCache.find((voice) => voice.name === selectedName) || null;
    }

    function ensureAudioContext() {
      if (!audioContext) {
        const AudioCtor = window.AudioContext || window.webkitAudioContext;
        if (AudioCtor) {
          audioContext = new AudioCtor();
        }
      }

      if (audioContext && audioContext.state === "suspended") {
        audioContext.resume();
      }
    }

    function playTone(freq, durationSeconds) {
      try {
        ensureAudioContext();
        if (!audioContext) {
          return;
        }

        const oscillator = audioContext.createOscillator();
        const gain = audioContext.createGain();
        gain.gain.value = 0.03 + (parseFloat(byId("volumeRange").value) * 0.07);
        oscillator.type = "sine";
        oscillator.frequency.value = freq;
        oscillator.connect(gain);
        gain.connect(audioContext.destination);
        oscillator.start();
        oscillator.stop(audioContext.currentTime + durationSeconds);
      } catch (error) {
      }
    }

    function speakText(text) {
      if (!audioReady) {
        return;
      }

      const cleaned = trimText(text, "");
      if (!cleaned.length) {
        return;
      }

      if ("speechSynthesis" in window && voicesLoaded) {
        try {
          window.speechSynthesis.cancel();
          const utterance = new SpeechSynthesisUtterance(cleaned);
          const voice = selectedVoice();
          if (voice) {
            utterance.voice = voice;
            utterance.lang = voice.lang;
          }
          utterance.rate = parseFloat(byId("rateRange").value);
          utterance.pitch = parseFloat(byId("pitchRange").value);
          utterance.volume = parseFloat(byId("volumeRange").value);
          window.speechSynthesis.speak(utterance);
          byId("audioStatus").textContent = 'Speaking: "' + cleaned + '"';
          return;
        } catch (error) {
        }
      }

      playTone(720, 0.14);
      byId("audioStatus").textContent = "Speech voice unavailable. Tone fallback played.";
    }

    function enableAudio() {
      audioReady = true;
      ensureAudioContext();
      if ("speechSynthesis" in window) {
        refreshVoiceList();
        window.speechSynthesis.resume();
      }
      byId("audioBadge").textContent = "Audio enabled";
      byId("audioBadge").className = "badge good";
      byId("audioStatus").textContent = "Audio enabled on this device.";
      speakText("Smart glove audio ready.");
    }

    function testAudio() {
      speakText("Smart glove voice test.");
    }

    function speakLetterNow() {
      if (!latestData) return;
      if (latestData.caption === "No recognized letter") {
        speakText("No recognized letter.");
        return;
      }
      speakText("Letter " + latestData.caption);
    }

    function speakWordNow() {
      if (!latestData) return;
      const word = trimText(latestData.word, "");
      if (!word.length) {
        speakText("No word yet.");
        return;
      }
      speakText(word);
    }

    function speakSentenceNow() {
      if (!latestData) return;
      const sentence = trimText(latestData.sentence, "");
      if (!sentence.length) {
        speakText("No sentence yet.");
        return;
      }
      speakText(sentence);
    }

    function maybeSpeak(data) {
      if (!audioReady) {
        return;
      }

      if (byId("autoLetter").checked && data.caption !== "No recognized letter" && data.caption !== lastSpokenLetter) {
        lastSpokenLetter = data.caption;
        speakText("Letter " + data.caption);
        return;
      }

      const currentWord = trimText(data.word, "");
      if (byId("autoWord").checked && currentWord.length && currentWord !== lastSpokenWord) {
        lastSpokenWord = currentWord;
        speakText(currentWord);
        return;
      }

      const currentSentence = trimText(data.sentence, "");
      if (byId("autoSentence").checked && currentSentence.length && currentSentence !== lastSpokenSentence) {
        lastSpokenSentence = currentSentence;
        speakText(currentSentence);
      }
    }

    function renderFingerCards(data) {
      const wrap = byId("fingers");
      wrap.innerHTML = "";

      for (let i = 0; i < 5; i++) {
        const noSignal = !data.signal[i];
        const card = document.createElement("article");
        card.className = "finger";
        card.innerHTML = `
          <div class="finger-top">
            <div class="finger-name">${fingerNames[i]}</div>
            <div class="finger-state ${noSignal ? "warn" : ""}">${stateName(data.state[i])}</div>
          </div>
          <div class="bar">
            <div class="fill" style="width:${data.bend[i]}%"></div>
          </div>
          <div class="finger-raw">
            Pin: GPIO${data.pin[i]}${data.pinAuto[i] ? " auto" : ""}<br>
            Raw: ${data.raw[i]}<br>
            Bend: ${data.bend[i]}%<br>
            Baseline: ${data.baseline[i]}<br>
            Range: ${data.min[i]}-${data.max[i]}
          </div>
        `;
        wrap.appendChild(card);
      }
    }

    function render(data) {
      latestData = data;

      byId("caption").textContent = data.caption;
      byId("word").textContent = trimText(data.word, "...");
      byId("sentence").textContent = trimText(data.sentence, "...");
      byId("metaLine").textContent =
        "Detected: " + data.letter +
        " | Recognition sensors: " + data.activeCount + "/" + data.requiredCount +
        " | Total live pins: " + data.totalActiveCount + "/5" +
        " | Uptime: " + Math.floor(data.ms / 1000) + "s";
      byId("captionNote").textContent = data.statusText;
      byId("tipLine").textContent = data.tipText;

      const sensorBadge = byId("sensorBadge");
      sensorBadge.textContent = data.statusText;
      if (data.activeCount === data.requiredCount) {
        sensorBadge.className = "badge good";
      } else if (data.activeCount > 0) {
        sensorBadge.className = "badge warn";
      } else {
        sensorBadge.className = "badge bad";
      }

      renderFingerCards(data);
      maybeSpeak(data);
    }

    async function sendAction(action) {
      try {
        await fetch("/action?cmd=" + encodeURIComponent(action), { cache: "no-store" });

        if (action === "clear" || action === "backspace") {
          lastSpokenWord = "";
        }

        if (action === "clearsentence") {
          lastSpokenSentence = "";
        }

        if (action === "recalibrate") {
          lastSpokenLetter = "";
          lastSpokenWord = "";
          lastSpokenSentence = "";
        }

        await poll();
      } catch (error) {
        byId("metaLine").textContent = "Action failed. Retry when the glove is reachable.";
      }
    }

    async function poll() {
      try {
        const response = await fetch("/data", { cache: "no-store" });
        const data = await response.json();
        render(data);
      } catch (error) {
        byId("metaLine").textContent = "Waiting for ESP32...";
      }
    }

    if ("speechSynthesis" in window) {
      refreshVoiceList();
      window.speechSynthesis.onvoiceschanged = refreshVoiceList;
    } else {
      byId("audioStatus").textContent = "Speech synthesis is not available in this browser.";
    }

    setInterval(poll, 250);
    poll();
  </script>
</body>
</html>
)rawliteral";

// ---------------------------------------------------------------------------
// Arduino setup and loop
// ---------------------------------------------------------------------------

void setup() {
  Serial.begin(115200);
  delay(500);

  setupSensors();
  refreshFingerPinMapping();
  setupWiFiPage();

  Serial.println();
  Serial.println("ESP32 Smart Glove Wi-Fi Voice Site ready.");
  Serial.print("Connect phone to Wi-Fi: ");
  Serial.println(WIFI_AP_NAME);
  Serial.print("Password: ");
  Serial.println(WIFI_AP_PASSWORD);
  Serial.println("Open browser: http://192.168.4.1");
  Serial.println("Tap Enable Audio on the page to unlock browser speech.");
  Serial.print("Configured pin map: ");
  Serial.println(pinMapText());
  if (fingerPins[FINGER_PINKY] == 25) {
    Serial.println("Warning: Pinky is on GPIO25 (ADC2). Wi-Fi mode may leave it unreadable.");
  }
  Serial.println("This local page uses no mobile data or internet.");
}

void loop() {
  const unsigned long nowMs = millis();

  if (nowMs - lastSensorReadMs >= SENSOR_READ_INTERVAL_MS) {
    lastSensorReadMs = nowMs;
    readSensors();
    currentLetterIndex = detectLetter();
    updateWordBuilder(currentLetterIndex, nowMs);
  }

  if (nowMs - lastSerialPrintMs >= SERIAL_PRINT_INTERVAL_MS) {
    lastSerialPrintMs = nowMs;
    printSerialStatus();
  }

  server.handleClient();
  delay(2);
}

// ---------------------------------------------------------------------------
// Setup
// ---------------------------------------------------------------------------

void setupSensors() {
  analogReadResolution(12);

  for (byte i = 0; i < NUM_FINGERS; i++) {
    pinMode(DEFAULT_FINGER_PINS[i], INPUT);
    analogSetPinAttenuation(DEFAULT_FINGER_PINS[i], ADC_11db);
  }

  for (byte i = 0; i < NUM_ADC1_CANDIDATES; i++) {
    pinMode(ADC1_CANDIDATE_PINS[i], INPUT);
    analogSetPinAttenuation(ADC1_CANDIDATE_PINS[i], ADC_11db);
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

// ---------------------------------------------------------------------------
// Sensor logic
// ---------------------------------------------------------------------------

void readSensors() {
  for (byte finger = 0; finger < NUM_FINGERS; finger++) {
    rawValues[finger] = analogRead(fingerPins[finger]);
    sensorHasSignal[finger] = rawHasSignal(rawValues[finger], finger);

    if (!sensorHasSignal[finger]) {
      bendPercent[finger] = 0;
      fingerStates[finger] = STATE_NO_SIGNAL;
      continue;
    }

    if (!smoothingReady) {
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

bool rawHasSignal(int rawValue, byte finger) {
  if (fingerPinLikelyConnected[finger]) {
    return true;
  }

  return rawValue > RAW_LOW_NO_SIGNAL && rawValue < RAW_HIGH_NO_SIGNAL;
}

void refreshFingerPinMapping() {
  bool candidateConnected[NUM_ADC1_CANDIDATES] = {false, false, false, false, false, false};

  connectedCandidateCount = 0;

  for (byte candidate = 0; candidate < NUM_ADC1_CANDIDATES; candidate++) {
    const int pin = ADC1_CANDIDATE_PINS[candidate];
    int localMin = 4095;
    int localMax = 0;
    long total = 0;

    for (byte sample = 0; sample < 12; sample++) {
      const int value = analogRead(pin);
      total += value;

      if (value < localMin) {
        localMin = value;
      }

      if (value > localMax) {
        localMax = value;
      }

      delay(2);
    }

    candidatePinAverage[candidate] = total / 12;
    candidatePinMin[candidate] = localMin;
    candidatePinMax[candidate] = localMax;

    const bool insideRails =
      candidatePinAverage[candidate] > RAW_LOW_NO_SIGNAL &&
      candidatePinAverage[candidate] < RAW_HIGH_NO_SIGNAL;
    const bool hasMotion = (localMax - localMin) >= 4;

    candidateConnected[candidate] = insideRails || hasMotion;

    if (candidateConnected[candidate]) {
      connectedCandidateCount++;
    }
  }

  for (byte finger = 0; finger < NUM_FINGERS; finger++) {
    const int defaultPin = DEFAULT_FINGER_PINS[finger];
    const int candidateIndex = indexOfCandidatePin(defaultPin);

    fingerPins[finger] = defaultPin;
    fingerPinAutoMapped[finger] = false;
    fingerPinLikelyConnected[finger] = false;

    if (candidateIndex >= 0 && candidateConnected[candidateIndex]) {
      fingerPinLikelyConnected[finger] = true;
    }
  }
}

int indexOfCandidatePin(int pin) {
  for (byte i = 0; i < NUM_ADC1_CANDIDATES; i++) {
    if (ADC1_CANDIDATE_PINS[i] == pin) {
      return i;
    }
  }

  return -1;
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
// Gesture logic
// ---------------------------------------------------------------------------

int detectLetter() {
  for (int i = 0; i < NUM_LETTERS; i++) {
    if (letterMatches(LETTERS[i])) {
      return i;
    }
  }

  return LETTER_UNKNOWN;
}

bool letterMatches(const LetterRule &rule) {
  for (byte finger = 0; finger < NUM_FINGERS; finger++) {
    if (IGNORE_PINKY_IN_RECOGNITION && finger == FINGER_PINKY) {
      continue;
    }

    if (!stateMatches(fingerStates[finger], rule.match[finger])) {
      return false;
    }
  }

  return true;
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
// Word and sentence builder
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

  if (currentSentence.length() > 0) {
    currentSentence += ' ';
  }

  currentSentence += currentWord;
  trimSentenceText();
  currentWord = "";
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
// Web handlers
// ---------------------------------------------------------------------------

void handleRoot() {
  server.send_P(200, "text/html", INDEX_HTML);
}

void handleData() {
  String json = "{";

  json += "\"ms\":";
  json += millis();

  json += ",\"letter\":";
  appendJsonString(json, letterName(currentLetterIndex));

  json += ",\"caption\":";
  appendJsonString(json, captionText(currentLetterIndex));

  json += ",\"word\":";
  appendJsonString(json, currentWord);

  json += ",\"sentence\":";
  appendJsonString(json, currentSentence);

  json += ",\"statusText\":";
  appendJsonString(json, statusText());

  json += ",\"tipText\":";
  appendJsonString(json, tipText());

  json += ",\"activeCount\":";
  json += activeSensorCount();

  json += ",\"totalActiveCount\":";
  json += totalSensorCount();

  json += ",\"requiredCount\":";
  json += requiredSensorCount();

  json += ",\"pin\":[";
  appendIntArray(json, fingerPins, NUM_FINGERS);
  json += "]";

  json += ",\"pinAuto\":[";
  appendBoolArray(json, fingerPinAutoMapped, NUM_FINGERS);
  json += "]";

  json += ",\"raw\":[";
  appendIntArray(json, rawValues, NUM_FINGERS);
  json += "]";

  json += ",\"bend\":[";
  appendIntArray(json, bendPercent, NUM_FINGERS);
  json += "]";

  json += ",\"baseline\":[";
  appendBaselineArray(json);
  json += "]";

  json += ",\"min\":[";
  appendObservedArray(json, true);
  json += "]";

  json += ",\"max\":[";
  appendObservedArray(json, false);
  json += "]";

  json += ",\"signal\":[";
  appendBoolArray(json, sensorHasSignal, NUM_FINGERS);
  json += "]";

  json += ",\"state\":[";
  for (byte i = 0; i < NUM_FINGERS; i++) {
    appendJsonString(json, fingerStateShort(fingerStates[i]));
    if (i < NUM_FINGERS - 1) {
      json += ",";
    }
  }
  json += "]";

  json += "}";

  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", json);
}

void handleAction() {
  if (!server.hasArg("cmd")) {
    server.send(400, "text/plain", "Missing cmd");
    return;
  }

  const String command = server.arg("cmd");

  if (command == "clear") {
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
    refreshFingerPinMapping();
  }

  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "text/plain", "OK");
}

void appendIntArray(String &json, int values[], byte count) {
  for (byte i = 0; i < count; i++) {
    json += values[i];
    if (i < count - 1) {
      json += ",";
    }
  }
}

void appendBoolArray(String &json, bool values[], byte count) {
  for (byte i = 0; i < count; i++) {
    json += values[i] ? "true" : "false";
    if (i < count - 1) {
      json += ",";
    }
  }
}

void appendBaselineArray(String &json) {
  for (byte i = 0; i < NUM_FINGERS; i++) {
    json += baselineReady[i] ? (int)(baselineValues[i] + 0.5f) : 0;
    if (i < NUM_FINGERS - 1) {
      json += ",";
    }
  }
}

void appendObservedArray(String &json, bool useMin) {
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

byte activeSensorCount() {
  byte count = 0;

  for (byte i = 0; i < NUM_FINGERS; i++) {
    if (IGNORE_PINKY_IN_RECOGNITION && i == FINGER_PINKY) {
      continue;
    }

    if (sensorHasSignal[i]) {
      count++;
    }
  }

  return count;
}

byte totalSensorCount() {
  byte count = 0;

  for (byte i = 0; i < NUM_FINGERS; i++) {
    if (sensorHasSignal[i]) {
      count++;
    }
  }

  return count;
}

byte requiredSensorCount() {
  return IGNORE_PINKY_IN_RECOGNITION ? (NUM_FINGERS - 1) : NUM_FINGERS;
}

String statusText() {
  const byte activeCount = activeSensorCount();
  const byte requiredCount = requiredSensorCount();

  if (connectedCandidateCount == 0) {
    if (fingerPins[FINGER_PINKY] == 25) {
      return "No core ADC1 signal, pinky ignored on GPIO25";
    }
    return "No ADC1 sensor signal";
  }

  if (activeCount == 0) {
    if (fingerPins[FINGER_PINKY] == 25) {
      return "No live core signal, pinky ignored";
    }
    return "Pins found, but no live finger signal";
  }

  if (activeCount < requiredCount) {
    if (fingerPins[FINGER_PINKY] == 25) {
      return "Partial core signal, pinky ignored";
    }
    return "Partial ADC1 signal";
  }

  if (currentLetterIndex == LETTER_UNKNOWN) {
    if (IGNORE_PINKY_IN_RECOGNITION) {
      return "Core fingers ready, pinky ignored";
    }
    return "Full signal, no mapped letter yet";
  }

  if (IGNORE_PINKY_IN_RECOGNITION) {
    return "Letter ready, pinky ignored";
  }

  return "Letter ready";
}

String tipText() {
  const byte activeCount = activeSensorCount();
  const byte requiredCount = requiredSensorCount();

  if (connectedCandidateCount == 0) {
    return "Software cannot invent missing voltage. Pinky stays on GPIO25 in code, but the site only depends on thumb, index, middle, and ring now.";
  }

  if (activeCount < requiredCount) {
    if (fingerPins[FINGER_PINKY] == 25) {
      return "Thumb, index, middle, and ring are the active recognition set. Pinky remains on GPIO25 for monitoring only.";
    }
    return "Unread sensors still need real ADC1 wiring.";
  }

  if (currentLetterIndex == LETTER_UNKNOWN) {
    return "Hold one bend pattern steady for about one second to lock a letter.";
  }

  if (currentWord.length() == 0) {
    return "Hold a stable letter and it will enter the live word automatically.";
  }

  return "Use Commit Word to move the live word into the sentence line.";
}

// ---------------------------------------------------------------------------
// Serial/status helpers
// ---------------------------------------------------------------------------

void printSerialStatus() {
  Serial.print("Raw T/I/M/R/P: ");
  for (byte i = 0; i < NUM_FINGERS; i++) {
    Serial.print(rawValues[i]);
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

  Serial.print(" | Bend%: ");
  for (byte i = 0; i < NUM_FINGERS; i++) {
    Serial.print(bendPercent[i]);
    if (i < NUM_FINGERS - 1) {
      Serial.print(", ");
    }
  }

  Serial.print(" | Letter: ");
  Serial.print(captionText(currentLetterIndex));
  Serial.print(" | Pins: ");
  Serial.print(pinMapText());
  Serial.print(" | Word: ");
  Serial.print(currentWord);
  Serial.print(" | Sentence: ");
  Serial.println(currentSentence);
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

    if (fingerPinAutoMapped[i]) {
      text += "*";
    }
  }

  return text;
}

const char *fingerStateShort(FingerState state) {
  switch (state) {
    case STATE_NO_SIGNAL:
      return "N";
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

const char *letterName(int letterIndex) {
  if (letterIndex == LETTER_UNKNOWN) {
    return "UNKNOWN";
  }

  return LETTERS[letterIndex].letterName;
}

const char *captionText(int letterIndex) {
  if (letterIndex == LETTER_UNKNOWN) {
    return "No recognized letter";
  }

  return LETTERS[letterIndex].caption;
}
