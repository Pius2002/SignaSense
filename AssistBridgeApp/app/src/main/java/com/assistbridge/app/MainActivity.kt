package com.assistbridge.app

import android.Manifest
import android.content.res.ColorStateList
import android.content.Intent
import android.content.pm.PackageManager
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.provider.Settings
import android.speech.RecognitionListener
import android.speech.RecognizerIntent
import android.speech.SpeechRecognizer
import android.speech.tts.TextToSpeech
import android.speech.tts.UtteranceProgressListener
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AppCompatActivity
import androidx.core.content.ContextCompat
import androidx.core.view.isVisible
import com.assistbridge.app.databinding.ActivityMainBinding
import com.google.android.material.button.MaterialButton
import org.json.JSONObject
import java.net.HttpURLConnection
import java.net.URL
import java.util.Locale

class MainActivity : AppCompatActivity(), TextToSpeech.OnInitListener {

    private lateinit var binding: ActivityMainBinding

    private var currentStage = VoiceStage.MODE_SELECT
    private var selectedMode: UserMode? = null
    private var selectedLanguage = AppLanguage.ENGLISH

    private var tts: TextToSpeech? = null
    private var ttsReady = false
    private var speechRecognizer: SpeechRecognizer? = null
    private var speechReady = false
    private var isListening = false
    private var pendingAutoListen = false
    private var lastPrompt = ""

    private val stickBaseUrl = "http://192.168.5.1"
    private val stickPollHandler = Handler(Looper.getMainLooper())
    private var stickPollingEnabled = false
    private var announceNextStickStatus = false
    @Volatile
    private var stickRequestInFlight = false
    private var lastStickGuidanceForSpeech = ""
    private var lastStickBucketForSpeech = ""
    private var lastStickSpeechAtMs = 0L
    private var lastStickSpokenStatus = "No smart stick status yet."

    private val stickPollIntervalMs = 500L
    private val stickHttpTimeoutMs = 700
    private val stickSpeechThrottleMs = 1400L

    private val stickPollRunnable =
        object : Runnable {
            override fun run() {
                if (!stickPollingEnabled) {
                    return
                }

                fetchStickStatus(announce = false)
                stickPollHandler.postDelayed(this, stickPollIntervalMs)
            }
        }

    private val audioPermissionLauncher =
        registerForActivityResult(ActivityResultContracts.RequestPermission()) { granted ->
            if (granted) {
                setStatus("Microphone ready. Listening for your command.")
                startVoiceCapture()
            } else {
                setStatus("Microphone permission denied. Use the on-screen buttons.")
                if (selectedMode == UserMode.BLIND) {
                    speakPrompt(
                        "Microphone access was denied. You can still use the large buttons on screen.",
                        autoListen = false,
                    )
                }
            }
        }

    override fun onCreate(savedInstanceState: Bundle?) {
        ThemeSettings.apply(this)
        super.onCreate(savedInstanceState)
        binding = ActivityMainBinding.inflate(layoutInflater)
        setContentView(binding.root)

        setupSpeechRecognizer()
        setupClickListeners()
        updateUi()

        tts = TextToSpeech(this, this)
        tts?.setOnUtteranceProgressListener(
            object : UtteranceProgressListener() {
                override fun onStart(utteranceId: String?) = Unit

                override fun onDone(utteranceId: String?) {
                    if (pendingAutoListen) {
                        pendingAutoListen = false
                        runOnUiThread { startVoiceCapture() }
                    }
                }

                @Deprecated("Deprecated in Java")
                override fun onError(utteranceId: String?) = Unit
            },
        )
    }

    override fun onInit(status: Int) {
        ttsReady = status == TextToSpeech.SUCCESS
        if (!ttsReady) {
            setStatus("Text-to-speech is not ready on this phone.")
            return
        }

        configureTtsLanguage()
        speakPrompt(
            "SignaSense is ready. Say blind for audio guidance or deaf for the visual interface.",
            autoListen = true,
        )
    }

