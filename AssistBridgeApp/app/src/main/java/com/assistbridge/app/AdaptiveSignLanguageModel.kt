package com.assistbridge.app

import android.content.Context
import java.util.Locale
import kotlin.math.max
import kotlin.math.min

enum class SignDetectionMode {
    AUTO,
    SLOW_LETTERS,
    SENTENCE_ONLY,
}

data class AdaptiveTimingDecision(
    val requiredHoldMs: Long,
    val minimumAppendGapMs: Long,
    val releaseResetMs: Long,
    val sentenceOnly: Boolean,
    val detail: String,
)

class AdaptiveSignLanguageModel(context: Context) {
    private val appContext = context.applicationContext
    private val prefs = appContext.getSharedPreferences("signasense_adaptive_language", Context.MODE_PRIVATE)

    var detectionMode: SignDetectionMode = loadMode()
        private set

    private var learnedHoldMs = prefs.getLong(KEY_HOLD_MS, 620L).coerceIn(360L, 1600L)
    private var learnedGapMs = prefs.getLong(KEY_GAP_MS, 520L).coerceIn(260L, 1500L)
    private var correctionPressure = prefs.getFloat(KEY_CORRECTION_PRESSURE, 0f).coerceIn(0f, 1f)
    private val wordUse = loadCounts(KEY_WORD_COUNTS).toMutableMap()
    private val sentenceUse = loadCounts(KEY_SENTENCE_COUNTS).toMutableMap()

    private val words: List<String> =
        loadAssetLines("sign_language_words.txt")
            .map { cleanLetters(it) }
            .filter { it.isNotBlank() }
            .distinct()
            .sorted()

    private val sentences: List<String> =
        loadAssetLines("sign_language_phrases.txt")
            .map { cleanSentence(it) }
            .filter { it.isNotBlank() }
            .distinct()
            .sorted()

    fun cycleMode(): SignDetectionMode {
        detectionMode =
            when (detectionMode) {
                SignDetectionMode.AUTO -> SignDetectionMode.SLOW_LETTERS
                SignDetectionMode.SLOW_LETTERS -> SignDetectionMode.SENTENCE_ONLY
                SignDetectionMode.SENTENCE_ONLY -> SignDetectionMode.AUTO
            }
        prefs.edit().putString(KEY_MODE, detectionMode.name).apply()
        return detectionMode
    }

    fun modeTitle(): String {
        return when (detectionMode) {
            SignDetectionMode.AUTO -> "Detection Mode: Adaptive"
            SignDetectionMode.SLOW_LETTERS -> "Detection Mode: More Time"
            SignDetectionMode.SENTENCE_ONLY -> "Detection Mode: Sentences Only"
        }
    }

    fun modeDescription(): String {
        return when (detectionMode) {
            SignDetectionMode.AUTO ->
                "Adaptive timing learns from corrections and gives extra hold time when letters are ambiguous."
            SignDetectionMode.SLOW_LETTERS ->
                "More-time mode waits longer before accepting each letter."
            SignDetectionMode.SENTENCE_ONLY ->
                "Sentence-only mode uses detected letters as phrase input and speaks only selected full sentences."
        }
    }

    fun isSentenceOnly(sentenceContext: String = "", currentWord: String = ""): Boolean {
        if (detectionMode == SignDetectionMode.SENTENCE_ONLY) {
            return true
        }
        if (detectionMode != SignDetectionMode.AUTO) {
            return false
        }

        val context = cleanSentence(listOf(sentenceContext, currentWord).joinToString(" "))
        if (context.isBlank()) {
            return false
        }

        val strongSentenceMatches = buildSentenceSuggestions(context, currentWord, limit = 1).isNotEmpty()
        val weakWordMatches = buildWordSuggestions(currentWord, limit = 2).size <= 1
        return context.contains(" ") && strongSentenceMatches && weakWordMatches
    }

