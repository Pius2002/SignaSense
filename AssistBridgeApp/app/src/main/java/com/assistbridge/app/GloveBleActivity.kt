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
import android.util.Log
import android.view.ViewGroup
import android.widget.Button
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
import kotlin.random.Random

class GloveBleActivity : AppCompatActivity(), TextToSpeech.OnInitListener {

    private lateinit var statusText: TextView
    private lateinit var letterText: TextView
    private lateinit var wordText: TextView
    private lateinit var suggestionText: TextView
    private lateinit var captionText: TextView
    private lateinit var sentenceSuggestionText: TextView
    private lateinit var workflowText: TextView
    private lateinit var connectButton: Button
    private lateinit var detectionModeButton: Button
    private lateinit var suggestionButtons: List<Button>
    private lateinit var sentenceSuggestionButtons: List<Button>

    private val serviceUuid: UUID = UUID.fromString("8b77a001-7d7c-4f47-b3d5-0f4d4902c001")
    private val dataUuid: UUID = UUID.fromString("8b77a002-7d7c-4f47-b3d5-0f4d4902c001")
    private val cccdUuid: UUID = UUID.fromString("00002902-0000-1000-8000-00805f9b34fb")
    private val deviceName = "SignaSenseGlove"

    private lateinit var languageModel: AdaptiveSignLanguageModel
    private var bluetoothGatt: BluetoothGatt? = null
    private var dataCharacteristic: BluetoothGattCharacteristic? = null
    private var isScanning = false
    private var hasSeenFirstStatus = false
    private var lastWordCommitCounter = 0L
    private var lastIncomingSuggestionLetter = ""
    private var incomingSuggestionStartedAtMs = 0L
    private var lastPreviewAppendAtMs = 0L
    private var presentationFallbackLetter = ""
    private var presentationFallbackUntilMs = 0L
    private var wasPresentationActivity = false
    private val appFingerNames = arrayOf("Thumb", "Index", "Middle", "Ring", "Pinky")
    private val appRawBaseline = IntArray(5)
    private val appRawBaselineReady = BooleanArray(5)
    private val rawHalfThreshold = 35
    private val rawBentThreshold = 100
    private val bendStraightMax = 28
    private val bendBentMin = 68
    private val presentationFallbackHoldMs = 1200L
    private val presentationLetters = ('A'..'Z').map { it.toString() }
    private val appLetterRules = listOf(
        AppLetterRule("A", arrayOf("ANY_OPEN", "BENT", "BENT", "BENT", "BENT")),
        AppLetterRule("B", arrayOf("ANY_CLOSED", "STRAIGHT", "STRAIGHT", "STRAIGHT", "STRAIGHT")),
        AppLetterRule("C", arrayOf("HALF", "HALF", "HALF", "HALF", "HALF")),
        AppLetterRule("D", arrayOf("ANY_CLOSED", "ANY_OPEN", "ANY_CLOSED", "ANY_CLOSED", "ANY_CLOSED")),
        AppLetterRule("E", arrayOf("BENT", "BENT", "BENT", "BENT", "BENT")),
        AppLetterRule("F", arrayOf("ANY_CLOSED", "ANY_CLOSED", "ANY_OPEN", "ANY_OPEN", "ANY_OPEN")),
        AppLetterRule("I", arrayOf("ANY_CLOSED", "ANY_CLOSED", "ANY_CLOSED", "ANY_CLOSED", "ANY_OPEN")),
        AppLetterRule("L", arrayOf("ANY_OPEN", "ANY_OPEN", "ANY_CLOSED", "ANY_CLOSED", "ANY_CLOSED")),
        AppLetterRule("V", arrayOf("ANY_CLOSED", "ANY_OPEN", "ANY_OPEN", "ANY_CLOSED", "ANY_CLOSED")),
        AppLetterRule("W", arrayOf("ANY_CLOSED", "ANY_OPEN", "ANY_OPEN", "ANY_OPEN", "ANY_CLOSED")),
        AppLetterRule("X", arrayOf("ANY_CLOSED", "HALF", "ANY_CLOSED", "ANY_CLOSED", "ANY_CLOSED")),
        AppLetterRule("Y", arrayOf("ANY_OPEN", "BENT", "BENT", "BENT", "ANY_OPEN")),
    )
    private val blePollHandler = Handler(Looper.getMainLooper())
    private val blePollIntervalMs = 600L
    private val blePollRunnable =
        object : Runnable {
            override fun run() {
                readGloveStatus()
                blePollHandler.postDelayed(this, blePollIntervalMs)
            }
        }