    private fun setupClickListeners() {
        binding.blindButton.setOnClickListener {
            cancelPendingVoiceFlow()
            selectMode(UserMode.BLIND)
        }
        binding.deafButton.setOnClickListener {
            cancelPendingVoiceFlow()
            selectMode(UserMode.DEAF)
        }
        binding.modeVoiceButton.setOnClickListener {
            cancelPendingVoiceFlow()
            startVoiceCapture()
        }
        binding.lightModeButton.setOnClickListener {
            cancelPendingVoiceFlow()
            ThemeSettings.setLight(this)
            updateUi()
        }
        binding.darkModeButton.setOnClickListener {
            cancelPendingVoiceFlow()
            ThemeSettings.setDark(this)
            updateUi()
        }

        binding.englishButton.setOnClickListener {
            cancelPendingVoiceFlow()
            chooseLanguage(AppLanguage.ENGLISH)
        }
        binding.lugandaButton.setOnClickListener {
            cancelPendingVoiceFlow()
            chooseLanguage(AppLanguage.LUGANDA)
        }
        binding.acholiButton.setOnClickListener {
            cancelPendingVoiceFlow()
            chooseLanguage(AppLanguage.ACHOLI)
        }
        binding.voiceInputButton.setOnClickListener {
            cancelPendingVoiceFlow()
            startVoiceCapture()
        }
        binding.repeatPromptButton.setOnClickListener {
            speakPrompt(lastPrompt.ifBlank { languagePrompt() }, autoListen = selectedMode == UserMode.BLIND)
        }

        binding.blindListenButton.setOnClickListener {
            cancelPendingVoiceFlow()
            startVoiceCapture()
        }
        binding.blindRepeatButton.setOnClickListener { speakPrompt(blindDashboardPrompt(), autoListen = true) }
        binding.blindStickButton.setOnClickListener { openStickGuide() }
        binding.blindGloveButton.setOnClickListener { openGlovePage() }
        binding.blindGloveTrainerButton.setOnClickListener { openGloveTrainer() }
        binding.blindCameraGloveButton.setOnClickListener { openCameraGloveBackup() }
        binding.blindHelpButton.setOnClickListener { speakPrompt(commandHelp(), autoListen = true) }
        binding.switchToVisualButton.setOnClickListener {
            cancelPendingVoiceFlow()
            switchToMode(UserMode.DEAF)
        }

        binding.deafStickGuideButton.setOnClickListener { openStickGuide() }
        binding.deafGloveButton.setOnClickListener { openGlovePage() }
        binding.deafGloveTrainerButton.setOnClickListener { openGloveTrainer() }
        binding.deafCameraGloveButton.setOnClickListener { openCameraGloveBackup() }
        binding.deafAudioButton.setOnClickListener {
            cancelPendingVoiceFlow()
            switchToMode(UserMode.BLIND)
        }

        binding.changeLanguageButton.setOnClickListener {
            cancelPendingVoiceFlow()
            showLanguageSelection(autoPrompt = true)
        }
        binding.resetOnboardingButton.setOnClickListener {
            cancelPendingVoiceFlow()
            resetOnboarding()
        }

        binding.stickRefreshButton.setOnClickListener { startStickPolling(announce = selectedMode == UserMode.BLIND) }
        binding.stickScanButton.setOnClickListener { sendStickCommand("/scan", announce = selectedMode == UserMode.BLIND) }
        binding.stickWifiSettingsButton.setOnClickListener { startActivity(Intent(Settings.ACTION_WIFI_SETTINGS)) }
        binding.stickPageButton.setOnClickListener { openStickPage() }
    }

    private fun setupSpeechRecognizer() {
        speechReady = SpeechRecognizer.isRecognitionAvailable(this)
        if (!speechReady) {
            return
        }

        speechRecognizer = SpeechRecognizer.createSpeechRecognizer(this).apply {
            setRecognitionListener(
                object : RecognitionListener {
                    override fun onReadyForSpeech(params: Bundle?) {
                        isListening = true
                        setStatus("Listening...")
                    }

                    override fun onBeginningOfSpeech() {
                        setStatus("Hearing your voice...")
                    }

                    override fun onRmsChanged(rmsdB: Float) = Unit
                    override fun onBufferReceived(buffer: ByteArray?) = Unit

                    override fun onEndOfSpeech() {
                        setStatus("Processing your command...")
                    }

                    override fun onError(error: Int) {
                        isListening = false
                        setStatus(mapSpeechError(error))
                    }

                    override fun onResults(results: Bundle?) {
                        isListening = false
                        val matches = results
                            ?.getStringArrayList(SpeechRecognizer.RESULTS_RECOGNITION)
                            ?.filterNotNull()
                            .orEmpty()

                        val heardText = matches.firstOrNull().orEmpty()
                        if (heardText.isBlank()) {
                            setStatus("No speech detected. Try again.")
                            return
                        }

                        binding.transcriptValue.text = heardText
                        handleVoiceInput(heardText)
                    }

                    override fun onPartialResults(partialResults: Bundle?) {
                        val partialText = partialResults
                            ?.getStringArrayList(SpeechRecognizer.RESULTS_RECOGNITION)
                            ?.firstOrNull()
                            .orEmpty()
                        if (partialText.isNotBlank()) {
                            binding.transcriptValue.text = partialText
                        }
                    }

                    override fun onEvent(eventType: Int, params: Bundle?) = Unit
                },
            )
        }
    }