    fun timingDecision(
        prefix: String,
        candidateLetter: String,
        confidencePercent: Int,
        nowMs: Long,
        lastAppendAtMs: Long,
        sentenceContext: String,
        sameAsLastAccepted: Boolean,
    ): AdaptiveTimingDecision {
        val cleanPrefix = cleanLetters(prefix)
        val cleanCandidate = cleanLetters(candidateLetter).take(1)
        val sentenceOnly = isSentenceOnly(sentenceContext, cleanPrefix)
        val ambiguity = ambiguityCount(cleanPrefix + cleanCandidate)
        val confidencePenalty =
            when {
                confidencePercent <= 0 -> 220L
                confidencePercent < 55 -> 280L
                confidencePercent < 70 -> 120L
                confidencePercent > 86 -> -90L
                else -> 0L
            }

        var holdMs = learnedHoldMs
        holdMs += min(360L, ambiguity * 45L)
        holdMs += confidencePenalty
        holdMs += (correctionPressure * 280f).toLong()
        if (sameAsLastAccepted) {
            holdMs += 260L
        }
        holdMs =
            when (detectionMode) {
                SignDetectionMode.AUTO -> holdMs
                SignDetectionMode.SLOW_LETTERS -> holdMs + 430L
                SignDetectionMode.SENTENCE_ONLY -> holdMs - 80L
            }
        if (sentenceOnly) {
            holdMs = max(380L, holdMs - 60L)
        }

        val gapMs =
            when (detectionMode) {
                SignDetectionMode.SLOW_LETTERS -> learnedGapMs + 360L
                SignDetectionMode.SENTENCE_ONLY -> max(260L, learnedGapMs - 80L)
                SignDetectionMode.AUTO -> learnedGapMs
            }.coerceIn(260L, 1600L)

        val elapsedSinceAppend = if (lastAppendAtMs <= 0L) Long.MAX_VALUE else nowMs - lastAppendAtMs
        val waitingForGap = elapsedSinceAppend < gapMs
        val detail = buildString {
            append(if (sentenceOnly) "sentence timing" else "letter timing")
            append(", hold ")
            append(holdMs.coerceIn(340L, 1800L))
            append(" ms")
            if (ambiguity >= 4) {
                append(", ambiguous")
            }
            if (waitingForGap) {
                append(", spacing")
            }
        }

        return AdaptiveTimingDecision(
            requiredHoldMs = holdMs.coerceIn(340L, 1800L),
            minimumAppendGapMs = gapMs,
            releaseResetMs = max(260L, (holdMs * 0.65f).toLong()).coerceIn(260L, 1100L),
            sentenceOnly = sentenceOnly,
            detail = detail,
        )
    }

    fun releaseResetMs(): Long {
        return max(280L, (learnedHoldMs * 0.65f).toLong()).coerceIn(280L, 1100L)
    }

    fun recordAcceptedLetter(heldMs: Long, interLetterGapMs: Long) {
        val held = heldMs.coerceIn(340L, 1800L)
        val gap = interLetterGapMs.takeIf { it in 120L..3000L } ?: learnedGapMs
        learnedHoldMs = weightedAverage(learnedHoldMs, held, 0.16f).coerceIn(360L, 1500L)
        learnedGapMs = weightedAverage(learnedGapMs, gap, 0.12f).coerceIn(280L, 1500L)
        correctionPressure = max(0f, correctionPressure - 0.04f)
        saveTiming()
    }

    fun recordCorrection() {
        learnedHoldMs = (learnedHoldMs + 90L).coerceIn(420L, 1600L)
        learnedGapMs = (learnedGapMs + 70L).coerceIn(320L, 1500L)
        correctionPressure = min(1f, correctionPressure + 0.18f)
        saveTiming()
    }

    fun recordWordCommitted(word: String) {
        val clean = cleanLetters(word)
        if (clean.isBlank()) {
            return
        }

        wordUse[clean] = (wordUse[clean] ?: 0) + 1
        saveCounts(KEY_WORD_COUNTS, wordUse)
    }

