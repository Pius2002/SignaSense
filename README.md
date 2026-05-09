# SignaSense

SignaSense is an assistive technology project combining a smart walking stick, a smart sign-language glove, and an Android application. The system supports obstacle awareness for blind users and sign/caption support for deaf users.

## Main Components

- **Smart Stick:** ESP32-based obstacle detection using an ultrasonic sensor, buzzer backup, Bluetooth speaker voice output, and embedded English/Luganda audio prompts.
- **Smart Glove:** ESP32-based flex-sensor glove that reads finger bends and sends detected hand/finger data to the Android app.
- **Android App:** SignaSense mobile interface for blind and deaf users, including audio onboarding, smart stick controls, smart glove BLE screen, camera sign backup, theme selection, and text-to-speech output.
- **Camera Backup:** Phone camera sign-recognition path using Android CameraX and MediaPipe Tasks Vision.
- **Reports and Diagrams:** Final report, presentation slides, ERD, use case diagram, and supporting documentation.

## Hardware Used

- ESP32 development board
- Ultrasonic distance sensor
- Passive/active buzzer backup
- Bluetooth speaker
- Five flex sensors for glove finger bend sensing
- Voltage-divider resistors for flex sensors
- Jumper wires, breadboard/prototyping board, and shared ground wiring
- Android phone for the SignaSense app
- USB cable for programming and testing ESP32 boards
- Power bank/external supply for portable operation

## Embedded Tools and Technologies

- **Arduino IDE:** Used to write, compile, and upload ESP32 firmware.
- **ESP32 Arduino Core:** Provides GPIO, ADC, Wi-Fi, BLE, Preferences, and Bluetooth support for ESP32.
- **C/C++ for Arduino:** Main firmware language for smart stick and smart glove sketches.
- **Ultrasonic Distance Measurement:** Distance is calculated from echo pulse time and speed of sound.
- **ESP32 ADC:** Reads analog values from flex sensors.
- **ESP32 BLE:** Used by the glove to communicate with the Android application.
- **ESP32 Wi-Fi/WebServer:** Used in earlier/local web interface modes for status pages and captions.
- **ESP32 Bluetooth A2DP Source:** Used by the smart stick to stream spoken audio prompts to a Bluetooth speaker.
- **ESP32 Preferences:** Stores small persistent settings such as language cycling/state.
- **Embedded PCM Audio Clips:** Used for stick voice prompts in English and Luganda.

## Android App Tools and Technologies

- **Android Studio / Android Gradle Plugin 8.5.2:** Android project build system.
- **Gradle Wrapper:** Used to build the app consistently from the command line.
- **Kotlin 1.9.24:** Main Android app programming language.
- **Java 17:** JVM target for the Android build.
- **Android SDK:** `compileSdk 36`, `targetSdk 34`, `minSdk 26`.
- **AndroidX Core KTX 1.15.0:** Android Kotlin extensions and compatibility utilities.
- **AndroidX AppCompat 1.7.0:** Compatibility support for Android activities and UI.
- **AndroidX Activity KTX 1.10.1:** Activity APIs and Kotlin support.
- **Material Components 1.12.0:** Buttons, cards, and Material UI components.
- **Android View Binding:** Type-safe access to XML layout views.
- **Android Text-to-Speech:** Speaks prompts, captions, words, and guidance.
- **Android Speech Recognition:** Supports voice-based onboarding and audio interface commands.
- **Android BLE APIs:** Connects the app to the ESP32 smart glove.
- **Android WebKit:** Supports embedded/local web views where needed.
- **CameraX 1.4.1:** Provides camera preview and frame processing for sign backup.
- **MediaPipe Tasks Vision 0.10.14:** Used for camera-based hand/sign recognition support.

## Model and Data Tools

- **MediaPipe hand/vision pipeline:** Used for camera hand tracking and sign backup.
- **Local model assets:** Stored under the `models` folder for sign/gesture recognition work.
- **Python model scripts:** Used for model experimentation, data preparation, and gesture recognition testing.
- **NPZ/model files:** Support stored hand gesture datasets and trained model experiments.

## Documentation and Presentation Tools

- **Microsoft Word / DOCX:** Final report and chapter documents.
- **Microsoft PowerPoint / PPTX:** Project presentation slides.
- **Mermaid diagrams:** ERD and use-case diagram generation.
- **PNG diagrams/screenshots:** Used for report figures and presentation visuals.
- **Python document scripts:** Used to generate and update report/presentation artifacts.

## Important Project Folders

- `AssistBridgeApp/` - Android application source code.
- `SmartStickESP32/` - Smart stick ESP32 firmware.
- `SignaSenseGloveBLEESP32/` - BLE smart glove ESP32 firmware.
- `SignaSenseGloveESP32/` - Wi-Fi smart glove firmware variant.
- `SmartGloveBT/` - Bluetooth audio glove experiment.
- `SmartGloveWiFiCaptions/` - Wi-Fi caption glove experiment.
- `models/` - Camera/gesture recognition models and scripts.
- `Luganda Recordings/` - Luganda audio recordings used for stick voice prompts.
- `report/` - Report documents, presentation files, and report-generation scripts.
- `reports/` - Generated diagrams such as ERD and architecture-related figures.

## Build Commands

Android debug build:

```powershell
cd AssistBridgeApp
.\gradlew.bat :app:assembleDebug
```

Install debug APK on a connected Android phone:

```powershell
adb install -r AssistBridgeApp\app\build\outputs\apk\debug\app-debug.apk
```

ESP32 firmware:

1. Open the required `.ino` folder in Arduino IDE.
2. Select the correct ESP32 board.
3. Select the detected COM port.
4. Compile and upload.

## Notes

- Flex sensors should be wired as voltage dividers using 3.3 V and a shared ground.
- ESP32 analog input must not receive voltages above 3.3 V.
- If an ultrasonic sensor outputs a 5 V echo signal, the echo line must be level-shifted or voltage-divided before entering the ESP32.
- Bluetooth speaker speech output on ESP32 is done using audio clips/A2DP, not full real-time text-to-speech generated on the microcontroller.
- Android app speech recognition depends on the phone speech engine and may require installed offline speech packs for no-data use.