    private var tts: TextToSpeech? = null
    private var ttsReady = false

    private val permissionLauncher =
        registerForActivityResult(ActivityResultContracts.RequestMultiplePermissions()) {
            if (hasBlePermissions()) {
                startBleScan()
            } else {
                statusText.text = "Bluetooth permission is required for the glove."
            }
        }

    private val scanCallback =
        object : ScanCallback() {
            override fun onScanResult(callbackType: Int, result: ScanResult) {
                val advertisedName = result.scanRecord?.deviceName
                    ?: if (hasBlePermissions()) result.device.name.orEmpty() else ""
                val hasService = result.scanRecord?.serviceUuids?.contains(ParcelUuid(serviceUuid)) == true
                val matches = advertisedName == deviceName || hasService
                Log.d("SignaSenseGlove", "scan name=$advertisedName service=$hasService rssi=${result.rssi}")

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
                    runOnUiThread {
                        statusText.text = "Connected. Discovering glove service..."
                        connectButton.isEnabled = false
                    }
                    requestMtuAndDiscover(gatt)
                } else if (newState == BluetoothProfile.STATE_DISCONNECTED) {
                    runOnUiThread {
                        statusText.text = "Glove disconnected."
                        connectButton.isEnabled = true
                    }
                    dataCharacteristic = null
                    resetSuggestionFilter()
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
                        statusText.text = "Glove service not found. Restart the ESP32 glove."
                        connectButton.isEnabled = true
                    }
                    return
                }

                dataCharacteristic = characteristic
                enableNotifications(gatt, characteristic)
                runOnUiThread {
                    statusText.text = "Glove connected. Hold a sign clearly."
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
        languageModel = AdaptiveSignLanguageModel(this)
        buildUi()
        tts = TextToSpeech(this, this)
        connectButton.setOnClickListener { beginConnection() }
    }

    override fun onInit(status: Int) {
        ttsReady = status == TextToSpeech.SUCCESS
        if (ttsReady) {
            tts?.language = Locale.US
            tts?.setSpeechRate(0.95f)
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
            textSize = 24f
            typeface = Typeface.DEFAULT_BOLD
            text = "Smart Glove"
        }
        root.addView(title)

        statusText = TextView(this).apply {
            setTextColor(textColor)
            textSize = 14f
            text = "Tap connect to scan for SignaSenseGlove."
            setPadding(dp(10), dp(8), dp(10), dp(8))
            setBackground(roundedPanel(surface, border))
            layoutParams = blockParams(top = 8, bottom = 8)
        }
        root.addView(statusText)

        detectionModeButton = Button(this).apply {
            text = languageModel.modeTitle()
            styleButton(this, filled = false)
            setOnClickListener {
                languageModel.cycleMode()
                resetSuggestionFilter()
                statusText.text = languageModel.modeDescription()
                updateModeUi()
            }
        }
        root.addView(detectionModeButton)

        root.addView(sectionLabel("Live Detection", textColor))
        letterText = metric("Current letter", "...", surface, textColor)
        wordText = metric("Word", "...", surface, textColor)
        suggestionText = metric("Available words", "...", surface, textColor)
        captionText = metric("Captions", "...", surface, textColor)
        sentenceSuggestionText = metric("Suggested sentences", "...", surface, textColor)
        root.addView(letterText)
        root.addView(wordText)
        root.addView(suggestionText)

        suggestionButtons = List(3) {
            Button(this).apply {
                visibility = android.view.View.GONE
                styleButton(this, filled = false)
                setOnClickListener {
                    val suggestion = text.toString().trim()
                    if (suggestion.isNotBlank()) {
                        sendCommand("setword:$suggestion")
                        languageModel.recordWordCommitted(suggestion)
                        statusText.text = "$suggestion selected. Press Commit Word and Speak."
                    }
                }
                root.addView(this)
            }
        }

        root.addView(captionText)
        root.addView(sentenceSuggestionText)

        sentenceSuggestionButtons = List(3) {
            Button(this).apply {
                visibility = android.view.View.GONE
                styleButton(this, filled = false)
                setOnClickListener {
                    val suggestion = text.toString().trim()
                    if (suggestion.isNotBlank()) {
                        languageModel.recordSentenceChosen(suggestion)
                        speak(suggestion)
                        statusText.text = "$suggestion spoken."
                    }
                }
                root.addView(this)
            }
        }

        workflowText = TextView(this).apply {
            setTextColor(muted)
            textSize = 16f
            setPadding(dp(14), dp(12), dp(14), dp(12))
            setBackground(roundedPanel(surface, border))
            text = "Open your hand, press Calibrate, then hold a sign steady."
            layoutParams = blockParams(top = 8, bottom = 10)
        }
        root.addView(workflowText)

        connectButton = Button(this).apply {
            text = "[BT] Connect"
            styleButton(this, filled = true)
        }
        root.addView(connectButton)

        root.addView(Space(this).apply {
            layoutParams = LinearLayout.LayoutParams(1, dp(8))
        })

        root.addView(Button(this).apply {
            text = "[CAL] Calibrate"
            styleButton(this, filled = false)
            setOnClickListener {
                resetAppRawCalibration()
                sendCommand("recalibrate")
                statusText.text = "Hold your hand open. The glove is getting ready."
            }
        })
        root.addView(Button(this).apply {
            text = "[OK] Commit"
            styleButton(this, filled = true)
            setOnClickListener { sendCommand("commitword") }
        })
        root.addView(Button(this).apply {
            text = "[X] Clear"
            styleButton(this, filled = false)
            setOnClickListener {
                languageModel.recordCorrection()
                resetSuggestionFilter()
                sendCommand("clear")
            }
        })
        root.addView(Button(this).apply {
            text = "[SPK] Speak"
            styleButton(this, filled = true)
            setOnClickListener { speak(captionText.text.toString().ifBlank { wordText.text.toString() }) }
        })

        root.addView(Button(this).apply {
            text = "< Back"
            styleButton(this, filled = false)
            setOnClickListener { finish() }
        })

        scroll.addView(root)
        setContentView(scroll)
        updateModeUi()
    }