    fun recordSentenceChosen(sentence: String) {
        val clean = cleanSentence(sentence)
        if (clean.isBlank()) {
            return
        }

        sentenceUse[clean] = (sentenceUse[clean] ?: 0) + 1
        saveCounts(KEY_SENTENCE_COUNTS, sentenceUse)
    }

    fun buildWordSuggestions(prefix: String, limit: Int = 3): List<String> {
        val cleanPrefix = cleanLetters(prefix)
        if (cleanPrefix.isBlank()) {
            return emptyList()
        }

        return words
            .mapNotNull { candidate ->
                scoreWord(cleanPrefix, candidate)?.let { baseScore ->
                    candidate to (baseScore - min(24, (wordUse[candidate] ?: 0) * 3))
                }
            }
            .sortedWith(compareBy<Pair<String, Int>> { it.second }.thenBy { it.first.length }.thenBy { it.first })
            .map { it.first }
            .take(limit)
    }

    fun buildSentenceSuggestions(context: String, currentWord: String, limit: Int = 3): List<String> {
        val cleanContext = cleanSentence(context)
        val cleanCurrentWord = cleanLetters(currentWord)
        if (cleanContext.isBlank() && cleanCurrentWord.isBlank()) {
            return emptyList()
        }

        val contextWords = cleanContext.split(" ").filter { it.isNotBlank() }
        return sentences
            .mapNotNull { sentence ->
                scoreSentence(sentence, cleanContext, contextWords, cleanCurrentWord)?.let { baseScore ->
                    sentence to (baseScore - min(30, (sentenceUse[sentence] ?: 0) * 4))
                }
            }
            .sortedWith(compareBy<Pair<String, Int>> { it.second }.thenBy { it.first.length }.thenBy { it.first })
            .map { it.first }
            .take(limit)
    }

    fun cleanLetters(value: String): String {
        return value.trim().uppercase(Locale.US).filter { it in 'A'..'Z' }
    }

    fun cleanSentence(value: String): String {
        return value.uppercase(Locale.US)
            .replace(Regex("[^A-Z ]"), " ")
            .replace(Regex("\\s+"), " ")
            .trim()
    }

    private fun scoreWord(prefix: String, candidate: String): Int? {
        if (candidate == prefix) {
            return 0
        }
        if (candidate.startsWith(prefix)) {
            return 10 + candidate.length - prefix.length
        }
        if (prefix.length >= 2) {
            val candidateStart = candidate.take(prefix.length.coerceAtMost(candidate.length))
            val distance = editDistance(prefix, candidateStart)
            if (distance <= 1) {
                return 35 + distance * 5 + candidate.length
            }
            if (isSubsequence(prefix, candidate)) {
                return 70 + candidate.length
            }
        }
        return null
    }

    private fun scoreSentence(
        sentence: String,
        context: String,
        contextWords: List<String>,
        currentWord: String,
    ): Int? {
        val compactContext = cleanLetters(context)
        val compactSentence = cleanLetters(sentence)
        val acronym = sentence.split(" ")
            .filter { it.isNotBlank() }
            .joinToString("") { it.first().toString() }

        if (sentence == context) {
            return 0
        }
        if (context.isNotBlank() && sentence.startsWith(context)) {
            return 10 + sentence.length - context.length
        }
        if (compactContext.isNotBlank() && acronym.startsWith(compactContext)) {
            return 24 + sentence.length - min(sentence.length, compactContext.length)
        }
        if (compactContext.length >= 2 && compactSentence.startsWith(compactContext)) {
            return 34 + sentence.length - min(sentence.length, compactContext.length)
        }
        if (currentWord.isNotBlank() && sentence.split(" ").any { it.startsWith(currentWord) }) {
            return 50 + sentence.length
        }
        if (contextWords.isNotEmpty() && contextWords.all { word -> sentence.contains(word) }) {
            return 65 + sentence.length
        }
        if (currentWord.length >= 2 && sentence.split(" ").any { isSubsequence(currentWord, it) }) {
            return 86 + sentence.length
        }
        if (compactContext.length >= 3 && isSubsequence(compactContext, compactSentence)) {
            return 110 + sentence.length
        }
        return null
    }

