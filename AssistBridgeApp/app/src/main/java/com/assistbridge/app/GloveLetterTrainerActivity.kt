package com.assistbridge.app

import android.Manifest
import android.annotation.SuppressLint
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothGatt
import android.bluetooth.BluetoothGattCallback
import android.bluetooth.BluetoothGattCharacteristic
import android.bluetooth.BluetoothGattDescriptor
import android.bluetooth.BluetoothManager
import android.bluetooth.BluetoothProfile
import android.bluetooth.le.ScanCallback
import android.bluetooth.le.ScanResult
import android.bluetooth.le.ScanSettings
import android.content.ClipData
import android.content.ClipboardManager
import android.content.Context
import android.content.pm.PackageManager
import android.graphics.Typeface
import android.graphics.drawable.GradientDrawable
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.os.ParcelUuid
import android.os.SystemClock
import android.speech.tts.TextToSpeech
import android.text.InputFilter
import android.text.InputType
import android.util.Log
import android.view.ViewGroup
import android.widget.Button
import android.widget.EditText
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.Space
import android.widget.TextView
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AppCompatActivity
import androidx.core.content.ContextCompat
import org.json.JSONObject
import java.util.Locale
import java.util.UUID
import kotlin.math.abs
import kotlin.math.max
import kotlin.math.min
import kotlin.math.roundToInt

class GloveLetterTrainerActivity : AppCompatActivity(), TextToSpeech.OnInitListener {

    private lateinit var statusText: TextView
    private lateinit var currentLetterText: TextView
    private lateinit var stepText: TextView
    private lateinit var liveText: TextView
    private lateinit var trainedText: TextView
    private lateinit var exportText: TextView
    private lateinit var trainingLettersInput: EditText
    private lateinit var connectButton: Button
    private lateinit var startButton: Button
    private lateinit var retryButton: Button
    private lateinit var exportButton: Button

    private val serviceUuid: UUID = UUID.fromString("8b77a001-7d7c-4f47-b3d5-0f4d4902c001")
    private val dataUuid: UUID = UUID.fromString("8b77a002-7d7c-4f47-b3d5-0f4d4902c001")
    private val cccdUuid: UUID = UUID.fromString("00002902-0000-1000-8000-00805f9b34fb")
    private val deviceName = "SignaSenseGlove"

    private val fingerNames = arrayOf("Thumb", "Index", "Middle", "Ring", "Pinky")
    private val letters = ('A'..'Z').map { it.toString() }
    private val prepMs = 3_000L
    private val captureMs = 1_500L
    private val testCaptureMs = 1_200L
    private val requiredTestPasses = UserDefinedSignsStore.REQUIRED_TEST_PASSES
    private val minimumFramesPerCapture = 3
    private val minimumUsableSignals = 3

    private var bluetoothGatt: BluetoothGatt? = null
    private var dataCharacteristic: BluetoothGattCharacteristic? = null
    private var isScanning = false
    private var connected = false
    private var lastFrame: SensorFrame? = null
    private var lastFrameAtMs = 0L
    private var lastPins = intArrayOf(25, 33, 32, 35, 34)

    private var currentLetterIndex = 0
    private var testAttempt = 1
    private var phase = TrainerPhase.IDLE
    private var phaseStartedAtMs = 0L
    private var activeTrainingLetters = emptyList<String>()
    private val captureFrames = mutableListOf<SensorFrame>()
    private val trainedPatterns = linkedMapOf<String, LetterPattern>()
    private lateinit var signsStore: UserDefinedSignsStore

    private var tts: TextToSpeech? = null
    private var ttsReady = false

    private val uiHandler = Handler(Looper.getMainLooper())
    private val trainerTick =
        object : Runnable {
            override fun run() {
                updateTimedTrainer()
                uiHandler.postDelayed(this, 200L)
            }
        }

    private val blePollHandler = Handler(Looper.getMainLooper())
    private val blePollIntervalMs = 600L
    private val blePollRunnable =
        object : Runnable {
            override fun run() {
                readGloveStatus()
                blePollHandler.postDelayed(this, blePollIntervalMs)
            }
        }

    private val permissionLauncher =
        registerForActivityResult(ActivityResultContracts.RequestMultiplePermissions()) {
            if (hasBlePermissions()) {
                startBleScan()
            } else {
                statusText.text = "Bluetooth permission is required to train from glove readings."
            }
        }

    private val scanCallback =
        object : ScanCallback() {
            override fun onScanResult(callbackType: Int, result: ScanResult) {
                val advertisedName = result.scanRecord?.deviceName
                    ?: if (hasBlePermissions()) result.device.name.orEmpty() else ""
                val hasService = result.scanRecord?.serviceUuids?.contains(ParcelUuid(serviceUuid)) == true
                val matches = advertisedName == deviceName || hasService
                Log.d("GloveTrainer", "scan name=$advertisedName service=$hasService rssi=${result.rssi}")

                if (matches) {
                    stopBleScan()
                    connectToDevice(result.device)
                }
            }