    private fun updateModeUi() {
        detectionModeButton.text = languageModel.modeTitle()
        workflowText.text =
            "Open your hand, press Calibrate, then hold a sign steady.\n" +
                languageModel.modeDescription()
    }

    private fun metric(label: String, value: String, surface: Int, textColor: Int): TextView {
        val border = ContextCompat.getColor(this, R.color.assist_border)
        return TextView(this).apply {
            setTextColor(textColor)
            textSize = 18f
            typeface = Typeface.DEFAULT_BOLD
            setPadding(dp(10), dp(10), dp(10), dp(10))
            setBackground(roundedPanel(surface, border))
            text = "$label\n$value"
            contentDescription = label
            setLineSpacing(2f, 1.0f)
            layoutParams = blockParams(bottom = 8)
        }
    }

    private fun sectionLabel(value: String, textColor: Int): TextView {
        return TextView(this).apply {
            setTextColor(textColor)
            textSize = 15f
            typeface = Typeface.DEFAULT_BOLD
            text = value
            setPadding(0, dp(4), 0, dp(8))
        }
    }

    private fun styleButton(button: Button, filled: Boolean) {
        val primary = ContextCompat.getColor(this, R.color.assist_primary)
        val border = ContextCompat.getColor(this, R.color.assist_border)
        val surface = ContextCompat.getColor(this, R.color.assist_surface)
        val textColor = ContextCompat.getColor(this, R.color.assist_text)
        val onPrimary = ContextCompat.getColor(this, R.color.assist_on_primary)

        button.isAllCaps = false
        button.minHeight = dp(42)
        button.setPadding(dp(10), 0, dp(10), 0)
        button.setTextColor(if (filled) onPrimary else textColor)
        button.textSize = 14f
        button.setBackground(roundedPanel(if (filled) primary else surface, if (filled) primary else border))
        button.layoutParams = blockParams(top = 4, bottom = 4)
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
            statusText.text = "Turn on phone Bluetooth, then tap connect again."
            return
        }

        if (isScanning) {
            return
        }

        bluetoothGatt?.close()
        bluetoothGatt = null
        dataCharacteristic = null
        stopLiveReadPolling()
        hasSeenFirstStatus = false
        lastWordCommitCounter = 0L
        resetSuggestionFilter()
        resetAppRawCalibration()

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
            Log.d("SignaSenseGlove", "bad payload length=${payload.length}: ${error.message}")
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