    private fun ambiguityCount(prefix: String): Long {
        val cleanPrefix = cleanLetters(prefix)
        if (cleanPrefix.isBlank()) {
            return 0L
        }

        return words.asSequence()
            .filter { word -> word.startsWith(cleanPrefix) || (cleanPrefix.length >= 2 && scoreWord(cleanPrefix, word) != null) }
            .take(9)
            .count()
            .toLong()
    }

    private fun loadAssetLines(fileName: String): List<String> {
        return try {
            appContext.assets.open(fileName).bufferedReader().useLines { lines ->
                lines.map { it.trim() }
                    .filter { it.isNotBlank() && !it.startsWith("#") }
                    .toList()
            }
        } catch (_: Exception) {
            emptyList()
        }
    }

    private fun loadMode(): SignDetectionMode {
        val raw = prefs.getString(KEY_MODE, SignDetectionMode.AUTO.name)
        return SignDetectionMode.entries.firstOrNull { it.name == raw } ?: SignDetectionMode.AUTO
    }

    private fun weightedAverage(current: Long, sample: Long, sampleWeight: Float): Long {
        return ((current * (1f - sampleWeight)) + (sample * sampleWeight)).toLong()
    }

    private fun saveTiming() {
        prefs.edit()
            .putLong(KEY_HOLD_MS, learnedHoldMs)
            .putLong(KEY_GAP_MS, learnedGapMs)
            .putFloat(KEY_CORRECTION_PRESSURE, correctionPressure)
            .apply()
    }

    private fun loadCounts(key: String): Map<String, Int> {
        val raw = prefs.getString(key, "").orEmpty()
        if (raw.isBlank()) {
            return emptyMap()
        }

        return raw.lineSequence()
            .mapNotNull { line ->
                val parts = line.split("|", limit = 2)
                if (parts.size != 2) {
                    null
                } else {
                    parts[0] to (parts[1].toIntOrNull() ?: 0)
                }
            }
            .filter { it.first.isNotBlank() && it.second > 0 }
            .toMap()
    }

    private fun saveCounts(key: String, counts: Map<String, Int>) {
        val raw = counts.entries
            .sortedByDescending { it.value }
            .take(80)
            .joinToString("\n") { "${it.key}|${it.value}" }
        prefs.edit().putString(key, raw).apply()
    }

    private fun isSubsequence(needle: String, haystack: String): Boolean {
        var position = 0
        for (char in haystack) {
            if (position < needle.length && needle[position] == char) {
                position++
            }
        }
        return position == needle.length
    }

    private fun editDistance(left: String, right: String): Int {
        val previous = IntArray(right.length + 1) { it }
        val current = IntArray(right.length + 1)

        for (i in 1..left.length) {
            current[0] = i
            for (j in 1..right.length) {
                val cost = if (left[i - 1] == right[j - 1]) 0 else 1
                current[j] = minOf(
                    previous[j] + 1,
                    current[j - 1] + 1,
                    previous[j - 1] + cost,
                )
            }
            for (j in previous.indices) {
                previous[j] = current[j]
            }
        }

        return previous[right.length]
    }

    private companion object {
        const val KEY_MODE = "mode"
        const val KEY_HOLD_MS = "hold_ms"
        const val KEY_GAP_MS = "gap_ms"
        const val KEY_CORRECTION_PRESSURE = "correction_pressure"
        const val KEY_WORD_COUNTS = "word_counts"
        const val KEY_SENTENCE_COUNTS = "sentence_counts"
    }
}