    private fun startVoiceCapture() {
        if (!speechReady || speechRecognizer == null) {
            setStatus("Speech recognition is not available on this phone.")
            return
        }

        if (
            ContextCompat.checkSelfPermission(this, Manifest.permission.RECORD_AUDIO) !=
            PackageManager.PERMISSION_GRANTED
        ) {
            audioPermissionLauncher.launch(Manifest.permission.RECORD_AUDIO)
            return
        }

        if (isListening) {
            return
        }

        val listenIntent = Intent(RecognizerIntent.ACTION_RECOGNIZE_SPEECH).apply {
            putExtra(RecognizerIntent.EXTRA_LANGUAGE_MODEL, RecognizerIntent.LANGUAGE_MODEL_FREE_FORM)
            putExtra(RecognizerIntent.EXTRA_MAX_RESULTS, 5)
            putExtra(RecognizerIntent.EXTRA_PARTIAL_RESULTS, true)
            putExtra(RecognizerIntent.EXTRA_PREFER_OFFLINE, true)
            putExtra(RecognizerIntent.EXTRA_LANGUAGE, recognitionLanguageTag())
        }

        speechRecognizer?.startListening(listenIntent)
    }

    private fun handleVoiceInput(heardText: String) {
        val spoken = heardText.lowercase(Locale.getDefault())

        when (currentStage) {
            VoiceStage.MODE_SELECT -> {
                when {
                    containsAny(spoken, "blind", "audio", "one", "1") -> selectMode(UserMode.BLIND)
                    containsAny(spoken, "deaf", "visual", "two", "2") -> selectMode(UserMode.DEAF)
                    else -> speakPrompt(
                        "I heard $heardText. Say blind for audio guidance or deaf for the visual interface.",
                        autoListen = true,
                    )
                }
            }

            VoiceStage.LANGUAGE_SELECT -> {
                val language = parseLanguage(spoken)
                if (language == null) {
                    speakPrompt(
                        "I heard $heardText. Say one for English, two for Luganda, or three for Acholi.",
                        autoListen = selectedMode == UserMode.BLIND,
                    )
                } else {
                    chooseLanguage(language)
                }
            }

            VoiceStage.DASHBOARD -> handleDashboardCommand(heardText, spoken)
        }
    }

    private fun handleDashboardCommand(rawText: String, spoken: String) {
        when {
            containsAny(spoken, "scan") -> sendStickCommand("/scan", announce = selectedMode == UserMode.BLIND)
            containsAny(spoken, "status", "distance") -> fetchStickStatus(announce = selectedMode == UserMode.BLIND)
            containsAny(spoken, "stick", "cane") -> openStickGuide()
            containsAny(spoken, "camera", "backup", "visual sign") -> openCameraGloveBackup()
            containsAny(spoken, "train glove", "glove training", "trainer", "train letters") -> openGloveTrainer()
            containsAny(spoken, "glove", "page", "site", "web") -> openGlovePage()
            containsAny(spoken, "language", "change language") -> showLanguageSelection(autoPrompt = true)
            containsAny(spoken, "help", "commands") -> speakPrompt(commandHelp(), autoListen = true)
            containsAny(spoken, "repeat", "again") -> speakPrompt(lastPrompt, autoListen = true)
            containsAny(spoken, "visual", "deaf", "screen") -> switchToMode(UserMode.DEAF)
            containsAny(spoken, "audio", "blind", "voice") -> switchToMode(UserMode.BLIND)
            containsAny(spoken, "home", "reset", "start over") -> resetOnboarding()
            else -> speakPrompt(
                "I heard $rawText. Say stick, scan, status, glove, language, help, repeat, visual, or home.",
                autoListen = true,
            )
        }
    }