            override fun onScanFailed(errorCode: Int) {
                isScanning = false
                runOnUiThread {
                    statusText.text = "BLE scan failed: $errorCode"
                    connectButton.isEnabled = true
                }
            }
        }

    private val gattCallback =
        object : BluetoothGattCallback() {
            override fun onConnectionStateChange(gatt: BluetoothGatt, status: Int, newState: Int) {
                if (newState == BluetoothProfile.STATE_CONNECTED) {
                    connected = true
                    runOnUiThread {
                        statusText.text = "Connected. Discovering glove training service..."
                        connectButton.isEnabled = false
                    }
                    requestMtuAndDiscover(gatt)
                } else if (newState == BluetoothProfile.STATE_DISCONNECTED) {
                    connected = false
                    runOnUiThread {
                        statusText.text = "Glove disconnected."
                        connectButton.isEnabled = true
                    }
                    dataCharacteristic = null
                    stopLiveReadPolling()
                    bluetoothGatt?.close()
                    bluetoothGatt = null
                }
            }

            override fun onMtuChanged(gatt: BluetoothGatt, mtu: Int, status: Int) {
                discoverServices(gatt)
            }

            override fun onServicesDiscovered(gatt: BluetoothGatt, status: Int) {
                val service = gatt.getService(serviceUuid)
                val characteristic = service?.getCharacteristic(dataUuid)
                if (characteristic == null) {
                    runOnUiThread {
                        statusText.text = "Glove service not found. Restart the ESP32 glove, then connect again."
                        connectButton.isEnabled = true
                    }
                    return
                }

                dataCharacteristic = characteristic
                enableNotifications(gatt, characteristic)
                runOnUiThread {
                    statusText.text = "Glove connected. Waiting for actual raw readings..."
                }
            }

            override fun onCharacteristicChanged(
                gatt: BluetoothGatt,
                characteristic: BluetoothGattCharacteristic,
                value: ByteArray,
            ) {
                handlePayload(value.toString(Charsets.UTF_8))
            }

            @Deprecated("Deprecated in Java")
            override fun onCharacteristicChanged(
                gatt: BluetoothGatt,
                characteristic: BluetoothGattCharacteristic,
            ) {
                handlePayload(characteristic.value?.toString(Charsets.UTF_8).orEmpty())
            }

            override fun onCharacteristicRead(
                gatt: BluetoothGatt,
                characteristic: BluetoothGattCharacteristic,
                value: ByteArray,
                status: Int,
            ) {
                if (status == BluetoothGatt.GATT_SUCCESS) {
                    handlePayload(value.toString(Charsets.UTF_8))
                }
            }

            override fun onDescriptorWrite(
                gatt: BluetoothGatt,
                descriptor: BluetoothGattDescriptor,
                status: Int,
            ) {
                startLiveReadPolling()
            }
        }

    override fun onCreate(savedInstanceState: Bundle?) {
        ThemeSettings.apply(this)
        super.onCreate(savedInstanceState)
        signsStore = UserDefinedSignsStore(this)
        loadPatterns()
        buildUi()
        updateTrainingSummary()
        tts = TextToSpeech(this, this)
        uiHandler.post(trainerTick)
    }

    override fun onInit(status: Int) {
        ttsReady = status == TextToSpeech.SUCCESS
        if (ttsReady) {
            tts?.language = Locale.US
            tts?.setSpeechRate(0.92f)
            tts?.setPitch(1.0f)
        }
    }

    private fun buildUi() {
        val background = ContextCompat.getColor(this, R.color.assist_background)
        val surface = ContextCompat.getColor(this, R.color.assist_surface)
        val textColor = ContextCompat.getColor(this, R.color.assist_text)
        val muted = ContextCompat.getColor(this, R.color.assist_muted)
        val border = ContextCompat.getColor(this, R.color.assist_border)

        val scroll = ScrollView(this).apply {
            setBackgroundColor(background)
            layoutParams = ViewGroup.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.MATCH_PARENT,
            )
        }