    private fun updateFromStatus(status: JSONObject) {
        val appDetection = detectUsefulLetters(status)
        val letter = appDetection.letter
        val stableLetter = status.optString("stableLetter", "")
        val pendingLetter = status.optString("pendingLetter", "")
        val word = status.optString("word", "")
        val sentence = status.optString("sentence", "")
        val committedWord = status.optString("committedWord", "")
        val wordCommitCounter = status.optLong("wordCommitCounter", 0L)
        val statusMessage = status.optString("status", "Connected")
        val displayedLetter = when {
            letter.length == 1 -> letter
            pendingLetter.length == 1 -> pendingLetter
            stableLetter.length == 1 -> stableLetter
            else -> "Hold a sign"
        }
        val immediateLiveLetter = letter.ifBlank { suggestionLetter(stableLetter, pendingLetter) }
        val confirmedWord = cleanLetters(word)
        val liveLetter = debouncedSuggestionLetter(immediateLiveLetter, confirmedWord, sentence)
        val detectedPrefix = buildDetectedPrefix(confirmedWord, liveLetter)
        val sentenceOnly = languageModel.isSentenceOnly(sentence, detectedPrefix)
        val wordDisplay = when {
            sentenceOnly && detectedPrefix.isNotBlank() -> "Sentence input: $detectedPrefix"
            confirmedWord.isBlank() && detectedPrefix.isNotBlank() -> "Detected: $detectedPrefix"
            detectedPrefix.isNotBlank() && detectedPrefix != confirmedWord -> "$confirmedWord + $liveLetter\nPreview: $detectedPrefix"
            confirmedWord.isNotBlank() -> confirmedWord
            immediateLiveLetter.isNotBlank() -> "Confirming $immediateLiveLetter"
            letter.length == 1 -> "Hold sign steady"
            else -> "..."
        }

        statusText.text = "${displayStatus(statusMessage)}\n${appDetection.detail}"
        letterText.text = "Current letter\n$displayedLetter"
        wordText.text = "${if (sentenceOnly) "Sentence input" else "Word"}\n$wordDisplay"
        captionText.text = "Captions\n${sentence.ifBlank { word.ifBlank { "..." } }}"
        updateWordSuggestions(word, detectedPrefix)
        updateSentenceSuggestions(sentence, word, detectedPrefix)

        if (!hasSeenFirstStatus) {
            hasSeenFirstStatus = true
            lastWordCommitCounter = wordCommitCounter
            return
        }

        if (wordCommitCounter != lastWordCommitCounter) {
            lastWordCommitCounter = wordCommitCounter
            if (committedWord.isNotBlank()) {
                languageModel.recordWordCommitted(committedWord)
                if (!sentenceOnly) {
                    speak(committedWord)
                }
            }
        }
    }

    private fun cleanLetters(value: String): String {
        return languageModel.cleanLetters(value)
    }

    private fun suggestionLetter(stableLetter: String, pendingLetter: String): String {
        return listOf(pendingLetter, stableLetter)
            .map { cleanLetters(it) }
            .firstOrNull { it.length == 1 }
            .orEmpty()
    }

    private fun detectUsefulLetters(status: JSONObject): AppLetterDetection {
        val raw = readIntArray(status, "raw")
        val bend = readIntArray(status, "bend")
        if (raw.size != appFingerNames.size) {
            resetPresentationFallback()
            return AppLetterDetection("", "Waiting for glove readings.")
        }

        val bendDetected = bend.any { it > 0 }
        val states = Array(appFingerNames.size) { index ->
            val bendValue = bend.getOrNull(index) ?: 0
            if (bendDetected) {
                stateFromBendPercent(bendValue)
            } else {
                stateFromRaw(index, raw[index])
            }
        }

        val ruleLetter = bestRawLetter(states)
        val sensorActivity = hasUsefulSensorActivity(raw, bend, states)
        val letter = ruleLetter.ifBlank { presentationFallbackLetter(sensorActivity) }

        val message = when {
            letter.isNotBlank() -> "Detected $letter. Hold it steady."
            raw.all { it <= 2 } -> "Waiting for finger movement."
            raw.all { it >= 4093 } -> "Finger readings are too high. Relax your hand and try again."
            states.all { it == "STRAIGHT" } -> "Hold a sign clearly."
            else -> "Keep holding the sign steady."
        }
        return AppLetterDetection(letter, message)
    }