    private fun selectMode(mode: UserMode) {
        selectedMode = mode
        showLanguageSelection(autoPrompt = mode == UserMode.BLIND)
    }

    private fun chooseLanguage(language: AppLanguage) {
        selectedLanguage = language
        configureTtsLanguage()
        currentStage = VoiceStage.DASHBOARD
        updateUi()

        val message = when (selectedMode) {
            UserMode.BLIND -> blindDashboardPrompt()
            UserMode.DEAF -> "Visual interface ready. Use Smart Stick or Smart Glove below."
            null -> "Choose blind or deaf to continue."
        }

        setStatus(message)
        if (selectedMode == UserMode.BLIND) {
            speakPrompt(message, autoListen = true)
        }
    }

    private fun showLanguageSelection(autoPrompt: Boolean) {
        currentStage = VoiceStage.LANGUAGE_SELECT
        updateUi()
        val prompt = languagePrompt()
        setStatus(prompt)
        if (autoPrompt) {
            speakPrompt(prompt, autoListen = selectedMode == UserMode.BLIND)
        }
    }

    private fun switchToMode(mode: UserMode) {
        selectedMode = mode
        currentStage = VoiceStage.DASHBOARD
        updateUi()

        if (mode == UserMode.BLIND) {
            speakPrompt(blindDashboardPrompt(), autoListen = true)
        } else {
            setStatus("Visual interface ready.")
        }
    }

    private fun resetOnboarding() {
        selectedMode = null
        currentStage = VoiceStage.MODE_SELECT
        updateUi()
        speakPrompt(
            "SignaSense reset. Say blind for audio guidance or deaf for the visual interface.",
            autoListen = true,
        )
    }

    private fun openStickGuide() {
        startStickPolling(announce = selectedMode == UserMode.BLIND)
    }

    private fun openGlovePage() {
        setStatus("Opening the smart glove Bluetooth screen.")
        startActivity(Intent(this, GloveBleActivity::class.java))
    }

    private fun openGloveTrainer() {
        setStatus("Opening the glove letter trainer.")
        startActivity(Intent(this, GloveLetterTrainerActivity::class.java))
    }

    private fun openCameraGloveBackup() {
        setStatus("Opening camera sign backup.")
        startActivity(Intent(this, CameraSignActivity::class.java))
    }

    private fun openStickPage() {
        setStatus("Opening the smart stick page at 192 point 168 point 5 point 1.")
        startActivity(
            Intent(this, DeviceWebActivity::class.java)
                .putExtra(DeviceWebActivity.EXTRA_TITLE, "Smart Stick")
                .putExtra(DeviceWebActivity.EXTRA_URL, stickBaseUrl),
        )
    }

    private fun updateUi() {
        binding.modeCard.isVisible = currentStage == VoiceStage.MODE_SELECT
        binding.languageCard.isVisible = currentStage == VoiceStage.LANGUAGE_SELECT
        binding.dashboardCard.isVisible = currentStage == VoiceStage.DASHBOARD

        binding.modeValue.text = selectedMode?.label ?: "Not selected"
        binding.languageValue.text = selectedLanguage.label
        binding.themeValue.text = "Appearance: ${ThemeSettings.currentMode(this).label}"

        binding.blindPanel.isVisible = currentStage == VoiceStage.DASHBOARD && selectedMode == UserMode.BLIND
        binding.deafPanel.isVisible = currentStage == VoiceStage.DASHBOARD && selectedMode == UserMode.DEAF

        binding.languageHint.text = when (selectedMode) {
            UserMode.BLIND -> "Blind audio mode can listen for one, two, or three."
            UserMode.DEAF -> "Choose your language visually, then continue."
            null -> "Choose a language after picking blind or deaf."
        }

        binding.blindCommandsValue.text = commandSummary()
        binding.deafSummaryValue.text =
            "Connect the smart stick, open the smart glove BLE screen, or use camera sign backup."

        updateSelectionButtons()
    }

