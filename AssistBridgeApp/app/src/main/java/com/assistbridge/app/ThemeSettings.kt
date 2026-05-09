package com.assistbridge.app

import android.content.Context
import androidx.appcompat.app.AppCompatDelegate

object ThemeSettings {
    private const val preferencesName = "signasense_theme_preferences"
    private const val themeKey = "theme_mode"
    private const val lightValue = "light"
    private const val darkValue = "dark"

    fun apply(context: Context) {
        AppCompatDelegate.setDefaultNightMode(currentMode(context).delegateMode)
    }

    fun setLight(context: Context) {
        setMode(context, AppThemeMode.LIGHT)
    }

    fun setDark(context: Context) {
        setMode(context, AppThemeMode.DARK)
    }

    fun currentMode(context: Context): AppThemeMode {
        val value = context
            .getSharedPreferences(preferencesName, Context.MODE_PRIVATE)
            .getString(themeKey, lightValue)

        return if (value == darkValue) AppThemeMode.DARK else AppThemeMode.LIGHT
    }

    private fun setMode(context: Context, mode: AppThemeMode) {
        context
            .getSharedPreferences(preferencesName, Context.MODE_PRIVATE)
            .edit()
            .putString(themeKey, mode.storageValue)
            .apply()

        AppCompatDelegate.setDefaultNightMode(mode.delegateMode)
    }
}

enum class AppThemeMode(
    val label: String,
    val storageValue: String,
    val delegateMode: Int,
) {
    LIGHT("Light mode", "light", AppCompatDelegate.MODE_NIGHT_NO),
    DARK("Dark mode", "dark", AppCompatDelegate.MODE_NIGHT_YES),
}