        val root = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dp(18), dp(20), dp(18), dp(28))
        }

        val title = TextView(this).apply {
            setTextColor(textColor)
            textSize = 30f
            typeface = Typeface.DEFAULT_BOLD
            text = "Glove Letter Trainer"
        }
        root.addView(title)

        val intro = TextView(this).apply {
            setTextColor(muted)
            textSize = 16f
            text = "This screen trains from the ESP32 glove raw sensor readings only. Type the letters you want to use, make each requested sign, hold it steady, then pass five live tests before the app moves to the next letter."
            setLineSpacing(2f, 1.0f)
            layoutParams = blockParams(top = 8, bottom = 12)
        }
        root.addView(intro)

        statusText = panelText("Tap Connect Glove. Training will not start until live raw readings arrive.", surface, textColor, border, 16f)
        root.addView(statusText)

        currentLetterText = metric("Current letter", "-", surface, textColor, border)
        stepText = metric("Training step", "Idle", surface, textColor, border)
        liveText = panelText("Live readings will appear here.", surface, textColor, border, 15f).apply {
            typeface = Typeface.MONOSPACE
        }
        trainedText = panelText("", surface, textColor, border, 15f)
        exportText = panelText("Arduino export will appear here after training.", surface, textColor, border, 13f).apply {
            typeface = Typeface.MONOSPACE
        }

        root.addView(currentLetterText)
        root.addView(stepText)
        root.addView(liveText)
        root.addView(trainedText)

        connectButton = Button(this).apply {
            text = "Connect Glove"
            styleButton(this, filled = true)
            setOnClickListener { beginConnection() }
        }
        root.addView(connectButton)

        trainingLettersInput = EditText(this).apply {
            hint = "Letters to train, e.g. A C Y"
            setSingleLine(true)
            inputType = InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_FLAG_CAP_CHARACTERS
            filters = arrayOf(InputFilter.AllCaps())
            setTextColor(textColor)
            setHintTextColor(muted)
            textSize = 16f
            setPadding(dp(14), dp(10), dp(14), dp(10))
            setBackground(roundedPanel(surface, border))
            layoutParams = blockParams(top = 6, bottom = 6)
        }
        root.addView(trainingLettersInput)

        startButton = Button(this).apply {
            text = "Start Selected Training"
            styleButton(this, filled = true)
            setOnClickListener { continueTraining() }
        }
        root.addView(startButton)

        retryButton = Button(this).apply {
            text = "Retry Current Letter"
            styleButton(this, filled = false)
            setOnClickListener { retryCurrentLetter() }
        }
        root.addView(retryButton)

        exportButton = Button(this).apply {
            text = "Export Arduino Patterns"
            styleButton(this, filled = false)
            setOnClickListener { showExportAndCopy() }
        }
        root.addView(exportButton)

        root.addView(Button(this).apply {
            text = "Clear Trained Patterns on This Phone"
            styleButton(this, filled = false)
            setOnClickListener { clearLocalPatterns() }
        })

        root.addView(Button(this).apply {
            text = "Back"
            styleButton(this, filled = false)
            setOnClickListener { finish() }
        })

        root.addView(Space(this).apply {
            layoutParams = LinearLayout.LayoutParams(1, dp(8))
        })
        root.addView(exportText)

        scroll.addView(root)
        setContentView(scroll)
    }

    private fun panelText(value: String, surface: Int, textColor: Int, border: Int, size: Float): TextView {
        return TextView(this).apply {
            setTextColor(textColor)
            textSize = size
            text = value
            setPadding(dp(14), dp(12), dp(14), dp(12))
            setBackground(roundedPanel(surface, border))
            setLineSpacing(2f, 1.0f)
            layoutParams = blockParams(bottom = 10)
        }
    }

    private fun metric(label: String, value: String, surface: Int, textColor: Int, border: Int): TextView {
        return panelText("$label\n$value", surface, textColor, border, 24f).apply {
            typeface = Typeface.DEFAULT_BOLD
            contentDescription = label
        }
    }

    private fun styleButton(button: Button, filled: Boolean) {
        val primary = ContextCompat.getColor(this, R.color.assist_primary)
        val border = ContextCompat.getColor(this, R.color.assist_border)
        val surface = ContextCompat.getColor(this, R.color.assist_surface)
        val textColor = ContextCompat.getColor(this, R.color.assist_text)
        val onPrimary = ContextCompat.getColor(this, R.color.assist_on_primary)

        button.isAllCaps = false
        button.minHeight = dp(54)
        button.setPadding(dp(12), 0, dp(12), 0)
        button.setTextColor(if (filled) onPrimary else textColor)
        button.textSize = 16f
        button.setBackground(roundedPanel(if (filled) primary else surface, if (filled) primary else border))
        button.layoutParams = blockParams(top = 6, bottom = 6)
    }

    private fun roundedPanel(fillColor: Int, strokeColor: Int): GradientDrawable {
        return GradientDrawable().apply {
            shape = GradientDrawable.RECTANGLE
            cornerRadius = dp(8).toFloat()
            setColor(fillColor)
            setStroke(dp(1), strokeColor)
        }
    }

    private fun blockParams(top: Int = 0, bottom: Int = 0): LinearLayout.LayoutParams {
        return LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT,
            ViewGroup.LayoutParams.WRAP_CONTENT,
        ).apply {
            topMargin = dp(top)
            bottomMargin = dp(bottom)
        }
    }

    private fun continueTraining() {
        when (phase) {
            TrainerPhase.IDLE, TrainerPhase.COMPLETE -> startTrainingFromNextNeededLetter()
            TrainerPhase.WAITING_PATTERN_READY -> beginCapturePhase(TrainerPhase.CAPTURING_PATTERN)
            TrainerPhase.WAITING_TEST_READY -> beginCapturePhase(TrainerPhase.CAPTURING_TEST)
            TrainerPhase.PREPARE_CAPTURE,
            TrainerPhase.CAPTURING_PATTERN,
            TrainerPhase.PREPARE_TEST,
            TrainerPhase.CAPTURING_TEST -> {
                statusText.text = "Training is already running. Hold the current sign steady."
            }
        }
    }

    private fun startTrainingFromNextNeededLetter() {
        if (!hasFreshLiveFrame()) {
            val message = "Connect the glove first. I need live raw sensor readings before training."
            statusText.text = message
            speak(message)
            return
        }

        val requestedLetters = requestedTrainingLetters()
        if (requestedLetters.isEmpty()) {
            val message = "Type one or more letters to train first. Example: A C Y."
            statusText.text = message
            speak(message)
            updateStartButtonText()
            return
        }

        activeTrainingLetters = requestedLetters
        currentLetterIndex = activeTrainingLetters.indexOfFirst { letter ->
            (trainedPatterns[letter]?.testsPassed ?: 0) < requiredTestPasses
        }.takeIf { it >= 0 } ?: 0

        beginLetterSetup()
    }

    private fun retryCurrentLetter() {
        if (activeTrainingLetters.isEmpty()) {
            activeTrainingLetters = requestedTrainingLetters()
        }
        if (activeTrainingLetters.isEmpty()) {
            statusText.text = "Type the letter you want to retry first."
            updateStartButtonText()
            return
        }

        if (!hasFreshLiveFrame()) {
            val message = "No fresh glove reading. Connect the glove and move your fingers once."
            statusText.text = message
            speak(message)
            return
        }

        trainedPatterns.remove(currentLetter())
        savePatterns()
        testAttempt = 1
        beginLetterSetup()
    }

    private fun beginLetterSetup() {
        val letter = currentLetter()
        phase = TrainerPhase.WAITING_PATTERN_READY
        phaseStartedAtMs = SystemClock.elapsedRealtime()
        captureFrames.clear()
        currentLetterText.text = "Current letter\n$letter"
        stepText.text = "Training step\nMake letter $letter, then tap Continue Capture."
        statusText.text = "Make letter $letter and hold it steady. Tap Continue Capture when ready."
        updateStartButtonText()
        speak("Make letter $letter. Hold it steady. Tap continue capture when ready.")
    }

    private fun beginCapturePhase(nextPhase: TrainerPhase) {
        phase = if (nextPhase == TrainerPhase.CAPTURING_PATTERN) {
            TrainerPhase.PREPARE_CAPTURE
        } else {
            TrainerPhase.PREPARE_TEST
        }
        phaseStartedAtMs = SystemClock.elapsedRealtime()
        captureFrames.clear()
        val letter = currentLetter()
        val label = if (phase == TrainerPhase.PREPARE_CAPTURE) {
            "Capturing pattern for $letter in three seconds"
        } else {
            "Testing $letter, attempt $testAttempt of $requiredTestPasses in three seconds"
        }
        stepText.text = "Training step\n$label"
        statusText.text = label
        updateStartButtonText()
    }

    private fun updateTimedTrainer() {
        val now = SystemClock.elapsedRealtime()
        when (phase) {
            TrainerPhase.IDLE,
            TrainerPhase.WAITING_PATTERN_READY,
            TrainerPhase.WAITING_TEST_READY,
            TrainerPhase.COMPLETE -> Unit
            TrainerPhase.PREPARE_CAPTURE -> {
                val remaining = countdownRemaining(now, prepMs)
                stepText.text = "Training step\nMake letter ${currentLetter()}. Capture starts in $remaining seconds."
                if (now - phaseStartedAtMs >= prepMs) {
                    beginCapturePhase(TrainerPhase.CAPTURING_PATTERN)
                }
            }

            TrainerPhase.CAPTURING_PATTERN -> {
                val elapsed = now - phaseStartedAtMs
                stepText.text = "Training step\nCapturing ${currentLetter()}... ${captureFrames.size} usable frames"
                if (elapsed >= captureMs) {
                    finishPatternCapture()
                }
            }

            TrainerPhase.PREPARE_TEST -> {
                val remaining = countdownRemaining(now, prepMs)
                stepText.text = "Training step\nTest ${currentLetter()}, attempt $testAttempt of $requiredTestPasses. Capture starts in $remaining seconds."
                if (now - phaseStartedAtMs >= prepMs) {
                    beginCapturePhase(TrainerPhase.CAPTURING_TEST)
                }
            }

            TrainerPhase.CAPTURING_TEST -> {
                val elapsed = now - phaseStartedAtMs
                stepText.text = "Training step\nTesting ${currentLetter()}... ${captureFrames.size} usable frames"
                if (elapsed >= testCaptureMs) {
                    finishTestCapture()
                }
            }
        }
    }

    private fun finishPatternCapture() {
        val letter = currentLetter()
        val pattern = averagePattern(letter, captureFrames, existingPasses = 0)
        if (pattern == null) {
            phase = TrainerPhase.IDLE
            val message = "Pattern capture failed for $letter. The app did not receive enough usable glove readings."
            statusText.text = message
            stepText.text = "Training step\n$message"
            updateStartButtonText()
            speak(message)
            return
        }

        trainedPatterns[letter] = pattern
        savePatterns()
        testAttempt = 1
        phase = TrainerPhase.WAITING_TEST_READY
        phaseStartedAtMs = SystemClock.elapsedRealtime()
        updateTrainingSummary()
        updateStartButtonText()
        val message = "Pattern captured for $letter. Make $letter again, then tap Continue Test for attempt one of five."
        statusText.text = message
        speak(message)
    }

    private fun finishTestCapture() {
        val letter = currentLetter()
        val currentPattern = trainedPatterns[letter]
        val sample = averagePattern(letter, captureFrames, existingPasses = 0)
        if (currentPattern == null || sample == null) {
            phase = TrainerPhase.WAITING_TEST_READY
            phaseStartedAtMs = SystemClock.elapsedRealtime()
            val message = "Test did not get enough glove readings. Hold letter $letter again."
            statusText.text = message
            updateStartButtonText()
            speak(message)
            return
        }

        val match = classify(sample)
        val currentDistance = patternDistance(sample, currentPattern)
        val passed = match?.letter == letter && currentDistance <= currentPattern.tolerance

        if (!passed) {
            phase = TrainerPhase.WAITING_TEST_READY
            phaseStartedAtMs = SystemClock.elapsedRealtime()
            val predicted = match?.letter ?: "unknown"
            val message = "Test failed. I read $predicted, not $letter. Make $letter again, then tap Continue Test."
            statusText.text = "$message Distance ${currentDistance.roundToInt()}, tolerance ${currentPattern.tolerance}."
            updateStartButtonText()
            speak(message)
            return
        }

        val updated = currentPattern.copy(testsPassed = max(currentPattern.testsPassed, testAttempt))
        trainedPatterns[letter] = updated
        savePatterns()
        updateTrainingSummary()

        if (testAttempt >= requiredTestPasses) {
            if (currentLetterIndex >= activeTrainingLetters.lastIndex) {
                phase = TrainerPhase.COMPLETE
                stepText.text = "Training step\nSelected letter training complete."
                statusText.text = "Selected letters are trained and tested on this phone."
                updateStartButtonText()
                speak("Selected letters are trained and tested.")
            } else {
                currentLetterIndex += 1
                testAttempt = 1
                beginLetterSetup()
            }
        } else {
            testAttempt += 1
            phase = TrainerPhase.WAITING_TEST_READY
            phaseStartedAtMs = SystemClock.elapsedRealtime()
            val message = "Good. Test $letter passed. Make $letter again, then tap Continue Test for attempt $testAttempt of $requiredTestPasses."
            statusText.text = "$message Distance ${currentDistance.roundToInt()}."
            updateStartButtonText()
            speak(message)
        }
    }

    private fun countdownRemaining(now: Long, totalMs: Long): Long {
        val remaining = max(0L, totalMs - (now - phaseStartedAtMs))
        return max(1L, (remaining + 999L) / 1_000L)
    }

    private fun currentLetter(): String {
        val scope = activeTrainingLetters.ifEmpty { requestedTrainingLetters() }.ifEmpty { listOf("A") }
        return scope[currentLetterIndex.coerceIn(0, scope.lastIndex)]
    }

    private fun requestedTrainingLetters(): List<String> {
        if (!::trainingLettersInput.isInitialized) {
            return emptyList()
        }

        return trainingLettersInput.text
            .toString()
            .uppercase(Locale.US)
            .filter { it in 'A'..'Z' }
            .map { it.toString() }
            .distinct()
    }

    private fun hasFreshLiveFrame(): Boolean {
        val frame = lastFrame ?: return false
        return SystemClock.elapsedRealtime() - lastFrameAtMs < 2_500L && isUsableFrame(frame)
    }

    private fun averagePattern(letter: String, frames: List<SensorFrame>, existingPasses: Int): LetterPattern? {
        if (frames.size < minimumFramesPerCapture) {
            return null
        }

        val raw = IntArray(fingerNames.size)
        val bend = IntArray(fingerNames.size)
        for (index in fingerNames.indices) {
            raw[index] = frames.map { it.raw[index] }.average().roundToInt()
            bend[index] = frames.map { it.bend[index] }.average().roundToInt().coerceIn(0, 100)
        }

        val drift = fingerNames.indices.map { index ->
            val values = frames.map { it.bend[index] }
            (values.maxOrNull() ?: 0) - (values.minOrNull() ?: 0)
        }.average()
        val rawDrift = fingerNames.indices.map { index ->
            val values = frames.map { it.raw[index] }
            ((values.maxOrNull() ?: 0) - (values.minOrNull() ?: 0)) / 20.0
        }.average()
        val tolerance = (max(drift, rawDrift) * 2.5 + 12.0).roundToInt().coerceIn(14, 36)

        return LetterPattern(
            letter = letter,
            raw = raw,
            bend = bend,
            tolerance = tolerance,
            testsPassed = existingPasses,
        )
    }

    private fun classify(sample: LetterPattern): LetterMatch? {
        return trainedPatterns.values
            .filter { it.raw.any { value -> value > 0 } }
            .map { pattern -> LetterMatch(pattern.letter, patternDistance(sample, pattern)) }
            .minByOrNull { it.distance }
    }

    private fun patternDistance(sample: LetterPattern, pattern: LetterPattern): Double {
        val bendDistance = bendDistance(sample.bend, pattern.bend)
        val rawDistance = rawDistance(sample.raw, pattern.raw)
        val bendHasInformation = sample.bend.any { it > 5 } || pattern.bend.any { it > 5 }
        return if (bendHasInformation) {
            (bendDistance * 0.70) + (rawDistance * 0.30)
        } else {
            rawDistance
        }
    }

    private fun bendDistance(first: IntArray, second: IntArray): Double {
        return first.indices.sumOf { index -> abs(first[index] - second[index]).toDouble() } / first.size
    }

    private fun rawDistance(first: IntArray, second: IntArray): Double {
        return first.indices.sumOf { index ->
            abs(first[index] - second[index]).toDouble() / 20.0
        } / first.size
    }

    private fun updateFromStatus(status: JSONObject) {
        val frame = readFrame(status) ?: return
        val pins = readIntArray(status, "pins")
        if (pins.size == fingerNames.size) {
            lastPins = pins
        }

        lastFrame = frame
        lastFrameAtMs = SystemClock.elapsedRealtime()
        liveText.text = buildLiveReadings(frame)

        if (phase == TrainerPhase.CAPTURING_PATTERN || phase == TrainerPhase.CAPTURING_TEST) {
            if (isUsableFrame(frame)) {
                captureFrames.add(frame)
            } else {
                statusText.text = "Live frame received but not usable. Check flex sensor power, ground, and signal wires."
            }
        }
    }

    private fun readFrame(status: JSONObject): SensorFrame? {
        val raw = readIntArray(status, "raw")
        val bend = readIntArray(status, "bend")
        val signal = readBoolArray(status, "signal")
        if (raw.size != fingerNames.size || bend.size != fingerNames.size || signal.size != fingerNames.size) {
            return null
        }

        return SensorFrame(raw, bend, signal)
    }

    private fun readIntArray(status: JSONObject, name: String): IntArray {
        val values = status.optJSONArray(name) ?: return IntArray(0)
        return IntArray(values.length()) { index -> values.optInt(index, 0) }
    }

    private fun readBoolArray(status: JSONObject, name: String): BooleanArray {
        val values = status.optJSONArray(name) ?: return BooleanArray(0)
        return BooleanArray(values.length()) { index -> values.optBoolean(index, false) }
    }

    private fun isUsableFrame(frame: SensorFrame): Boolean {
        val activeSignals = frame.signal.count { it }
        val rawMax = frame.raw.maxOrNull() ?: 0
        val rawMin = frame.raw.minOrNull() ?: 0
        return activeSignals >= minimumUsableSignals && rawMax > 20 && rawMax - rawMin > 8
    }

    private fun buildLiveReadings(frame: SensorFrame): String {
        val builder = StringBuilder()
        builder.appendLine("Live glove readings")
        builder.appendLine("Pin order is read from the ESP32 payload.")
        fingerNames.forEachIndexed { index, name ->
            val signal = if (frame.signal[index]) "OK" else "NO SIGNAL"
            builder.appendLine(
                "$name GPIO ${lastPins.getOrNull(index) ?: 0}: raw=${frame.raw[index]} bend=${frame.bend[index]} signal=$signal",
            )
        }
        builder.appendLine("Usable for training: ${if (isUsableFrame(frame)) "yes" else "no"}")
        return builder.toString()
    }

    private fun updateTrainingSummary() {
        val selectedLetters = activeTrainingLetters.ifEmpty { requestedTrainingLetters() }
        val visibleLetters = selectedLetters.ifEmpty { trainedPatterns.keys.sorted() }
        val trainedCount = visibleLetters.count { letter -> (trainedPatterns[letter]?.testsPassed ?: 0) >= requiredTestPasses }
        val partialCount = visibleLetters.count { letter -> (trainedPatterns[letter]?.testsPassed ?: 0) in 1 until requiredTestPasses }
        val builder = StringBuilder()
        builder.appendLine("Training progress")
        builder.appendLine("Selected letters: ${visibleLetters.joinToString(" ").ifBlank { "none" }}")
        builder.appendLine("Fully tested selected letters: $trainedCount / ${visibleLetters.size}")
        builder.appendLine("Partly tested letters: $partialCount")
        if (visibleLetters.isNotEmpty()) {
            builder.appendLine(
                visibleLetters.joinToString(" ") { letter ->
                    val passes = trainedPatterns[letter]?.testsPassed ?: 0
                    if (passes >= requiredTestPasses) "$letter:5" else "$letter:$passes"
                },
            )
        }
        trainedText.text = builder.toString()
    }

    private fun showExportAndCopy() {
        val export = buildArduinoExport()
        exportText.text = export
        val clipboard = getSystemService(Context.CLIPBOARD_SERVICE) as ClipboardManager
        clipboard.setPrimaryClip(ClipData.newPlainText("SignaSense glove trained patterns", export))
        statusText.text = "Arduino pattern code copied to clipboard and shown below."
        speak("Arduino pattern code copied.")
    }

    private fun buildArduinoExport(): String {
        val builder = StringBuilder()
        builder.appendLine("// SignaSense glove trained letter patterns")
        builder.appendLine("// Generated on this phone from actual ESP32 glove raw readings.")
        builder.appendLine("// Finger order: Thumb, Index, Middle, Ring, Pinky.")
        builder.appendLine("// Pins from last glove payload: ${lastPins.joinToString(", ")}")
        builder.appendLine("const bool TRAINED_PATTERN_ENABLED[26] = {")
        builder.appendLine(
            letters.joinToString(", ") { letter ->
                if ((trainedPatterns[letter]?.testsPassed ?: 0) >= requiredTestPasses) "true" else "false"
            },
        )
        builder.appendLine("};")
        builder.appendLine("const int TRAINED_PATTERN_BEND[26][5] = {")
        letters.forEach { letter ->
            val pattern = trainedPatterns[letter]
            val values = pattern?.bend ?: IntArray(fingerNames.size)
            builder.appendLine("  { ${values.joinToString(", ")} }, // $letter")
        }
        builder.appendLine("};")
        builder.appendLine("const int TRAINED_PATTERN_RAW[26][5] = {")
        letters.forEach { letter ->
            val pattern = trainedPatterns[letter]
            val values = pattern?.raw ?: IntArray(fingerNames.size)
            builder.appendLine("  { ${values.joinToString(", ")} }, // $letter")
        }
        builder.appendLine("};")
        builder.appendLine("const int TRAINED_PATTERN_TOLERANCE[26] = {")
        builder.appendLine(
            letters.joinToString(", ") { letter ->
                (trainedPatterns[letter]?.tolerance ?: 0).toString()
            },
        )
        builder.appendLine("};")
        return builder.toString()
    }

    private fun loadPatterns() {
        trainedPatterns.clear()
        signsStore.loadPatterns().values.forEach { pattern ->
            if (pattern.raw.size == fingerNames.size && pattern.bend.size == fingerNames.size) {
                trainedPatterns[pattern.letter] = pattern.toLetterPattern()
            }
        }
    }

    private fun savePatterns() {
        signsStore.savePatterns(trainedPatterns.values.map { it.toUserDefinedPattern() })
    }

    private fun UserDefinedSignPattern.toLetterPattern(): LetterPattern {
        return LetterPattern(letter, raw, bend, tolerance, testsPassed)
    }

    private fun LetterPattern.toUserDefinedPattern(): UserDefinedSignPattern {
        return UserDefinedSignPattern(
            letter = letter,
            raw = raw,
            bend = bend,
            tolerance = tolerance,
            testsPassed = testsPassed,
        )
    }

    private fun clearLocalPatterns() {
        trainedPatterns.clear()
        savePatterns()
        updateTrainingSummary()
        exportText.text = "Arduino export will appear here after training."
        phase = TrainerPhase.IDLE
        currentLetterIndex = 0
        activeTrainingLetters = emptyList()
        testAttempt = 1
        statusText.text = "Local trained glove patterns cleared on this phone."
        stepText.text = "Training step\nIdle"
        currentLetterText.text = "Current letter\n-"
        updateStartButtonText()
        speak("Local trained glove patterns cleared.")
    }

    private fun updateStartButtonText() {
        startButton.text = when (phase) {
            TrainerPhase.IDLE -> "Start Selected Training"
            TrainerPhase.WAITING_PATTERN_READY -> "Continue Capture"
            TrainerPhase.WAITING_TEST_READY -> "Continue Test $testAttempt of $requiredTestPasses"
            TrainerPhase.PREPARE_CAPTURE,
            TrainerPhase.CAPTURING_PATTERN,
            TrainerPhase.PREPARE_TEST,
            TrainerPhase.CAPTURING_TEST -> "Training Running"
            TrainerPhase.COMPLETE -> "Restart Selected Training"
        }
    }

    private fun beginConnection() {
        if (!hasBlePermissions()) {
            permissionLauncher.launch(requiredBlePermissions())
            return
        }

        startBleScan()
    }

    @SuppressLint("MissingPermission")
    private fun startBleScan() {
        val adapter = bluetoothAdapter()
        if (adapter == null) {
            statusText.text = "This phone does not have Bluetooth."
            return
        }

        if (!adapter.isEnabled) {
            statusText.text = "Turn on phone Bluetooth, then tap Connect Glove again."
            return
        }

        if (isScanning) {
            return
        }

        bluetoothGatt?.close()
        bluetoothGatt = null
        dataCharacteristic = null
        connected = false
        lastFrame = null
        stopLiveReadPolling()

        val settings = ScanSettings.Builder()
            .setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY)
            .build()

        statusText.text = "Scanning for SignaSenseGlove..."
        connectButton.isEnabled = false
        isScanning = true
        adapter.bluetoothLeScanner?.startScan(null, settings, scanCallback)
    }

    @SuppressLint("MissingPermission")
    private fun stopBleScan() {
        if (!isScanning) {
            return
        }

        bluetoothAdapter()?.bluetoothLeScanner?.stopScan(scanCallback)
        isScanning = false
    }

    @SuppressLint("MissingPermission")
    private fun connectToDevice(device: BluetoothDevice) {
        runOnUiThread { statusText.text = "Connecting to SignaSenseGlove..." }
        bluetoothGatt = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M) {
            device.connectGatt(this, false, gattCallback, BluetoothDevice.TRANSPORT_LE)
        } else {
            device.connectGatt(this, false, gattCallback)
        }
    }

    @SuppressLint("MissingPermission")
    private fun requestMtuAndDiscover(gatt: BluetoothGatt) {
        gatt.requestMtu(512)
        gatt.discoverServices()
    }

    @SuppressLint("MissingPermission")
    private fun discoverServices(gatt: BluetoothGatt) {
        gatt.discoverServices()
    }

    @SuppressLint("MissingPermission")
    private fun enableNotifications(gatt: BluetoothGatt, characteristic: BluetoothGattCharacteristic) {
        gatt.setCharacteristicNotification(characteristic, true)
        val descriptor = characteristic.getDescriptor(cccdUuid) ?: return

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            gatt.writeDescriptor(descriptor, BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE)
        } else {
            @Suppress("DEPRECATION")
            descriptor.value = BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE
            @Suppress("DEPRECATION")
            gatt.writeDescriptor(descriptor)
        }
    }

    private fun handlePayload(payload: String) {
        if (payload.isBlank()) {
            return
        }

        val status = try {
            JSONObject(payload)
        } catch (error: Exception) {
            Log.d("GloveTrainer", "bad payload length=${payload.length}: ${error.message}")
            return
        }

        runOnUiThread { updateFromStatus(status) }
    }

    @SuppressLint("MissingPermission")
    private fun startLiveReadPolling() {
        stopLiveReadPolling()
        blePollHandler.post(blePollRunnable)
    }

    private fun stopLiveReadPolling() {
        blePollHandler.removeCallbacks(blePollRunnable)
    }

    @SuppressLint("MissingPermission")
    private fun readGloveStatus() {
        val gatt = bluetoothGatt ?: return
        val characteristic = dataCharacteristic ?: return
        if (!hasBlePermissions()) {
            return
        }
        gatt.readCharacteristic(characteristic)
    }

    @SuppressLint("MissingPermission")
    private fun sendCommand(command: String) {
        val gatt = bluetoothGatt
        val characteristic = dataCharacteristic
        if (gatt == null || characteristic == null) {
            statusText.text = "Connect the glove before sending commands."
            return
        }

        val bytes = command.toByteArray(Charsets.UTF_8)
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            gatt.writeCharacteristic(characteristic, bytes, BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT)
        } else {
            @Suppress("DEPRECATION")
            characteristic.value = bytes
            characteristic.writeType = BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT
            @Suppress("DEPRECATION")
            gatt.writeCharacteristic(characteristic)
        }
    }

    private fun speak(message: String) {
        val clean = message.trim()
        if (!ttsReady || clean.isBlank()) {
            return
        }

        tts?.speak(clean, TextToSpeech.QUEUE_FLUSH, null, "glove-trainer")
    }

    private fun bluetoothAdapter() =
        (getSystemService(Context.BLUETOOTH_SERVICE) as BluetoothManager).adapter

    private fun hasBlePermissions(): Boolean {
        return requiredBlePermissions().all { permission ->
            ContextCompat.checkSelfPermission(this, permission) == PackageManager.PERMISSION_GRANTED
        }
    }

    private fun requiredBlePermissions(): Array<String> {
        return if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            arrayOf(
                Manifest.permission.BLUETOOTH_SCAN,
                Manifest.permission.BLUETOOTH_CONNECT,
                Manifest.permission.ACCESS_FINE_LOCATION,
            )
        } else {
            arrayOf(Manifest.permission.ACCESS_FINE_LOCATION)
        }
    }

    private fun dp(value: Int): Int {
        return (value * resources.displayMetrics.density).toInt()
    }

    override fun onDestroy() {
        uiHandler.removeCallbacks(trainerTick)
        stopBleScan()
        stopLiveReadPolling()
        bluetoothGatt?.close()
        bluetoothGatt = null
        tts?.stop()
        tts?.shutdown()
        super.onDestroy()
    }

    private data class SensorFrame(
        val raw: IntArray,
        val bend: IntArray,
        val signal: BooleanArray,
    )

    private data class LetterPattern(
        val letter: String,
        val raw: IntArray,
        val bend: IntArray,
        val tolerance: Int,
        val testsPassed: Int,
    )

    private data class LetterMatch(
        val letter: String,
        val distance: Double,
    )

    private enum class TrainerPhase {
        IDLE,
        WAITING_PATTERN_READY,
        PREPARE_CAPTURE,
        CAPTURING_PATTERN,
        WAITING_TEST_READY,
        PREPARE_TEST,
        CAPTURING_TEST,
        COMPLETE,
    }

}