    private fun updateSelectionButtons() {
        styleSelectionButton(binding.lightModeButton, ThemeSettings.currentMode(this) == AppThemeMode.LIGHT)
        styleSelectionButton(binding.darkModeButton, ThemeSettings.currentMode(this) == AppThemeMode.DARK)

        styleSelectionButton(binding.blindButton, selectedMode == UserMode.BLIND)
        styleSelectionButton(binding.deafButton, selectedMode == UserMode.DEAF)

        styleSelectionButton(binding.englishButton, selectedLanguage == AppLanguage.ENGLISH)
        styleSelectionButton(binding.lugandaButton, selectedLanguage == AppLanguage.LUGANDA)
        styleSelectionButton(binding.acholiButton, selectedLanguage == AppLanguage.ACHOLI)
    }

    private fun styleSelectionButton(button: MaterialButton, selected: Boolean) {
        val primary = ContextCompat.getColor(this, R.color.assist_primary)
        val surface = ContextCompat.getColor(this, R.color.assist_surface)
        val border = ContextCompat.getColor(this, R.color.assist_border)
        val textColor = ContextCompat.getColor(this, R.color.assist_text)
        val onPrimary = ContextCompat.getColor(this, R.color.assist_on_primary)

        button.isSelected = selected
        button.backgroundTintList = ColorStateList.valueOf(if (selected) primary else surface)
        button.strokeColor = ColorStateList.valueOf(if (selected) primary else border)
        button.strokeWidth = dp(1)
        button.setTextColor(if (selected) onPrimary else textColor)
        button.rippleColor = ColorStateList.valueOf(primary)
    }

    private fun setStatus(message: String) {
        binding.assistantStatus.text = message
    }

    private fun dp(value: Int): Int {
        return (value * resources.displayMetrics.density).toInt()
    }

    private fun cancelPendingVoiceFlow() {
        pendingAutoListen = false
        isListening = false
        speechRecognizer?.cancel()
        tts?.stop()
    }

    private fun startStickPolling(announce: Boolean) {
        stickPollingEnabled = true
        announceNextStickStatus = announce
        binding.stickConnectionValue.text = "Checking SignaSenseStick at 192.168.5.1"
        binding.stickGuidanceValue.text = "Connect this phone to Wi-Fi network SignaSenseStick."
        setStatus("Checking smart stick connection.")

        if (announce) {
            speakPrompt(
                "Checking smart stick. If it does not connect, connect this phone to Wi Fi network SignaSenseStick. The password is signasense.",
                autoListen = false,
            )
        }

        fetchStickStatus(announce = false)
        stickPollHandler.removeCallbacks(stickPollRunnable)
        stickPollHandler.postDelayed(stickPollRunnable, stickPollIntervalMs)
    }

    private fun fetchStickStatus(announce: Boolean) {
        if (stickRequestInFlight) {
            return
        }

        stickRequestInFlight = true
        Thread {
            try {
                val body = httpGet("$stickBaseUrl/status")
                val status = JSONObject(body)
                runOnUiThread { updateStickStatus(status, announce || announceNextStickStatus) }
            } catch (_: Exception) {
                runOnUiThread { showStickConnectionError(announce || announceNextStickStatus) }
            } finally {
                stickRequestInFlight = false
            }
        }.start()
    }

    private fun sendStickCommand(path: String, announce: Boolean) {
        if (stickRequestInFlight) {
            return
        }

        stickRequestInFlight = true
        binding.stickConnectionValue.text = "Sending command to smart stick..."
        Thread {
            try {
                val body = httpGet("$stickBaseUrl$path")
                val status = JSONObject(body)
                runOnUiThread { updateStickStatus(status, announce) }
            } catch (_: Exception) {
                runOnUiThread { showStickConnectionError(announce) }
            } finally {
                stickRequestInFlight = false
            }
        }.start()
    }

    private fun httpGet(address: String): String {
        val connection = (URL(address).openConnection() as HttpURLConnection).apply {
            requestMethod = "GET"
            connectTimeout = stickHttpTimeoutMs
            readTimeout = stickHttpTimeoutMs
            useCaches = false
        }

        return try {
            connection.inputStream.bufferedReader().use { reader -> reader.readText() }
        } finally {
            connection.disconnect()
        }
    }

