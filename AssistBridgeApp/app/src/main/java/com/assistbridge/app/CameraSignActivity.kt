package com.assistbridge.app

import android.Manifest
import android.content.Context
import android.content.pm.PackageManager
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.graphics.Color
import android.graphics.Typeface
import android.graphics.drawable.GradientDrawable
import android.graphics.ImageFormat
import android.graphics.Matrix
import android.graphics.Rect
import android.graphics.YuvImage
import android.os.Bundle
import android.os.SystemClock
import android.speech.tts.TextToSpeech
import android.text.InputFilter
import android.text.InputType
import android.view.View
import android.view.ViewGroup
import android.widget.Button
import android.widget.EditText
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.Space
import android.widget.TextView
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AppCompatActivity
import androidx.camera.core.CameraSelector
import androidx.camera.core.ImageAnalysis
import androidx.camera.core.ImageProxy
import androidx.camera.core.Preview
import androidx.camera.lifecycle.ProcessCameraProvider
import androidx.camera.view.PreviewView
import androidx.core.content.ContextCompat
import com.google.mediapipe.framework.image.BitmapImageBuilder
import com.google.mediapipe.tasks.core.BaseOptions
import com.google.mediapipe.tasks.vision.core.RunningMode
import com.google.mediapipe.tasks.vision.handlandmarker.HandLandmarker
import java.io.ByteArrayOutputStream
import java.util.Locale
import java.util.concurrent.ExecutorService
import java.util.concurrent.Executors
import kotlin.math.abs
import kotlin.math.max
import kotlin.math.sqrt

/*
  Camera Sign Backup

  Uses the Android MediaPipe Hand Landmarker model plus the provided landmark
  training samples in assets/gesture_landmarks.csv. The bundled Python .pkl/.h5
  files are desktop formats, so the app uses the same landmark data with an
  on-device nearest-neighbor classifier and a direct A-Z hand-landmark geometry
  classifier.

  Provided classes:
  - CLOSED/OPEN help identify open-finger hand shapes.
  - ZERO helps identify letter O.
  - THREE is kept as a training sample but is not a full alphabet model.

  This backup mode can identify A-Z from one hand using landmark geometry.
  J and Z need hand movement, so they are detected from recent fingertip motion.
  Accuracy improves when the phone sees the full hand clearly with the palm
  facing the camera.

  User-defined signs are stored only in this app's private phone storage:
  filesDir/user_defined_camera_signs.csv. They do not change the bundled
  general model and they do not affect other phones or app installations.
*/
class CameraSignActivity : AppCompatActivity(), TextToSpeech.OnInitListener {

    private lateinit var previewView: PreviewView
    private lateinit var statusText: TextView
    private lateinit var letterText: TextView
    private lateinit var wordText: TextView
    private lateinit var suggestionText: TextView
    private lateinit var sentenceText: TextView
    private lateinit var sentenceSuggestionText: TextView
    private lateinit var signModeText: TextView
    private lateinit var trainingText: TextView
    private lateinit var trainingLettersInput: EditText
    private lateinit var suggestionButtons: List<Button>
    private lateinit var sentenceSuggestionButtons: List<Button>
    private lateinit var generalSignsButton: Button
    private lateinit var userSignsButton: Button
    private lateinit var trainSignsButton: Button
    private lateinit var captureTrainingButton: Button
    private lateinit var clearUserSignsButton: Button
    private lateinit var calibrateWeakLettersButton: Button
    private lateinit var trainMotionButton: Button
    private lateinit var captureMotionButton: Button
    private lateinit var clearMotionButton: Button
    private lateinit var switchCameraButton: Button
    private lateinit var detectionModeButton: Button

    private val cameraExecutor: ExecutorService = Executors.newSingleThreadExecutor()
    private var handLandmarker: HandLandmarker? = null
    private lateinit var languageModel: AdaptiveSignLanguageModel
    @Volatile
    private var latestLandmarkFeatures: FloatArray? = null
    private var useFrontCamera = false
    private var tts: TextToSpeech? = null
    private var ttsReady = false
    private var gestureSamples: List<GestureSample> = emptyList()
    private var userSignSamples: MutableList<UserSignSample> = mutableListOf()
    private var motionPhraseSamples: MutableList<MotionPhraseSample> = mutableListOf()
    private var signMode = CameraSignMode.GENERAL
    private var trainingActive = false
    private var trainingCaptureActive = false
    private var motionTrainingActive = false
    private var motionCaptureActive = false
    private var trainingLetterIndex = 0
    private var motionPhraseIndex = 0
    private var trainingCapturedForCurrent = 0
    private var trainingPreparedLetter = ""
    private var lastTrainingCaptureAtMs = 0L
    private var motionCaptureStartedAtMs = 0L
    private var lastMotionCaptureFrameAtMs = 0L
    private var lastMotionPhraseSpokenAtMs = 0L
    private var lastMotionPhraseSpoken = ""

    private var candidateLetter = ""
    private var candidateGesture = ""
    private var candidateStartedAtMs = 0L
    private var candidateAcceptedThisHold = false
    private var lastAcceptedHeldLetter = ""
    private var lastLetterSeenAtMs = 0L
    private var lastAppendAtMs = 0L
    private var currentWord = ""
    private var currentSentence = ""
    private val recentIndexTips = ArrayDeque<LandmarkPoint>()
    private val recentPinkyTips = ArrayDeque<LandmarkPoint>()
    private val liveMotionFrames = ArrayDeque<FloatArray>()
    private val motionCaptureFrames = mutableListOf<FloatArray>()

    private val maxWordLength = 32
    private val nearestNeighbors = 5
    private val maxGestureDistance = 0.55f
    private val maxUserGestureDistance = 0.70f
    private val motionWindowSize = 8
    private val motionFrameWindowSize = 18
    private val motionCaptureDurationMs = 1800L
    private val motionCaptureFrameGapMs = 90L
    private val motionPhraseCooldownMs = 2600L
    private val samplesPerUserLetter = 8
    private val minimumWeakLetterSamples = 3
    private val trainingCaptureGapMs = 140L
    private val userSignsFileName = "user_defined_camera_signs.csv"
    private val motionPhrasesFileName = "user_motion_phrases.csv"
    private val alphabetLetters = ('A'..'Z').map { it.toString() }
    private val weakCalibrationLetters = listOf("C", "H", "J", "K", "V", "M", "N", "O", "Q", "S", "U", "X")
    private var activeTrainingLetters = emptyList<String>()
    private var activeTrainingKind = CameraTrainingKind.USER_SELECTED
    private val motionPhraseLabels = listOf("HELLO", "I", "YOU")

    private val cameraPermissionLauncher =
        registerForActivityResult(ActivityResultContracts.RequestPermission()) { granted ->
            if (granted) {
                startCamera()
            } else {
                statusText.text = "Camera permission denied. Use BLE glove mode."
            }
        }

    override fun onCreate(savedInstanceState: Bundle?) {
        ThemeSettings.apply(this)
        super.onCreate(savedInstanceState)
        languageModel = AdaptiveSignLanguageModel(this)
        gestureSamples = loadGestureSamples()
        userSignSamples = loadUserSignSamples().toMutableList()
        motionPhraseSamples = loadMotionPhraseSamples().toMutableList()
        buildUi()
        setupHandLandmarker()
        tts = TextToSpeech(this, this)
        requestCamera()
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

        root.addView(TextView(this).apply {
            setTextColor(textColor)
            textSize = 32f
            typeface = Typeface.DEFAULT_BOLD
            text = "Camera Signs"
        })

        root.addView(TextView(this).apply {
            setTextColor(muted)
            textSize = 16f
            setPadding(0, dp(6), 0, dp(14))
            text = "Use the general alphabet or train only the letters you need for your own hand."
        })

        statusText = TextView(this).apply {
            setTextColor(textColor)
            textSize = 16f
            setPadding(dp(14), dp(12), dp(14), dp(12))
            setBackground(roundedPanel(surface, border))
            text = "Loading hand model and gesture samples..."
            visibility = View.GONE
        }

        signModeText = TextView(this).apply {
            setTextColor(textColor)
            textSize = 18f
            setPadding(dp(14), dp(14), dp(14), dp(14))
            setBackground(roundedPanel(surface, border))
            layoutParams = blockParams(top = 12, bottom = 10)
            visibility = View.GONE
        }

        generalSignsButton = Button(this).apply {
            text = "Use General Signs"
            setOnClickListener { useGeneralSigns() }
            styleButton(this, filled = true)
        }
        root.addView(generalSignsButton)

        userSignsButton = Button(this).apply {
            text = "Use User-Defined Signs"
            setOnClickListener { useUserDefinedSigns() }
            styleButton(this, filled = false)
        }
        root.addView(userSignsButton)

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
            layoutParams = blockParams(top = 2, bottom = 8)
        }
        root.addView(trainingLettersInput)