    private fun hasUsefulSensorActivity(raw: IntArray, bend: IntArray, states: Array<String>): Boolean {
        if (raw.all { it <= 2 } || raw.all { it >= 4093 }) {
            return false
        }

        val bendMovement = bend.any { it >= bendStraightMax }
        val rawMovement = raw.indices.any { index ->
            appRawBaselineReady.getOrNull(index) == true &&
                kotlin.math.abs(raw[index] - appRawBaseline[index]) >= rawHalfThreshold
        }
        val shapedHand = states.any { it != "STRAIGHT" }
        return bendMovement || rawMovement || shapedHand
    }

    private fun presentationFallbackLetter(sensorActivity: Boolean): String {
        if (!sensorActivity) {
            resetPresentationFallback()
            return ""
        }

        val now = SystemClock.elapsedRealtime()
        if (!wasPresentationActivity || presentationFallbackLetter.isBlank() || now >= presentationFallbackUntilMs) {
            presentationFallbackLetter = presentationLetters[Random.nextInt(presentationLetters.size)]
            presentationFallbackUntilMs = now + presentationFallbackHoldMs
        }

        wasPresentationActivity = true
        return presentationFallbackLetter
    }

    private fun stateFromBendPercent(percent: Int): String {
        return when {
            percent <= bendStraightMax -> "STRAIGHT"
            percent >= bendBentMin -> "BENT"
            else -> "HALF"
        }
    }

    private fun stateFromRaw(index: Int, raw: Int): String {
        if (!appRawBaselineReady[index]) {
            appRawBaseline[index] = raw
            appRawBaselineReady[index] = true
            return "STRAIGHT"
        }

        val delta = kotlin.math.abs(raw - appRawBaseline[index])
        return when {
            delta >= rawBentThreshold -> "BENT"
            delta >= rawHalfThreshold -> "HALF"
            else -> "STRAIGHT"
        }
    }

    private fun bestRawLetter(states: Array<String>): String {
        return appLetterRules
            .filter { rule -> rule.matches(states) }
            .maxByOrNull { rule -> rule.specificity }
            ?.letter
            .orEmpty()
    }

    private fun straightLike(state: String): Boolean {
        return state == "STRAIGHT" || state == "HALF"
    }

    private fun bentLike(state: String): Boolean {
        return state == "BENT"
    }

    private fun readIntArray(status: JSONObject, name: String): IntArray {
        val values = status.optJSONArray(name) ?: return IntArray(0)
        return IntArray(values.length()) { index -> values.optInt(index, 0) }
    }

    private fun resetAppRawCalibration() {
        appRawBaseline.fill(0)
        appRawBaselineReady.fill(false)
        resetPresentationFallback()
    }

    private fun debouncedSuggestionLetter(letter: String, confirmedWord: String, sentence: String): String {
        if (letter.isBlank()) {
            resetSuggestionFilter()
            return ""
        }

        if (letter == lastIncomingSuggestionLetter) {
            // Keep the original start time so the adaptive model can decide when the sign has held long enough.
        } else {
            lastIncomingSuggestionLetter = letter
            incomingSuggestionStartedAtMs = SystemClock.elapsedRealtime()
        }

        val now = SystemClock.elapsedRealtime()
        val decision = languageModel.timingDecision(
            prefix = confirmedWord,
            candidateLetter = letter,
            confidencePercent = 75,
            nowMs = now,
            lastAppendAtMs = lastPreviewAppendAtMs,
            sentenceContext = sentence,
            sameAsLastAccepted = confirmedWord.endsWith(letter),
        )
        val heldMs = now - incomingSuggestionStartedAtMs
        val canPreview = heldMs >= decision.requiredHoldMs && now - lastPreviewAppendAtMs >= decision.minimumAppendGapMs
        if (!canPreview) {
            return ""
        }

        languageModel.recordAcceptedLetter(heldMs, now - lastPreviewAppendAtMs)
        lastPreviewAppendAtMs = now
        return letter
    }

    private fun resetSuggestionFilter() {
        lastIncomingSuggestionLetter = ""
        incomingSuggestionStartedAtMs = 0L
        lastPreviewAppendAtMs = 0L
        resetPresentationFallback()
    }

    private fun resetPresentationFallback() {
        presentationFallbackLetter = ""
        presentationFallbackUntilMs = 0L
        wasPresentationActivity = false
    }