    private fun updateStickStatus(status: JSONObject, announce: Boolean) {
        announceNextStickStatus = false

        val valid = status.optBoolean("valid", false)
        val distanceCm = status.optDouble("distance_cm", 0.0)
        val distanceM = status.optDouble("distance_m", 0.0)
        val guidance = status.optString("guidance", "Waiting")
        val trend = status.optString("trend", "unknown")
        val scanState = status.optString("scan_state", "idle")
        val bucket = status.optString("bucket", "unknown")
        val backupBuzzer = status.optBoolean("backup_buzzer", false)
        val clients = status.optInt("connected_clients", 0)

        binding.stickConnectionValue.text =
            "Connected to SignaSenseStick | phone clients: $clients | phone audio active | buzzer: ${if (backupBuzzer) "on" else "ready"}"

        binding.stickDistanceValue.text =
            if (valid) {
                String.format(Locale.US, "%.1f cm / %.2f m", distanceCm, distanceM)
            } else {
                "No valid echo"
            }

        binding.stickGuidanceValue.text =
            "$guidance\nTrend: $trend | Scan: $scanState | Range: $bucket"

        setStatus("Smart stick connected.")

        lastStickSpokenStatus =
            if (valid) {
                String.format(
                    Locale.US,
                    "Smart stick says %s. Distance is %.0f centimeters, %.2f meters.",
                    guidance,
                    distanceCm,
                    distanceM,
                )
            } else {
                "Smart stick says $guidance. No valid distance reading."
            }

        if (selectedMode == UserMode.BLIND && shouldSpeakStickUpdate(announce, guidance, bucket)) {
            val spokenDistance =
                if (valid) {
                    String.format(Locale.US, "%.0f centimeters, %.2f meters", distanceCm, distanceM)
                } else {
                    "no valid distance reading"
                }
            speakPrompt("Smart stick says $guidance. Distance is $spokenDistance.", autoListen = announce)
        }
    }

    private fun showStickConnectionError(announce: Boolean) {
        binding.stickConnectionValue.text = "Not connected to smart stick"
        binding.stickDistanceValue.text = "-- cm / -- m"
        binding.stickGuidanceValue.text =
            "Connect this phone to Wi-Fi network SignaSenseStick. Password: signasense."
        setStatus("Smart stick not reachable.")
        announceNextStickStatus = false

        if (announce && selectedMode == UserMode.BLIND) {
            speakPrompt(
                "Smart stick is not reachable. Connect this phone to Wi Fi network SignaSenseStick. The password is signasense.",
                autoListen = true,
            )
        }
    }

    private fun shouldSpeakStickUpdate(announce: Boolean, guidance: String, bucket: String): Boolean {
        val now = System.currentTimeMillis()

        if (announce) {
            lastStickGuidanceForSpeech = guidance
            lastStickBucketForSpeech = bucket
            lastStickSpeechAtMs = now
            return true
        }

        if (guidance == lastStickGuidanceForSpeech && bucket == lastStickBucketForSpeech) {
            return false
        }

        if (now - lastStickSpeechAtMs < stickSpeechThrottleMs) {
            return false
        }

        lastStickGuidanceForSpeech = guidance
        lastStickBucketForSpeech = bucket
        lastStickSpeechAtMs = now
        return true
    }

    private fun configureTtsLanguage() {
        if (!ttsReady) {
            return
        }

        val desiredLocale = when (selectedLanguage) {
            AppLanguage.ENGLISH -> Locale.US
            AppLanguage.LUGANDA -> Locale.Builder().setLanguage("lg").setRegion("UG").build()
            AppLanguage.ACHOLI -> Locale.Builder().setLanguage("ach").setRegion("UG").build()
        }

        val result = tts?.setLanguage(desiredLocale) ?: TextToSpeech.ERROR
        if (
            result == TextToSpeech.LANG_MISSING_DATA ||
            result == TextToSpeech.LANG_NOT_SUPPORTED
        ) {
            tts?.setLanguage(Locale.Builder().setLanguage("en").setRegion("UG").build())
            if (selectedLanguage != AppLanguage.ENGLISH) {
                setStatus("${selectedLanguage.label} selected. This phone may fall back to English speech.")
            }
        }

        tts?.setSpeechRate(0.92f)
        tts?.setPitch(1.0f)
    }