        trainSignsButton = Button(this).apply {
            text = "Train Selected Letters"
            setOnClickListener { startUserTraining() }
            styleButton(this, filled = false)
        }
        root.addView(trainSignsButton)

        calibrateWeakLettersButton = Button(this).apply {
            text = "Calibrate Weak Letters"
            setOnClickListener { startWeakLetterCalibration() }
            styleButton(this, filled = false)
        }
        root.addView(calibrateWeakLettersButton)

        trainMotionButton = Button(this).apply {
            text = "Train Motion Phrases"
            setOnClickListener { startMotionPhraseTraining() }
            styleButton(this, filled = false)
        }
        root.addView(trainMotionButton)

        trainingText = TextView(this).apply {
            setTextColor(textColor)
            textSize = 18f
            setPadding(dp(14), dp(14), dp(14), dp(14))
            setBackground(roundedPanel(surface, border))
            layoutParams = blockParams(top = 10, bottom = 10)
        }
        root.addView(trainingText)

        captureTrainingButton = Button(this).apply {
            text = "Capture Current Letter"
            setOnClickListener { beginTrainingCapture() }
            styleButton(this, filled = true)
        }
        root.addView(captureTrainingButton)

        captureMotionButton = Button(this).apply {
            text = "Capture Motion Phrase"
            setOnClickListener { beginMotionPhraseCapture() }
            styleButton(this, filled = true)
        }
        root.addView(captureMotionButton)

        clearUserSignsButton = Button(this).apply {
            text = "Clear User-Defined Signs"
            setOnClickListener { clearUserDefinedSigns() }
            styleButton(this, filled = false)
        }
        root.addView(clearUserSignsButton)

        clearMotionButton = Button(this).apply {
            text = "Clear Motion Phrases"
            setOnClickListener { clearMotionPhrases() }
            styleButton(this, filled = false)
        }
        root.addView(clearMotionButton)

        detectionModeButton = Button(this).apply {
            text = languageModel.modeTitle()
            setOnClickListener {
                languageModel.cycleMode()
                resetHeldLetterState()
                statusText.text = languageModel.modeDescription()
                updateModeUi()
                updateTextOutputs()
            }
            styleButton(this, filled = false)
        }
        root.addView(detectionModeButton)

