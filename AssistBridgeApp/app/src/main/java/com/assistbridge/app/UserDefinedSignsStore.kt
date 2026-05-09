package com.assistbridge.app

import android.content.Context
import kotlin.math.abs

data class UserDefinedSignPattern(
    val letter: String,
    val raw: IntArray,
    val bend: IntArray,
    val tolerance: Int,
    val testsPassed: Int,
    val updatedAtMs: Long = System.currentTimeMillis(),
) {
    fun isComplete(): Boolean {
        return letter.length == 1 &&
            raw.size == UserDefinedSignsStore.FINGER_COUNT &&
            bend.size == UserDefinedSignsStore.FINGER_COUNT &&
            testsPassed >= UserDefinedSignsStore.REQUIRED_TEST_PASSES
    }
}

data class UserDefinedSignMatch(
    val pattern: UserDefinedSignPattern,
    val distance: Double,
)

class UserDefinedSignsStore(context: Context) {

    private val preferences = context.applicationContext.getSharedPreferences(PREFERENCES_NAME, Context.MODE_PRIVATE)

    fun loadPatterns(): LinkedHashMap<String, UserDefinedSignPattern> {
        val patterns = linkedMapOf<String, UserDefinedSignPattern>()
        val stored = preferences.getString(PATTERNS_KEY, "").orEmpty()

        stored.lineSequence()
            .map { it.trim() }
            .filter { it.isNotBlank() }
            .forEach { line ->
                val pattern = parsePattern(line) ?: return@forEach
                patterns[pattern.letter] = pattern
            }

        return patterns
    }

    fun savePatterns(patterns: Iterable<UserDefinedSignPattern>) {
        val stored = patterns.joinToString("\n") { pattern ->
            listOf(
                pattern.letter,
                pattern.raw.joinToString(","),
                pattern.bend.joinToString(","),
                pattern.tolerance.toString(),
                pattern.testsPassed.toString(),
                pattern.updatedAtMs.toString(),
            ).joinToString("|")
        }

        preferences.edit()
            .putString(PATTERNS_KEY, stored)
            .commit()
    }

    fun clearPatterns() {
        preferences.edit()
            .putString(PATTERNS_KEY, "")
            .commit()
    }

    fun completePatterns(): List<UserDefinedSignPattern> {
        return loadPatterns().values.filter { it.isComplete() }
    }

    fun bestMatch(raw: IntArray, bend: IntArray, requireComplete: Boolean = true): UserDefinedSignMatch? {
        if (raw.size != FINGER_COUNT || bend.size != FINGER_COUNT) {
            return null
        }

        val candidates = loadPatterns().values
            .filter { pattern ->
                pattern.raw.size == FINGER_COUNT &&
                    pattern.bend.size == FINGER_COUNT &&
                    (!requireComplete || pattern.isComplete())
            }

        return candidates
            .map { pattern -> UserDefinedSignMatch(pattern, patternDistance(raw, bend, pattern)) }
            .filter { match -> match.distance <= match.pattern.tolerance }
            .minByOrNull { it.distance }
    }

    fun patternDistance(raw: IntArray, bend: IntArray, pattern: UserDefinedSignPattern): Double {
        val bendDistance = bendDistance(bend, pattern.bend)
        val rawDistance = rawDistance(raw, pattern.raw)
        val bendHasInformation = bend.any { it > 5 } || pattern.bend.any { it > 5 }
        return if (bendHasInformation) {
            (bendDistance * 0.70) + (rawDistance * 0.30)
        } else {
            rawDistance
        }
    }

    private fun parsePattern(line: String): UserDefinedSignPattern? {
        val parts = line.split('|')
        if (parts.size < 5) {
            return null
        }

        val letter = parts[0].trim().uppercase().take(1)
        val raw = parseIntCsv(parts[1])
        val bend = parseIntCsv(parts[2])
        val tolerance = parts[3].toIntOrNull() ?: 20
        val testsPassed = parts[4].toIntOrNull() ?: 0
        val updatedAtMs = parts.getOrNull(5)?.toLongOrNull() ?: 0L

        if (letter.length != 1 || raw.size != FINGER_COUNT || bend.size != FINGER_COUNT) {
            return null
        }

        return UserDefinedSignPattern(
            letter = letter,
            raw = raw,
            bend = bend,
            tolerance = tolerance,
            testsPassed = testsPassed,
            updatedAtMs = updatedAtMs.takeIf { it > 0L } ?: System.currentTimeMillis(),
        )
    }

    private fun parseIntCsv(value: String): IntArray {
        return value.split(',')
            .mapNotNull { it.trim().toIntOrNull() }
            .toIntArray()
    }

    private fun bendDistance(first: IntArray, second: IntArray): Double {
        return first.indices.sumOf { index -> abs(first[index] - second[index]).toDouble() } / first.size
    }

    private fun rawDistance(first: IntArray, second: IntArray): Double {
        return first.indices.sumOf { index ->
            abs(first[index] - second[index]).toDouble() / RAW_DISTANCE_SCALE
        } / first.size
    }

    companion object {
        const val PREFERENCES_NAME = "signasense_glove_letter_trainer"
        const val PATTERNS_KEY = "patterns_v2"
        const val FINGER_COUNT = 5
        const val REQUIRED_TEST_PASSES = 5
        private const val RAW_DISTANCE_SCALE = 20.0
    }
}