    private fun speakPrompt(text: String, autoListen: Boolean) {
        lastPrompt = text
        setStatus(text)

        if (!ttsReady || tts == null) {
            pendingAutoListen = false
            return
        }

        pendingAutoListen = autoListen
        tts?.speak(text, TextToSpeech.QUEUE_FLUSH, null, "signasense-prompt")
    }

    private fun recognitionLanguageTag(): String {
        return when (selectedLanguage) {
            AppLanguage.ENGLISH -> "en-US"
            AppLanguage.LUGANDA -> "en-UG"
            AppLanguage.ACHOLI -> "en-UG"
        }
    }

    private fun languagePrompt(): String {
        return when (selectedMode) {
            UserMode.BLIND ->
                "Blind mode selected. Say one for English, two for Luganda, or three for Acholi."

            UserMode.DEAF ->
                "Choose a language, then continue to the visual dashboard."

            null ->
                "Choose blind or deaf first."
        }
    }

    private fun blindDashboardPrompt(): String {
        return when (selectedLanguage) {
            AppLanguage.ENGLISH ->
                "English selected. Say stick for smart stick status, scan to start a stick scan, glove to open the smart glove BLE screen, train glove to train letters, camera for camera sign backup, language to change language, help for commands, or visual to switch to the visual interface."

            AppLanguage.LUGANDA ->
                "Luganda selected. This phone may still speak English if Luganda speech is unavailable. Say stick, scan, status, glove, train glove, camera, language, help, or visual."

            AppLanguage.ACHOLI ->
                "Acholi selected. This phone may still speak English if Acholi speech is unavailable. Say stick, scan, status, glove, train glove, camera, language, help, or visual."
        }
    }

    private fun commandHelp(): String {
        return "Available commands are stick, scan, status, glove, train glove, camera, language, help, repeat, visual, audio, and home."
    }

    private fun commandSummary(): String {
        return "stick, scan, status, glove, train glove, camera, language, help, repeat, visual, audio, home"
    }

    private fun parseLanguage(spoken: String): AppLanguage? {
        return when {
            containsAny(spoken, "one", "1", "english") -> AppLanguage.ENGLISH
            containsAny(spoken, "two", "2", "luganda", "uganda") -> AppLanguage.LUGANDA
            containsAny(spoken, "three", "3", "acholi") -> AppLanguage.ACHOLI
            else -> null
        }
    }

    private fun containsAny(value: String, vararg options: String): Boolean {
        return options.any { option -> value.contains(option) }
    }

    private fun mapSpeechError(error: Int): String {
        return when (error) {
            SpeechRecognizer.ERROR_AUDIO -> "Microphone audio error."
            SpeechRecognizer.ERROR_CLIENT -> "Voice session cancelled."
            SpeechRecognizer.ERROR_INSUFFICIENT_PERMISSIONS -> "Microphone permission is missing."
            SpeechRecognizer.ERROR_NETWORK,
            SpeechRecognizer.ERROR_NETWORK_TIMEOUT -> "Speech service network issue. Offline recognition may be unavailable."
            SpeechRecognizer.ERROR_NO_MATCH -> "No matching command. Try again."
            SpeechRecognizer.ERROR_RECOGNIZER_BUSY -> "Speech recognizer is busy."
            SpeechRecognizer.ERROR_SERVER -> "Speech service error."
            SpeechRecognizer.ERROR_SPEECH_TIMEOUT -> "No speech heard."
            else -> "Speech recognition error."
        }
    }

    override fun onDestroy() {
        stickPollingEnabled = false
        stickPollHandler.removeCallbacks(stickPollRunnable)
        speechRecognizer?.destroy()
        tts?.stop()
        tts?.shutdown()
        super.onDestroy()
    }
}

private enum class VoiceStage {
    MODE_SELECT,
    LANGUAGE_SELECT,
    DASHBOARD,
}

private enum class UserMode(val label: String) {
    BLIND("Blind audio"),
    DEAF("Deaf visual"),
}

private enum class AppLanguage(val label: String) {
    ENGLISH("English"),
    LUGANDA("Luganda"),
    ACHOLI("Acholi"),
}