        previewView = PreviewView(this).apply {
            setBackgroundResource(R.drawable.preview_frame)
            implementationMode = PreviewView.ImplementationMode.COMPATIBLE
            layoutParams = LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                dp(360),
            ).apply {
                bottomMargin = dp(12)
            }
        }
        root.addView(previewView)

        root.addView(sectionLabel("Live Output", textColor))

        letterText = metric("Camera letter", "...", surface, textColor)
        wordText = metric("Word", "...", surface, textColor)
        suggestionText = metric("Available words", "Detected letters will appear here", surface, textColor)
        sentenceText = metric("Sentence", "...", surface, textColor)
        sentenceSuggestionText = metric("Suggested sentences", "Detected words will appear here", surface, textColor)

        root.addView(letterText)
        root.addView(wordText)
        root.addView(suggestionText)

        suggestionButtons = List(3) {
            Button(this).apply {
                visibility = View.GONE
                styleButton(this, filled = false)
                setOnClickListener {
                    val suggestion = text.toString().trim()
                    if (suggestion.isNotBlank()) {
                        currentWord = suggestion
                        languageModel.recordWordCommitted(suggestion)
                        statusText.text = "$suggestion selected. Commit when ready."
                        updateTextOutputs()
                    }
                }
                root.addView(this)
            }
        }

        root.addView(sentenceText)
        root.addView(sentenceSuggestionText)

        sentenceSuggestionButtons = List(3) {
            Button(this).apply {
                visibility = View.GONE
                styleButton(this, filled = false)
                setOnClickListener {
                    val suggestion = text.toString().trim()
                    if (suggestion.isNotBlank()) {
                        currentSentence = suggestion
                        languageModel.recordSentenceChosen(suggestion)
                        updateTextOutputs()
                        speak(suggestion)
                    }
                }
                root.addView(this)
            }
        }

        root.addView(Button(this).apply {
            text = "Commit Word and Speak"
            styleButton(this, filled = true)
            setOnClickListener { commitWordAndSpeak() }
        })
        root.addView(Button(this).apply {
            text = "Backspace Letter"
            styleButton(this, filled = false)
            setOnClickListener {
                if (currentWord.isNotEmpty()) {
                    currentWord = currentWord.dropLast(1)
                    languageModel.recordCorrection()
                    resetHeldLetterState()
                    updateTextOutputs()
                }
            }
        })
        root.addView(Button(this).apply {
            text = "Clear Word"
            styleButton(this, filled = false)
            setOnClickListener {
                if (currentWord.isNotEmpty()) {
                    languageModel.recordCorrection()
                }
                currentWord = ""
                resetHeldLetterState()
                updateTextOutputs()
            }
        })
        root.addView(Button(this).apply {
            text = "Speak Sentence"
            styleButton(this, filled = true)
            setOnClickListener { speak(currentSentence.ifBlank { currentWord }) }
        })
        root.addView(Button(this).apply {
            text = "Clear Sentence"
            styleButton(this, filled = false)
            setOnClickListener {
                if (currentSentence.isNotEmpty()) {
                    languageModel.recordCorrection()
                }
                currentSentence = ""
                updateTextOutputs()
            }
        })

        switchCameraButton = Button(this).apply {
            text = "Switch Camera"
            styleButton(this, filled = false)
            setOnClickListener {
                useFrontCamera = !useFrontCamera
                startCamera()
            }
        }
        root.addView(switchCameraButton)

        root.addView(Space(this).apply {
            layoutParams = LinearLayout.LayoutParams(1, dp(8))
        })

        root.addView(Button(this).apply {
            text = "Back"
            styleButton(this, filled = false)
            setOnClickListener { finish() }
        })

        scroll.addView(root)
        setContentView(scroll)
        updateModeUi()
        updateTextOutputs()
    }

    private fun metric(label: String, value: String, surface: Int, textColor: Int): TextView {
        val border = ContextCompat.getColor(this, R.color.assist_border)
        return TextView(this).apply {
            setTextColor(textColor)
            textSize = 24f
            typeface = Typeface.DEFAULT_BOLD
            setPadding(dp(14), dp(14), dp(14), dp(14))
            setBackground(roundedPanel(surface, border))
            text = "$label\n$value"
            setLineSpacing(2f, 1.0f)
            layoutParams = blockParams(bottom = 10)
        }
    }

    private fun sectionLabel(value: String, textColor: Int): TextView {
        return TextView(this).apply {
            setTextColor(textColor)
            textSize = 18f
            typeface = Typeface.DEFAULT_BOLD
            text = value
            setPadding(0, dp(6), 0, dp(8))
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

    private fun styleSelectionButton(button: Button, selected: Boolean) {
        val primary = ContextCompat.getColor(this, R.color.assist_primary)
        val border = ContextCompat.getColor(this, R.color.assist_border)
        val surface = ContextCompat.getColor(this, R.color.assist_surface)
        val textColor = ContextCompat.getColor(this, R.color.assist_text)
        val onPrimary = ContextCompat.getColor(this, R.color.assist_on_primary)

        button.isSelected = selected
        button.setTextColor(if (selected) onPrimary else textColor)
        button.setBackground(roundedPanel(if (selected) primary else surface, if (selected) primary else border))
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

    private fun setupHandLandmarker() {
        try {
            val baseOptions = BaseOptions.builder()
                .setModelAssetPath("hand_landmarker.task")
                .build()
            val options = HandLandmarker.HandLandmarkerOptions.builder()
                .setBaseOptions(baseOptions)
                .setRunningMode(RunningMode.VIDEO)
                .setNumHands(1)
                .setMinHandDetectionConfidence(0.45f)
                .setMinHandPresenceConfidence(0.45f)
                .setMinTrackingConfidence(0.45f)
                .build()
            handLandmarker = HandLandmarker.createFromOptions(this, options)
            statusText.text = "Camera alphabet ready. Hold one sign steady."
        } catch (error: Exception) {
            handLandmarker = null
            statusText.text = "Hand model failed: ${error.message.orEmpty()}"
        }
    }

    private fun requestCamera() {
        if (ContextCompat.checkSelfPermission(this, Manifest.permission.CAMERA) == PackageManager.PERMISSION_GRANTED) {
            startCamera()
        } else {
            cameraPermissionLauncher.launch(Manifest.permission.CAMERA)
        }
    }

    private fun startCamera() {
        val cameraProviderFuture = ProcessCameraProvider.getInstance(this)
        cameraProviderFuture.addListener(
            {
                try {
                    val cameraProvider = cameraProviderFuture.get()
                    val preview = Preview.Builder().build().also {
                        it.setSurfaceProvider(previewView.surfaceProvider)
                    }
                    val analysis = ImageAnalysis.Builder()
                        .setBackpressureStrategy(ImageAnalysis.STRATEGY_KEEP_ONLY_LATEST)
                        .build()
                        .also {
                            it.setAnalyzer(cameraExecutor) { image ->
                                try {
                                    val prediction = detectCameraGesture(image)
                                    runOnUiThread { processDetectedPrediction(prediction) }
                                } finally {
                                    image.close()
                                }
                            }
                        }

                    val selector = if (useFrontCamera) {
                        CameraSelector.DEFAULT_FRONT_CAMERA
                    } else {
                        CameraSelector.DEFAULT_BACK_CAMERA
                    }

                    cameraProvider.unbindAll()
                    cameraProvider.bindToLifecycle(this, selector, preview, analysis)
                    val modelStatus = if (handLandmarker == null) {
                        "Model not ready."
                    } else if (gestureSamples.isEmpty()) {
                        "A-Z geometry active."
                    } else {
                        "A-Z geometry active with ${gestureSamples.size} calibration samples."
                    }
                    statusText.text =
                        if (useFrontCamera) {
                            "Front camera active. $modelStatus Hold sign steady."
                        } else {
                            "Back camera active. $modelStatus Hold sign steady."
                        }
                } catch (error: Exception) {
                    statusText.text = "Camera failed: ${error.message.orEmpty()}"
                }
            },
            ContextCompat.getMainExecutor(this),
        )
    }

    private fun processDetectedPrediction(prediction: CameraGesturePrediction?) {
        if (motionTrainingActive) {
            processMotionPhraseTrainingFrame()
            return
        }

        if (trainingActive) {
            processTrainingFrame()
            return
        }

        val now = SystemClock.elapsedRealtime()

        if (prediction == null) {
            if (now - lastLetterSeenAtMs > languageModel.releaseResetMs()) {
                lastAcceptedHeldLetter = ""
                candidateLetter = ""
                candidateGesture = ""
                candidateAcceptedThisHold = false
            }
            letterText.text = "Camera letter\n..."
            return
        }

        val phrase = prediction.spokenPhrase
        if (!phrase.isNullOrBlank()) {
            processMotionPhrasePrediction(phrase, prediction)
            return
        }

        val letter = prediction.letter
        if (letter != candidateLetter) {
            candidateLetter = letter
            candidateGesture = prediction.gesture
            candidateStartedAtMs = now
            candidateAcceptedThisHold = false
        }

        lastLetterSeenAtMs = now
        val heldMs = now - candidateStartedAtMs
        val decision = languageModel.timingDecision(
            prefix = currentWord,
            candidateLetter = letter,
            confidencePercent = prediction.confidencePercent,
            nowMs = now,
            lastAppendAtMs = lastAppendAtMs,
            sentenceContext = currentSentence,
            sameAsLastAccepted = letter == lastAcceptedHeldLetter,
        )
        val canAppend =
            heldMs >= decision.requiredHoldMs &&
                !candidateAcceptedThisHold &&
                letter != lastAcceptedHeldLetter &&
                now - lastAppendAtMs >= decision.minimumAppendGapMs

        if (canAppend) {
            appendLetter(letter)
            languageModel.recordAcceptedLetter(heldMs, now - lastAppendAtMs)
            candidateAcceptedThisHold = true
            lastAcceptedHeldLetter = letter
            lastAppendAtMs = now
            letterText.text = "Camera letter\n$letter accepted"
            statusText.text = "${prediction.gesture} -> $letter added (${prediction.confidencePercent}%). ${decision.detail}."
        } else {
            val state = if (heldMs < decision.requiredHoldMs) {
                "hold ${decision.requiredHoldMs - heldMs} ms"
            } else {
                "held"
            }
            letterText.text =
                "Camera letter\n$letter $state\n${prediction.gesture} ${prediction.confidencePercent}%\n${decision.detail}"
        }
    }

    private fun appendLetter(letter: String) {
        if (currentWord.length >= maxWordLength) {
            currentWord = currentWord.drop(1)
        }

        currentWord += letter
        updateTextOutputs()
    }

    private fun processMotionPhrasePrediction(phrase: String, prediction: CameraGesturePrediction) {
        val now = SystemClock.elapsedRealtime()
        if (phrase == lastMotionPhraseSpoken && now - lastMotionPhraseSpokenAtMs < motionPhraseCooldownMs) {
            letterText.text = "Motion phrase\n$phrase held\n${prediction.confidencePercent}%"
            return
        }

        currentSentence = listOf(currentSentence, phrase)
            .filter { it.isNotBlank() }
            .joinToString(" ")
            .trim()
        lastMotionPhraseSpoken = phrase
        lastMotionPhraseSpokenAtMs = now
        resetHeldLetterState()
        updateTextOutputs()
        letterText.text = "Motion phrase\n$phrase accepted"
        statusText.text = "Motion phrase $phrase detected (${prediction.confidencePercent}%)."
        languageModel.recordSentenceChosen(phrase)
        speak(phrase.lowercase(Locale.US))
    }

    private fun commitWordAndSpeak() {
        val word = cleanLetters(currentWord)
        if (word.isBlank()) {
            statusText.text = "No word to speak."
            return
        }

        if (languageModel.isSentenceOnly(currentSentence, word)) {
            val context = languageModel.cleanSentence(listOf(currentSentence, word).joinToString(" "))
            val suggestion = languageModel.buildSentenceSuggestions(context, word, limit = 1).firstOrNull()
            if (suggestion == null) {
                statusText.text = "Sentence-only mode needs more letters before speaking."
                return
            }

            currentSentence = suggestion
            currentWord = ""
            languageModel.recordSentenceChosen(suggestion)
            updateTextOutputs()
            speak(suggestion)
            return
        }

        currentSentence = listOf(currentSentence, word)
            .filter { it.isNotBlank() }
            .joinToString(" ")
            .trim()
        languageModel.recordWordCommitted(word)
        currentWord = ""
        updateTextOutputs()
        speak(word)
    }

    private fun useGeneralSigns() {
        signMode = CameraSignMode.GENERAL
        trainingActive = false
        trainingCaptureActive = false
        motionTrainingActive = false
        motionCaptureActive = false
        resetHeldLetterState()
        statusText.text = "General camera signs selected."
        updateModeUi()
    }

    private fun useUserDefinedSigns() {
        signMode = CameraSignMode.USER_DEFINED
        resetHeldLetterState()
        trainingActive = false
        trainingCaptureActive = false
        statusText.text =
            if (userSignSamples.isEmpty()) {
                "User-defined signs selected. Type the letters you want to train, then press Train Selected Letters."
            } else {
                "User-defined signs selected. This model is stored only on this phone."
            }
        updateModeUi()
    }

    private fun startUserTraining() {
        val requestedLetters = requestedTrainingLetters()
        if (requestedLetters.isEmpty()) {
            trainingActive = false
            trainingCaptureActive = false
            activeTrainingLetters = emptyList()
            statusText.text = "Type one or more letters to train first. Example: A C Y."
            updateModeUi()
            return
        }

        signMode = CameraSignMode.USER_DEFINED
        activeTrainingKind = CameraTrainingKind.USER_SELECTED
        activeTrainingLetters = requestedLetters
        trainingActive = true
        trainingCaptureActive = false
        motionTrainingActive = false
        motionCaptureActive = false
        trainingPreparedLetter = ""
        trainingLetterIndex = firstIncompleteUserLetterIndex().takeIf { it >= 0 } ?: 0
        trainingCapturedForCurrent = userSampleCount(currentTrainingLetter())
        resetHeldLetterState()
        statusText.text =
            "Training selected letters. Hold your sign for ${currentTrainingLetter()}, then press Capture Current Letter."
        updateModeUi()
    }

    private fun startWeakLetterCalibration() {
        activeTrainingKind = CameraTrainingKind.WEAK_LETTERS
        activeTrainingLetters = weakCalibrationLetters
        trainingActive = true
        trainingCaptureActive = false
        motionTrainingActive = false
        motionCaptureActive = false
        trainingPreparedLetter = ""
        trainingLetterIndex = firstIncompleteUserLetterIndex().takeIf { it >= 0 } ?: 0
        trainingCapturedForCurrent = userSampleCount(currentTrainingLetter())
        resetHeldLetterState()
        statusText.text =
            "Weak-letter calibration. Hold ${currentTrainingLetter()} clearly, then press Capture Current Letter."
        speak("Hold the sign for ${currentTrainingLetter()}, then press capture current letter.")
        updateModeUi()
    }

    private fun startMotionPhraseTraining() {
        trainingActive = false
        trainingCaptureActive = false
        motionTrainingActive = true
        motionCaptureActive = false
        motionPhraseIndex = firstIncompleteMotionPhraseIndex().takeIf { it >= 0 } ?: 0
        motionCaptureFrames.clear()
        liveMotionFrames.clear()
        resetHeldLetterState()
        val phrase = currentMotionPhrase()
        statusText.text = "Motion training. Press Capture Motion Phrase, then make the motion for $phrase."
        speak("Press capture motion phrase, then make your motion for $phrase.")
        updateModeUi()
    }

    private fun beginTrainingCapture() {
        if (!trainingActive) {
            startUserTraining()
            if (!trainingActive) {
                return
            }
        }

        val letter = currentTrainingLetter()
        if (trainingPreparedLetter != letter) {
            userSignSamples.removeAll { it.letter == letter }
            trainingCapturedForCurrent = 0
            trainingPreparedLetter = letter
            saveUserSignSamples()
        }

        trainingCaptureActive = true
        lastTrainingCaptureAtMs = 0L
        resetHeldLetterState()
        val requiredSamples =
            if (activeTrainingKind == CameraTrainingKind.WEAK_LETTERS) {
                minimumWeakLetterSamples
            } else {
                samplesPerUserLetter
            }
        statusText.text = "Capturing $letter. Keep the hand steady until $requiredSamples samples are saved."
        updateModeUi()
    }

    private fun beginMotionPhraseCapture() {
        if (!motionTrainingActive) {
            startMotionPhraseTraining()
        }

        val phrase = currentMotionPhrase()
        motionPhraseSamples.removeAll { it.phrase == phrase }
        saveMotionPhraseSamples()
        motionCaptureFrames.clear()
        liveMotionFrames.clear()
        motionCaptureActive = true
        motionCaptureStartedAtMs = SystemClock.elapsedRealtime()
        lastMotionCaptureFrameAtMs = 0L
        resetHeldLetterState()
        statusText.text = "Capturing $phrase. Start the motion now and keep the hand in the camera."
        speak("Start $phrase now.")
        updateModeUi()
    }

    private fun processTrainingFrame() {
        val letter = currentTrainingLetter()

        if (!trainingCaptureActive) {
            letterText.text = "Camera letter\nTraining $letter"
            updateModeUi()
            return
        }

        val features = latestLandmarkFeatures
        if (features == null) {
            letterText.text = "Camera letter\nNo hand detected for $letter"
            statusText.text = "Put the full hand in the camera frame for $letter."
            updateModeUi()
            return
        }

        val now = SystemClock.elapsedRealtime()
        if (now - lastTrainingCaptureAtMs < trainingCaptureGapMs) {
            return
        }

        val requiredSamples =
            if (activeTrainingKind == CameraTrainingKind.WEAK_LETTERS) {
                minimumWeakLetterSamples
            } else {
                samplesPerUserLetter
            }
        lastTrainingCaptureAtMs = now
        userSignSamples.add(UserSignSample(letter, features.copyOf()))
        trainingCapturedForCurrent = userSampleCount(letter)
        letterText.text = "Camera letter\nTraining $letter\n$trainingCapturedForCurrent/$requiredSamples samples"

        if (trainingCapturedForCurrent >= requiredSamples) {
            finishCurrentTrainingLetter()
        } else {
            updateModeUi()
        }
    }

    private fun processMotionPhraseTrainingFrame() {
        val phrase = currentMotionPhrase()

        if (!motionCaptureActive) {
            letterText.text = "Motion phrase\nTraining $phrase"
            updateModeUi()
            return
        }

        val features = latestLandmarkFeatures
        if (features == null) {
            letterText.text = "Motion phrase\nNo hand detected for $phrase"
            statusText.text = "Put the full hand in the camera frame for $phrase."
            updateModeUi()
            return
        }

        val now = SystemClock.elapsedRealtime()
        if (now - lastMotionCaptureFrameAtMs >= motionCaptureFrameGapMs) {
            lastMotionCaptureFrameAtMs = now
            motionCaptureFrames.add(features.copyOf())
        }

        val remainingMs = (motionCaptureDurationMs - (now - motionCaptureStartedAtMs)).coerceAtLeast(0L)
        letterText.text = "Motion phrase\nTraining $phrase\n${remainingMs / 1000.0f}s left"

        if (now - motionCaptureStartedAtMs < motionCaptureDurationMs) {
            updateModeUi()
            return
        }

        val signature = buildMotionSignature(motionCaptureFrames)
        if (signature == null) {
            statusText.text = "$phrase was not saved. Move the hand clearly and keep it in frame."
            motionCaptureActive = false
            updateModeUi()
            return
        }

        motionPhraseSamples.add(MotionPhraseSample(phrase, signature))
        saveMotionPhraseSamples()
        motionCaptureActive = false

        val nextIndex = nextIncompleteMotionPhraseIndex(motionPhraseIndex + 1)
        if (nextIndex >= 0) {
            motionPhraseIndex = nextIndex
            statusText.text = "$phrase saved. Now press Capture Motion Phrase and make ${currentMotionPhrase()}."
            speak("$phrase saved. Next, ${currentMotionPhrase()}.")
        } else {
            motionTrainingActive = false
            statusText.text = "Motion phrases ready. The camera can now speak hello, I, and you from your trained motions."
            speak("Motion phrases ready.")
        }

        updateModeUi()
    }

    private fun finishCurrentTrainingLetter() {
        val savedLetter = currentTrainingLetter()
        saveUserSignSamples()
        trainingCaptureActive = false
        trainingPreparedLetter = ""

        val nextIndex = nextIncompleteUserLetterIndex(trainingLetterIndex + 1)
        if (nextIndex >= 0) {
            trainingLetterIndex = nextIndex
            trainingCapturedForCurrent = userSampleCount(currentTrainingLetter())
            statusText.text =
                "$savedLetter saved. Now hold your sign for ${currentTrainingLetter()} and press Capture Current Letter."
        } else {
            trainingActive = false
            if (activeTrainingKind == CameraTrainingKind.USER_SELECTED) {
                signMode = CameraSignMode.USER_DEFINED
                statusText.text = "Selected user-defined letters are ready. Recognition now uses your trained signs."
            } else {
                signMode = CameraSignMode.GENERAL
                statusText.text = "Weak-letter calibration ready. General signs will use your saved weak-letter samples first."
            }
        }

        updateModeUi()
    }

    private fun clearUserDefinedSigns() {
        userSignSamples.clear()
        saveUserSignSamples()
        signMode = CameraSignMode.GENERAL
        trainingActive = false
        trainingCaptureActive = false
        motionTrainingActive = false
        motionCaptureActive = false
        trainingPreparedLetter = ""
        resetHeldLetterState()
        statusText.text = "User-defined signs cleared. General signs selected."
        updateModeUi()
    }

    private fun clearMotionPhrases() {
        motionPhraseSamples.clear()
        saveMotionPhraseSamples()
        motionTrainingActive = false
        motionCaptureActive = false
        motionCaptureFrames.clear()
        liveMotionFrames.clear()
        lastMotionPhraseSpoken = ""
        lastMotionPhraseSpokenAtMs = 0L
        statusText.text = "Motion phrases cleared."
        updateModeUi()
    }

    private fun updateModeUi() {
        val trainedUserLetters = alphabetLetters.filter { userSampleCount(it) >= samplesPerUserLetter }
        val trainedLetterList = trainedUserLetters.joinToString(", ").ifBlank { "none" }
        val weakReady = weakCalibrationLetters.count { userSampleCount(it) >= minimumWeakLetterSamples }
        val trainedMotions = motionPhraseLabels.count { motionPhraseSampleCount(it) > 0 }
        val totalSamples = userSignSamples.size
        val modeName =
            if (signMode == CameraSignMode.USER_DEFINED) {
                "User-defined signs"
            } else {
                "General signs"
            }

        signModeText.text =
            "Sign mode: $modeName\nUser-defined letters trained: ${trainedUserLetters.size} ($trainedLetterList), $totalSamples samples\n" +
                "Weak letters usable: $weakReady/${weakCalibrationLetters.size} ($minimumWeakLetterSamples+ samples each)\n" +
                "Motion phrases trained: $trainedMotions/${motionPhraseLabels.size}\n" +
                "Custom signs stay only on this phone.\n${languageModel.modeDescription()}"
        detectionModeButton.text = languageModel.modeTitle()
        styleSelectionButton(generalSignsButton, signMode == CameraSignMode.GENERAL)
        styleSelectionButton(userSignsButton, signMode == CameraSignMode.USER_DEFINED)

        trainingText.visibility =
            if (trainingActive || motionTrainingActive || userSignSamples.isNotEmpty() || motionPhraseSamples.isNotEmpty()) {
                View.VISIBLE
            } else {
                View.GONE
            }
        captureTrainingButton.visibility = if (trainingActive) View.VISIBLE else View.GONE
        captureMotionButton.visibility = if (motionTrainingActive) View.VISIBLE else View.GONE
        clearUserSignsButton.visibility = if (userSignSamples.isNotEmpty()) View.VISIBLE else View.GONE
        clearMotionButton.visibility = if (motionPhraseSamples.isNotEmpty()) View.VISIBLE else View.GONE

        trainingText.text =
            if (motionTrainingActive) {
                val phrase = currentMotionPhrase()
                "Training motion phrase $phrase\nSaved: ${motionPhraseSampleCount(phrase)} sample\n" +
                    "Press Capture Motion Phrase, then make the presentation motion clearly."
            } else if (trainingActive) {
                val letter = currentTrainingLetter()
                val requiredSamples =
                    if (activeTrainingKind == CameraTrainingKind.WEAK_LETTERS) {
                        minimumWeakLetterSamples
                    } else {
                        samplesPerUserLetter
                    }
                val scopeText =
                    if (activeTrainingKind == CameraTrainingKind.WEAK_LETTERS) {
                        "Weak letters: ${weakCalibrationLetters.joinToString(", ")}"
                    } else {
                        "Selected letters: ${activeTrainingLetters.joinToString(", ")}"
                    }
                "Training letter $letter\nCaptured: ${userSampleCount(letter)}/$requiredSamples\n" +
                    "Make your own sign for $letter. Press capture and hold steady.\n" +
                    scopeText
            } else if (userSignSamples.isNotEmpty() || motionPhraseSamples.isNotEmpty()) {
                "Local calibration is saved on this phone. Weak letters with $minimumWeakLetterSamples or more samples and motion phrases are used before the generic camera rules."
            } else {
                "No user-defined signs yet. Type the letters you want to train, then press Train Selected Letters."
            }

        captureTrainingButton.text =
            if (trainingCaptureActive) {
                "Capturing ${currentTrainingLetter()}..."
            } else {
                "Capture Samples for ${currentTrainingLetter()}"
            }

        captureMotionButton.text =
            if (motionCaptureActive) {
                "Capturing ${currentMotionPhrase()}..."
            } else {
                "Capture Motion for ${currentMotionPhrase()}"
            }
    }

    private fun resetHeldLetterState() {
        candidateLetter = ""
        candidateGesture = ""
        candidateStartedAtMs = 0L
        candidateAcceptedThisHold = false
        lastAcceptedHeldLetter = ""
        lastLetterSeenAtMs = 0L
        lastAppendAtMs = 0L
    }

    private fun updateTextOutputs() {
        val cleanWord = cleanLetters(currentWord)
        val cleanSentence = currentSentence.trim()
        val wordLabel = if (languageModel.isSentenceOnly(cleanSentence, cleanWord)) "Sentence input" else "Word"
        wordText.text = "$wordLabel\n${cleanWord.ifBlank { "..." }}"
        sentenceText.text = "Sentence\n${cleanSentence.ifBlank { "..." }}"
        updateWordSuggestions(cleanWord)
        updateSentenceSuggestions(cleanSentence, cleanWord)
    }

    private fun updateWordSuggestions(prefix: String) {
        if (languageModel.isSentenceOnly(currentSentence, prefix)) {
            suggestionText.text = "Available words\nSentence-only mode is active. Use full sentence suggestions below."
            suggestionButtons.forEach { button -> button.visibility = View.GONE }
            return
        }

        val suggestions = languageModel.buildWordSuggestions(prefix)

        suggestionText.text = when {
            prefix.isBlank() -> "Available words\nDetected letters will appear here"
            suggestions.isEmpty() -> "Available words\nNo listed word matches $prefix"
            suggestions.contains(prefix) -> "Available words\nDetected: $prefix\n$prefix is available"
            else -> "Available words\nDetected: $prefix\n${suggestions.joinToString(", ")}"
        }

        suggestionButtons.forEachIndexed { index, button ->
            val suggestion = suggestions.getOrNull(index)
            if (suggestion == null) {
                button.visibility = View.GONE
            } else {
                button.text = suggestion
                button.visibility = View.VISIBLE
            }
        }
    }

    private fun updateSentenceSuggestions(sentence: String, word: String) {
        val context = languageModel.cleanSentence(listOf(sentence, word).joinToString(" "))
        val suggestions = if (context.isBlank()) emptyList() else languageModel.buildSentenceSuggestions(context, word)

        sentenceSuggestionText.text = when {
            context.isBlank() -> "Suggested sentences\nDetected words or sentence letters will appear here"
            suggestions.isEmpty() -> "Suggested sentences\nNo sentence matches $context yet"
            else -> "Suggested sentences\n${suggestions.joinToString("\n")}"
        }

        sentenceSuggestionButtons.forEachIndexed { index, button ->
            val suggestion = suggestions.getOrNull(index)
            if (suggestion == null) {
                button.visibility = View.GONE
            } else {
                button.text = suggestion
                button.visibility = View.VISIBLE
            }
        }
    }

    private fun detectCameraGesture(image: ImageProxy): CameraGesturePrediction? {
        val landmarker = handLandmarker ?: run {
            latestLandmarkFeatures = null
            return null
        }

        val bitmap = imageProxyToBitmap(image)
        val mpImage = BitmapImageBuilder(bitmap).build()
        val result = try {
            landmarker.detectForVideo(mpImage, SystemClock.uptimeMillis())
        } catch (_: Exception) {
            latestLandmarkFeatures = null
            return null
        }

        val landmarks = result.landmarks().firstOrNull() ?: run {
            latestLandmarkFeatures = null
            return null
        }
        if (landmarks.size < 21) {
            latestLandmarkFeatures = null
            return null
        }

        val features = FloatArray(42)
        for (i in 0 until 21) {
            features[i * 2] = landmarks[i].x()
            features[i * 2 + 1] = landmarks[i].y()
        }

        val normalized = normalizeLandmarks(features)
        latestLandmarkFeatures = normalized.copyOf()
        return classifyLandmarks(normalized)
    }

    private fun imageProxyToBitmap(image: ImageProxy): Bitmap {
        val nv21 = yuv420ToNv21(image)
        val output = ByteArrayOutputStream()
        val yuvImage = YuvImage(nv21, ImageFormat.NV21, image.width, image.height, null)
        yuvImage.compressToJpeg(Rect(0, 0, image.width, image.height), 80, output)
        val jpegBytes = output.toByteArray()
        val bitmap = BitmapFactory.decodeByteArray(jpegBytes, 0, jpegBytes.size)
        val rotation = image.imageInfo.rotationDegrees
        if (rotation == 0) {
            return bitmap
        }

        val matrix = Matrix().apply { postRotate(rotation.toFloat()) }
        return Bitmap.createBitmap(bitmap, 0, 0, bitmap.width, bitmap.height, matrix, true)
    }

    private fun yuv420ToNv21(image: ImageProxy): ByteArray {
        val width = image.width
        val height = image.height
        val ySize = width * height
        val uvSize = width * height / 2
        val nv21 = ByteArray(ySize + uvSize)

        val yPlane = image.planes[0]
        val uPlane = image.planes[1]
        val vPlane = image.planes[2]

        var outputOffset = 0
        for (row in 0 until height) {
            val rowOffset = row * yPlane.rowStride
            for (col in 0 until width) {
                nv21[outputOffset++] = yPlane.buffer.get(rowOffset + col * yPlane.pixelStride)
            }
        }

        outputOffset = ySize
        val chromaHeight = height / 2
        val chromaWidth = width / 2
        for (row in 0 until chromaHeight) {
            val uRowOffset = row * uPlane.rowStride
            val vRowOffset = row * vPlane.rowStride
            for (col in 0 until chromaWidth) {
                nv21[outputOffset++] = vPlane.buffer.get(vRowOffset + col * vPlane.pixelStride)
                nv21[outputOffset++] = uPlane.buffer.get(uRowOffset + col * uPlane.pixelStride)
            }
        }

        return nv21
    }

    private fun normalizeLandmarks(raw: FloatArray): FloatArray {
        var minX = Float.MAX_VALUE
        var minY = Float.MAX_VALUE
        var maxX = -Float.MAX_VALUE
        var maxY = -Float.MAX_VALUE
        for (i in 0 until 21) {
            val x = raw[i * 2]
            val y = raw[i * 2 + 1]
            minX = minOf(minX, x)
            minY = minOf(minY, y)
            maxX = maxOf(maxX, x)
            maxY = maxOf(maxY, y)
        }

        val wristX = raw[0]
        val wristY = raw[1]
        val scale = sqrt((maxX - minX) * (maxX - minX) + (maxY - minY) * (maxY - minY))
            .coerceAtLeast(0.0001f)
        val normalized = FloatArray(42)
        for (i in 0 until 21) {
            normalized[i * 2] = (raw[i * 2] - wristX) / scale
            normalized[i * 2 + 1] = (raw[i * 2 + 1] - wristY) / scale
        }
        return normalized
    }

    private fun classifyLandmarks(features: FloatArray): CameraGesturePrediction? {
        rememberLiveMotionFrame(features)
        if (!trainingActive && !motionTrainingActive) {
            classifyTrainedMotionPhrase()?.let { return it }
        }

        val weakLetterOverride =
            classifyUserDefinedGesture(
                features = features,
                requireCompleteAlphabet = false,
                allowedLetters = weakCalibrationLetters.toSet(),
            )

        return if (signMode == CameraSignMode.USER_DEFINED) {
            classifyUserDefinedGesture(features, requireCompleteAlphabet = false)
        } else {
            weakLetterOverride ?: classifyAlphabetGeometry(features) ?: classifyProvidedGesture(features)
        }
    }

    private fun classifyUserDefinedGesture(
        features: FloatArray,
        requireCompleteAlphabet: Boolean,
        allowedLetters: Set<String>? = null,
    ): CameraGesturePrediction? {
        if (requireCompleteAlphabet && !hasCompleteUserAlphabet()) {
            return null
        }

        val eligibleLetters = userSignSamples
            .asSequence()
            .map { it.letter }
            .distinct()
            .filter { letter -> allowedLetters == null || letter in allowedLetters }
            .filter { letter ->
                val requiredSamples =
                    if (allowedLetters == null || requireCompleteAlphabet) {
                        samplesPerUserLetter
                    } else {
                        minimumWeakLetterSamples
                    }
                userSampleCount(letter) >= requiredSamples
            }
            .toSet()
        if (eligibleLetters.isEmpty()) {
            return null
        }

        val nearest = userSignSamples
            .asSequence()
            .filter { it.letter in eligibleLetters }
            .map { sample -> sample to euclideanDistance(features, sample.features) }
            .sortedBy { it.second }
            .take(nearestNeighbors)
            .toList()

        val closestDistance = nearest.firstOrNull()?.second ?: return null
        val winner = nearest
            .groupBy { it.first.letter }
            .mapValues { entry -> entry.value.sumOf { 1.0 / max(it.second.toDouble(), 0.0001) } }
            .maxByOrNull { it.value }
            ?.key ?: return null
        val calibratedLimit = trainedLetterDistanceLimit(winner, allowedLetters != null)
        if (closestDistance > calibratedLimit) {
            return null
        }

        val groupedWeights = nearest
            .groupBy { it.first.letter }
            .mapValues { entry -> entry.value.sumOf { 1.0 / max(it.second.toDouble(), 0.0001) } }
        val winnerWeight = groupedWeights[winner] ?: return null
        val runnerUpWeight = groupedWeights
            .filterKeys { it != winner }
            .values
            .maxOrNull() ?: 0.0
        if (runnerUpWeight > 0.0 && winnerWeight < runnerUpWeight * 1.08) {
            return null
        }

        val confidence = ((1f - (closestDistance / calibratedLimit)).coerceIn(0f, 1f) * 100f).toInt()
        return CameraGesturePrediction("trained-$winner", winner, confidence)
    }

    private fun trainedLetterDistanceLimit(letter: String, weakOverride: Boolean): Float {
        val samples = userSignSamples.filter { it.letter == letter }
        if (samples.size < 2) {
            return if (weakOverride) 0.34f else 0.28f
        }

        val centroid = FloatArray(samples.first().features.size)
        samples.forEach { sample ->
            for (i in centroid.indices) {
                centroid[i] += sample.features[i]
            }
        }
        for (i in centroid.indices) {
            centroid[i] /= samples.size.toFloat()
        }

        val spread = samples
            .map { sample -> euclideanDistance(sample.features, centroid) }
            .average()
            .toFloat()
        val base = if (weakOverride) 0.30f else 0.24f
        val upper = if (weakOverride) maxUserGestureDistance else 0.58f
        return (base + spread * 3.2f).coerceIn(base, upper)
    }

    private fun classifyProvidedGesture(features: FloatArray): CameraGesturePrediction? {
        if (gestureSamples.isEmpty()) {
            return null
        }

        val nearest = gestureSamples
            .asSequence()
            .map { sample -> sample to euclideanDistance(features, sample.features) }
            .sortedBy { it.second }
            .take(nearestNeighbors)
            .toList()

        val closestDistance = nearest.firstOrNull()?.second ?: return null
        if (closestDistance > maxGestureDistance) {
            return null
        }

        val winner = nearest
            .groupBy { it.first.labelId }
            .mapValues { entry -> entry.value.sumOf { 1.0 / max(it.second.toDouble(), 0.0001) } }
            .maxByOrNull { it.value }
            ?.key ?: return null

        val labelName = gestureSamples.firstOrNull { it.labelId == winner }?.labelName ?: return null
        val letter = gestureToLetter(labelName) ?: return null
        val confidence = ((1f - (closestDistance / maxGestureDistance)).coerceIn(0f, 1f) * 100f).toInt()
        return CameraGesturePrediction("sample-$labelName", letter, confidence)
    }

    private fun classifyTrainedMotionPhrase(): CameraGesturePrediction? {
        if (motionPhraseSamples.isEmpty() || liveMotionFrames.size < 8) {
            return null
        }

        val signature = buildMotionSignature(liveMotionFrames.toList()) ?: return null
        if (motionStrength(signature) < 0.14f) {
            return null
        }

        val nearest = motionPhraseSamples
            .map { sample -> sample to euclideanDistance(signature, sample.features) }
            .minByOrNull { it.second } ?: return null

        val liveStrength = motionStrength(signature)
        val trainedStrength = motionStrength(nearest.first.features)
        val strengthRatio =
            if (max(liveStrength, trainedStrength) <= 0.0001f) {
                0f
            } else {
                minOf(liveStrength, trainedStrength) / max(liveStrength, trainedStrength)
            }
        if (strengthRatio < 0.42f) {
            return null
        }

        val maxMotionDistance = motionDistanceLimit(nearest.first.features)
        if (nearest.second > maxMotionDistance) {
            return null
        }

        val confidence = ((1f - (nearest.second / maxMotionDistance)).coerceIn(0f, 1f) * 100f).toInt()
        if (confidence < 46) {
            return null
        }

        return CameraGesturePrediction(
            gesture = "motion-${nearest.first.phrase}",
            letter = "",
            confidencePercent = confidence,
            spokenPhrase = nearest.first.phrase,
        )
    }

    private fun motionDistanceLimit(trainedSignature: FloatArray): Float {
        val strength = motionStrength(trainedSignature)
        return (0.32f + strength * 0.07f).coerceIn(0.38f, 1.20f)
    }

    private fun classifyAlphabetGeometry(features: FloatArray): CameraGesturePrediction? {
        rememberMotion(features)

        val thumbOpen = thumbIsExtended(features)
        val indexOpen = fingerIsExtended(features, 8, 6, 5)
        val middleOpen = fingerIsExtended(features, 12, 10, 9)
        val ringOpen = fingerIsExtended(features, 16, 14, 13)
        val pinkyOpen = fingerIsExtended(features, 20, 18, 17)

        val indexLong = fingerLooksLong(features, 8, 6, 5)
        val middleLong = fingerLooksLong(features, 12, 10, 9)
        val ringLong = fingerLooksLong(features, 16, 14, 13)
        val pinkyLong = fingerLooksLong(features, 20, 18, 17)

        val openCount = listOf(indexOpen, middleOpen, ringOpen, pinkyOpen).count { it }
        val longCount = listOf(indexLong, middleLong, ringLong, pinkyLong).count { it }
        val thumbIndexDistance = landmarkDistance(features, 4, 8)
        val thumbMiddleDistance = landmarkDistance(features, 4, 12)
        val thumbRingDistance = landmarkDistance(features, 4, 16)
        val thumbPinkyDistance = landmarkDistance(features, 4, 20)
        val indexMiddleDistance = landmarkDistance(features, 8, 12)
        val indexHorizontal = fingerIsMostlyHorizontal(features, 8, 5)
        val middleHorizontal = fingerIsMostlyHorizontal(features, 12, 9)
        val indexDown = landmarkY(features, 8) > landmarkY(features, 5) + 0.05f
        val middleDown = landmarkY(features, 12) > landmarkY(features, 9) + 0.05f
        val indexMotion = totalMotion(recentIndexTips)
        val pinkyMotion = totalMotion(recentPinkyTips)
        val thumbTouchesIndex = thumbIndexDistance < 0.12f
        val thumbTouchesMiddle = thumbMiddleDistance < 0.13f
        val thumbTouchesRing = thumbRingDistance < 0.14f
        val thumbTouchesPinky = thumbPinkyDistance < 0.15f
        val indexMiddleClose = indexMiddleDistance < 0.10f
        val indexMiddleVeryClose = indexMiddleDistance < 0.050f
        val indexMiddleWide = indexMiddleDistance > 0.125f
        val roundedCShape =
            thumbOpen &&
                longCount <= 1 &&
                thumbIndexDistance in 0.12f..0.42f &&
                thumbMiddleDistance in 0.12f..0.46f &&
                !thumbTouchesIndex &&
                !thumbTouchesMiddle

        val letter = when {
            thumbTouchesIndex && middleOpen && ringOpen && pinkyOpen -> "F"

            roundedCShape -> "C"

            indexOpen && middleOpen && ringOpen && pinkyOpen -> "B"

            indexOpen && middleOpen && ringOpen && !pinkyOpen -> "W"

            thumbOpen && !indexOpen && !middleOpen && !ringOpen && pinkyOpen -> "Y"

            !thumbOpen && !indexOpen && !middleOpen && !ringOpen && pinkyOpen ->
                if (pinkyMotion > 0.10f) "J" else "I"

            thumbOpen && indexOpen && !middleOpen && !ringOpen && !pinkyOpen -> when {
                indexDown -> "Q"
                indexHorizontal && thumbIndexDistance < 0.28f -> "G"
                else -> "L"
            }

            !thumbOpen && indexOpen && !middleOpen && !ringOpen && !pinkyOpen -> when {
                indexMotion > 0.16f -> "Z"
                fingerIsHooked(features, 8, 7, 6, 5) -> "X"
                else -> "D"
            }

            thumbOpen && indexLong && middleLong && !ringLong && !pinkyLong -> {
                if (indexDown || middleDown) "P" else "K"
            }

            indexOpen && middleOpen && !ringOpen && !pinkyOpen -> when {
                indexMiddleVeryClose -> "R"
                indexHorizontal && middleHorizontal && indexMiddleDistance < 0.13f -> "H"
                indexMiddleClose -> "U"
                indexMiddleWide -> "V"
                else -> "V"
            }

            !indexOpen && !middleOpen && !ringOpen && !pinkyOpen -> classifyClosedOrRoundedLetter(
                features = features,
                thumbOpen = thumbOpen,
                thumbTouchesIndex = thumbTouchesIndex,
                thumbTouchesMiddle = thumbTouchesMiddle,
                thumbTouchesRing = thumbTouchesRing,
                thumbTouchesPinky = thumbTouchesPinky,
                openCount = openCount,
                longCount = longCount,
            )

            longCount == 0 && thumbOpen && thumbIndexDistance in 0.13f..0.36f -> "C"

            else -> null
        } ?: return null

        return CameraGesturePrediction("ASL-$letter", letter, confidenceForLetter(letter))
    }

    private fun classifyClosedOrRoundedLetter(
        features: FloatArray,
        thumbOpen: Boolean,
        thumbTouchesIndex: Boolean,
        thumbTouchesMiddle: Boolean,
        thumbTouchesRing: Boolean,
        thumbTouchesPinky: Boolean,
        openCount: Int,
        longCount: Int,
    ): String? {
        val thumbIndexDistance = landmarkDistance(features, 4, 8)
        val thumbMiddleDistance = landmarkDistance(features, 4, 12)
        val indexTipToMiddleMcp = landmarkDistance(features, 8, 9)
        val middleTipToRingMcp = landmarkDistance(features, 12, 13)
        val ringTipToPinkyMcp = landmarkDistance(features, 16, 17)
        val fingertipsClustered =
            landmarkDistance(features, 8, 12) < 0.12f &&
                landmarkDistance(features, 12, 16) < 0.12f &&
                landmarkDistance(features, 16, 20) < 0.12f

        if (thumbTouchesIndex && thumbTouchesMiddle && fingertipsClustered) {
            return "O"
        }

        if (thumbOpen && thumbIndexDistance in 0.13f..0.38f && thumbMiddleDistance in 0.13f..0.42f) {
            return "C"
        }

        if (thumbTouchesIndex && !thumbTouchesMiddle) {
            return "T"
        }

        if (thumbTouchesIndex && thumbTouchesMiddle && !thumbTouchesRing) {
            return "N"
        }

        if (thumbTouchesIndex && thumbTouchesMiddle && thumbTouchesRing && !thumbTouchesPinky) {
            return "M"
        }

        val fingersFoldedTightly =
            indexTipToMiddleMcp < 0.18f ||
                middleTipToRingMcp < 0.18f ||
                ringTipToPinkyMcp < 0.18f

        if (!thumbOpen && fingersFoldedTightly) {
            return "S"
        }

        if (!thumbOpen && openCount == 0 && longCount == 0) {
            return "E"
        }

        if (thumbOpen || thumbTouchesPinky) {
            return "A"
        }

        return null
    }

    private fun rememberMotion(features: FloatArray) {
        pushMotionPoint(recentIndexTips, LandmarkPoint(landmarkX(features, 8), landmarkY(features, 8)))
        pushMotionPoint(recentPinkyTips, LandmarkPoint(landmarkX(features, 20), landmarkY(features, 20)))
    }

    private fun rememberLiveMotionFrame(features: FloatArray) {
        liveMotionFrames.addLast(features.copyOf())
        while (liveMotionFrames.size > motionFrameWindowSize) {
            liveMotionFrames.removeFirst()
        }
    }

    private fun buildMotionSignature(frames: List<FloatArray>): FloatArray? {
        if (frames.size < 5) {
            return null
        }

        val keypoints = intArrayOf(0, 4, 8, 12, 20)
        val signature = FloatArray(keypoints.size * 3)
        var output = 0
        for (point in keypoints) {
            val startX = frames.take(2).map { landmarkX(it, point) }.average().toFloat()
            val startY = frames.take(2).map { landmarkY(it, point) }.average().toFloat()
            val endX = frames.takeLast(2).map { landmarkX(it, point) }.average().toFloat()
            val endY = frames.takeLast(2).map { landmarkY(it, point) }.average().toFloat()

            var path = 0f
            for (i in 1 until frames.size) {
                val dx = landmarkX(frames[i], point) - landmarkX(frames[i - 1], point)
                val dy = landmarkY(frames[i], point) - landmarkY(frames[i - 1], point)
                path += sqrt(dx * dx + dy * dy)
            }

            signature[output++] = endX - startX
            signature[output++] = endY - startY
            signature[output++] = path
        }

        return if (motionStrength(signature) < 0.08f) null else signature
    }

    private fun motionStrength(signature: FloatArray): Float {
        var total = 0f
        for (i in 2 until signature.size step 3) {
            total += signature[i]
        }
        return total
    }

    private fun pushMotionPoint(points: ArrayDeque<LandmarkPoint>, point: LandmarkPoint) {
        points.addLast(point)
        while (points.size > motionWindowSize) {
            points.removeFirst()
        }
    }

    private fun totalMotion(points: ArrayDeque<LandmarkPoint>): Float {
        if (points.size < 3) {
            return 0f
        }

        var total = 0f
        for (i in 1 until points.size) {
            val previous = points[i - 1]
            val current = points[i]
            val dx = current.x - previous.x
            val dy = current.y - previous.y
            total += sqrt(dx * dx + dy * dy)
        }
        return total
    }

    private fun fingerIsExtended(features: FloatArray, tip: Int, pip: Int, mcp: Int): Boolean {
        val tipToWrist = landmarkDistance(features, tip, 0)
        val pipToWrist = landmarkDistance(features, pip, 0)
        val tipToMcp = landmarkDistance(features, tip, mcp)
        val pipToMcp = landmarkDistance(features, pip, mcp)
        val uprightOrder =
            landmarkY(features, tip) < landmarkY(features, pip) &&
                landmarkY(features, pip) < landmarkY(features, mcp)

        return (tipToWrist > pipToWrist + 0.04f && tipToMcp > pipToMcp * 1.15f) ||
            (uprightOrder && tipToMcp > 0.12f)
    }

    private fun fingerLooksLong(features: FloatArray, tip: Int, pip: Int, mcp: Int): Boolean {
        val tipToMcp = landmarkDistance(features, tip, mcp)
        val pipToMcp = landmarkDistance(features, pip, mcp)
        return tipToMcp > pipToMcp * 1.05f
    }

    private fun thumbIsExtended(features: FloatArray): Boolean {
        val tipToWrist = landmarkDistance(features, 4, 0)
        val ipToWrist = landmarkDistance(features, 3, 0)
        val tipToIndexMcp = landmarkDistance(features, 4, 5)
        val ipToIndexMcp = landmarkDistance(features, 3, 5)
        return tipToWrist > ipToWrist + 0.03f && tipToIndexMcp > ipToIndexMcp + 0.025f
    }

    private fun fingerIsMostlyHorizontal(features: FloatArray, tip: Int, mcp: Int): Boolean {
        val dx = abs(landmarkX(features, tip) - landmarkX(features, mcp))
        val dy = abs(landmarkY(features, tip) - landmarkY(features, mcp))
        return dx > dy * 1.25f
    }

    private fun fingerIsHooked(features: FloatArray, tip: Int, dip: Int, pip: Int, mcp: Int): Boolean {
        val tipToMcp = landmarkDistance(features, tip, mcp)
        val pipToMcp = landmarkDistance(features, pip, mcp)
        val tipToPip = landmarkDistance(features, tip, pip)
        val dipToPip = landmarkDistance(features, dip, pip)
        val tipIsNotFullyFolded = tipToMcp > pipToMcp * 0.75f
        val tipIsCurled = tipToPip < dipToPip * 1.65f
        return tipIsNotFullyFolded && tipIsCurled
    }

    private fun landmarkDistance(features: FloatArray, left: Int, right: Int): Float {
        val dx = landmarkX(features, left) - landmarkX(features, right)
        val dy = landmarkY(features, left) - landmarkY(features, right)
        return sqrt(dx * dx + dy * dy)
    }

    private fun landmarkX(features: FloatArray, index: Int): Float {
        return features[index * 2]
    }

    private fun landmarkY(features: FloatArray, index: Int): Float {
        return features[index * 2 + 1]
    }

    private fun confidenceForLetter(letter: String): Int {
        return when (letter) {
            "A", "B", "C", "D", "F", "I", "L", "O", "V", "W", "Y" -> 82
            "E", "G", "H", "K", "P", "Q", "S", "T", "U", "X" -> 72
            "J", "M", "N", "R", "Z" -> 62
            else -> 60
        }
    }

    private fun euclideanDistance(left: FloatArray, right: FloatArray): Float {
        var total = 0f
        for (i in left.indices) {
            val delta = left[i] - right[i]
            total += delta * delta
        }
        return sqrt(total)
    }

    private fun gestureToLetter(gesture: String): String? {
        return when (gesture) {
            "CLOSED" -> "B"
            "OPEN" -> "B"
            "ZERO" -> "O"
            else -> null
        }
    }

    private fun currentTrainingLetter(): String {
        return activeTrainingLetters.getOrElse(trainingLetterIndex) { activeTrainingLetters.firstOrNull() ?: "A" }
    }

    private fun currentMotionPhrase(): String {
        return motionPhraseLabels.getOrElse(motionPhraseIndex) { motionPhraseLabels.first() }
    }

    private fun hasCompleteUserAlphabet(): Boolean {
        return alphabetLetters.all { userSampleCount(it) >= samplesPerUserLetter }
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

    private fun userSampleCount(letter: String): Int {
        return userSignSamples.count { it.letter == letter }
    }

    private fun motionPhraseSampleCount(phrase: String): Int {
        return motionPhraseSamples.count { it.phrase == phrase }
    }

    private fun firstIncompleteUserLetterIndex(): Int {
        return nextIncompleteUserLetterIndex(0)
    }

    private fun nextIncompleteUserLetterIndex(startIndex: Int): Int {
        val requiredSamples =
            if (activeTrainingKind == CameraTrainingKind.WEAK_LETTERS) {
                minimumWeakLetterSamples
            } else {
                samplesPerUserLetter
            }
        for (index in startIndex until activeTrainingLetters.size) {
            if (userSampleCount(activeTrainingLetters[index]) < requiredSamples) {
                return index
            }
        }
        return -1
    }

    private fun firstIncompleteMotionPhraseIndex(): Int {
        return nextIncompleteMotionPhraseIndex(0)
    }

    private fun nextIncompleteMotionPhraseIndex(startIndex: Int): Int {
        for (index in startIndex until motionPhraseLabels.size) {
            if (motionPhraseSampleCount(motionPhraseLabels[index]) == 0) {
                return index
            }
        }
        return -1
    }

    private fun loadUserSignSamples(): List<UserSignSample> {
        return try {
            openFileInput(userSignsFileName).bufferedReader().useLines { lines ->
                lines.drop(1).mapNotNull { line ->
                    val parts = line.split(",")
                    if (parts.size != 43) {
                        null
                    } else {
                        val letter = parts[0].trim().uppercase(Locale.US)
                        if (letter.length != 1 || letter[0] !in 'A'..'Z') {
                            return@mapNotNull null
                        }

                        val features = FloatArray(42)
                        for (i in 0 until 42) {
                            features[i] = parts[i + 1].toFloatOrNull() ?: return@mapNotNull null
                        }
                        UserSignSample(letter, features)
                    }
                }.toList()
            }
        } catch (_: Exception) {
            emptyList()
        }
    }

    private fun saveUserSignSamples() {
        openFileOutput(userSignsFileName, Context.MODE_PRIVATE).bufferedWriter().use { writer ->
            writer.append("letter")
            for (i in 0 until 42) {
                writer.append(",f").append(i.toString())
            }
            writer.newLine()

            userSignSamples
                .sortedWith(compareBy<UserSignSample> { it.letter }.thenBy { it.features.contentHashCode() })
                .forEach { sample ->
                    writer.append(sample.letter)
                    sample.features.forEach { value ->
                        writer.append(",").append(String.format(Locale.US, "%.8f", value))
                    }
                    writer.newLine()
                }
        }
    }

    private fun loadMotionPhraseSamples(): List<MotionPhraseSample> {
        return try {
            openFileInput(motionPhrasesFileName).bufferedReader().useLines { lines ->
                lines.drop(1).mapNotNull { line ->
                    val parts = line.split(",")
                    if (parts.size != 16) {
                        null
                    } else {
                        val phrase = parts[0].trim().uppercase(Locale.US)
                        if (phrase !in motionPhraseLabels) {
                            return@mapNotNull null
                        }

                        val features = FloatArray(15)
                        for (i in features.indices) {
                            features[i] = parts[i + 1].toFloatOrNull() ?: return@mapNotNull null
                        }
                        MotionPhraseSample(phrase, features)
                    }
                }.toList()
            }
        } catch (_: Exception) {
            emptyList()
        }
    }

    private fun saveMotionPhraseSamples() {
        openFileOutput(motionPhrasesFileName, Context.MODE_PRIVATE).bufferedWriter().use { writer ->
            writer.append("phrase")
            for (i in 0 until 15) {
                writer.append(",f").append(i.toString())
            }
            writer.newLine()

            motionPhraseSamples
                .sortedWith(compareBy<MotionPhraseSample> { it.phrase }.thenBy { it.features.contentHashCode() })
                .forEach { sample ->
                    writer.append(sample.phrase)
                    sample.features.forEach { value ->
                        writer.append(",").append(String.format(Locale.US, "%.8f", value))
                    }
                    writer.newLine()
                }
        }
    }

    private fun loadGestureSamples(): List<GestureSample> {
        return try {
            assets.open("gesture_landmarks.csv").bufferedReader().useLines { lines ->
                lines.drop(1).mapNotNull { line ->
                    val parts = line.split(",")
                    if (parts.size != 44) {
                        null
                    } else {
                        val labelId = parts[0].toIntOrNull() ?: return@mapNotNull null
                        val labelName = parts[1].trim().uppercase(Locale.US)
                        val features = FloatArray(42)
                        for (i in 0 until 42) {
                            features[i] = parts[i + 2].toFloatOrNull() ?: return@mapNotNull null
                        }
                        GestureSample(labelId, labelName, features)
                    }
                }.toList()
            }
        } catch (_: Exception) {
            emptyList()
        }
    }

    private fun cleanLetters(value: String): String {
        return languageModel.cleanLetters(value)
    }

    private fun speak(message: String) {
        val clean = message.trim()
        if (!ttsReady || clean.isBlank() || clean == "...") {
            return
        }

        tts?.speak(clean, TextToSpeech.QUEUE_FLUSH, null, "signasense-camera")
    }

    private fun dp(value: Int): Int {
        return (value * resources.displayMetrics.density).toInt()
    }

    override fun onDestroy() {
        handLandmarker?.close()
        cameraExecutor.shutdown()
        tts?.stop()
        tts?.shutdown()
        super.onDestroy()
    }
}

private data class GestureSample(
    val labelId: Int,
    val labelName: String,
    val features: FloatArray,
)

private data class UserSignSample(
    val letter: String,
    val features: FloatArray,
)

private data class MotionPhraseSample(
    val phrase: String,
    val features: FloatArray,
)

private data class CameraGesturePrediction(
    val gesture: String,
    val letter: String,
    val confidencePercent: Int,
    val spokenPhrase: String? = null,
)

private data class LandmarkPoint(
    val x: Float,
    val y: Float,
)

private enum class CameraSignMode {
    GENERAL,
    USER_DEFINED,
}

private enum class CameraTrainingKind {
    USER_SELECTED,
    WEAK_LETTERS,
}