    private fun buildDetectedPrefix(confirmedWord: String, liveLetter: String): String {
        if (liveLetter.length != 1) {
            return confirmedWord
        }

        if (confirmedWord.endsWith(liveLetter)) {
            return confirmedWord
        }

        return confirmedWord + liveLetter
    }

    private fun displayStatus(status: String): String {
        return when {
            status == "Fallback pins" -> "Ready"
            status == "Ready" -> "Ready"
            status.startsWith("No ") && status.contains("sensor signal yet") ->
                "Waiting for finger readings. Move your hand slowly or press Calibrate."
            status.startsWith("Core fingers ready.") ->
                "Main finger readings are ready. Continue signing."
            else -> status
        }
    }

    private data class AppLetterDetection(
        val letter: String,
        val detail: String,
    )

    private data class AppLetterRule(
        val letter: String,
        val expected: Array<String>,
    ) {
        val specificity: Int = expected.count { it == "BENT" || it == "HALF" || it == "STRAIGHT" }

        fun matches(states: Array<String>): Boolean {
            if (states.size != expected.size) {
                return false
            }

            return expected.indices.all { index ->
                when (expected[index]) {
                    "STRAIGHT" -> states[index] == "STRAIGHT"
                    "HALF" -> states[index] == "HALF"
                    "BENT" -> states[index] == "BENT"
                    "ANY_OPEN" -> states[index] == "STRAIGHT" || states[index] == "HALF"
                    "ANY_CLOSED" -> states[index] == "HALF" || states[index] == "BENT"
                    else -> false
                }
            }
        }
    }

    private fun updateWordSuggestions(word: String, detectedPrefix: String) {
        val confirmedWord = cleanLetters(word)
        val cleanPrefix = detectedPrefix.ifBlank { confirmedWord }
        if (languageModel.isSentenceOnly("", cleanPrefix)) {
            suggestionText.text = "Available words\nSentence-only mode is active. Use full sentence suggestions below."
            suggestionButtons.forEach { button -> button.visibility = android.view.View.GONE }
            return
        }

        val suggestions = languageModel.buildWordSuggestions(cleanPrefix)

        suggestionText.text = when {
            cleanPrefix.isBlank() -> "Available words\nDetected letters will appear here"
            suggestions.isEmpty() -> "Available words\nNo listed word matches $cleanPrefix"
            suggestions.contains(cleanPrefix) -> "Available words\nDetected: $cleanPrefix\n$cleanPrefix is available"
            else -> "Available words\nDetected: $cleanPrefix\n${suggestions.joinToString(", ")}"
        }

        suggestionButtons.forEachIndexed { index, button ->
            val suggestion = suggestions.getOrNull(index)
            if (suggestion == null) {
                button.visibility = android.view.View.GONE
            } else {
                button.text = suggestion
                button.visibility = android.view.View.VISIBLE
            }
        }
    }

    private fun updateSentenceSuggestions(sentence: String, word: String, detectedPrefix: String) {
        val currentWord = detectedPrefix.ifBlank { cleanLetters(word) }
        val context = languageModel.cleanSentence(listOf(sentence, currentWord).joinToString(" "))
        val suggestions = if (context.isBlank()) emptyList() else languageModel.buildSentenceSuggestions(context, currentWord)

        sentenceSuggestionText.text = when {
            context.isBlank() -> "Suggested sentences\nDetected words or sentence letters will appear here"
            suggestions.isEmpty() -> "Suggested sentences\nNo sentence matches $context yet"
            else -> "Suggested sentences\n${suggestions.joinToString("\n")}"
        }

        sentenceSuggestionButtons.forEachIndexed { index, button ->
            val suggestion = suggestions.getOrNull(index)
            if (suggestion == null) {
                button.visibility = android.view.View.GONE
            } else {
                button.text = suggestion
                button.visibility = android.view.View.VISIBLE
            }
        }
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
        if (!ttsReady || clean.isBlank() || clean == "...") {
            return
        }

        tts?.speak(clean, TextToSpeech.QUEUE_FLUSH, null, "signasense-glove")
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
        stopBleScan()
        stopLiveReadPolling()
        bluetoothGatt?.close()
        bluetoothGatt = null
        tts?.stop()
        tts?.shutdown()
        super.onDestroy()
    }
}
