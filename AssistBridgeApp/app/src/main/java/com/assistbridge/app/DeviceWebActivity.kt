package com.assistbridge.app

import android.annotation.SuppressLint
import android.graphics.Bitmap
import android.os.Bundle
import android.speech.tts.TextToSpeech
import android.view.View
import android.webkit.JavascriptInterface
import android.webkit.WebResourceError
import android.webkit.WebResourceRequest
import android.webkit.WebView
import android.webkit.WebViewClient
import androidx.appcompat.app.AppCompatActivity
import androidx.activity.addCallback
import androidx.core.view.isVisible
import com.assistbridge.app.databinding.ActivityDeviceWebBinding
import java.util.Locale

class DeviceWebActivity : AppCompatActivity(), TextToSpeech.OnInitListener {

    private lateinit var binding: ActivityDeviceWebBinding
    private var webTts: TextToSpeech? = null
    private var webTtsReady = false

    override fun onCreate(savedInstanceState: Bundle?) {
        ThemeSettings.apply(this)
        super.onCreate(savedInstanceState)
        binding = ActivityDeviceWebBinding.inflate(layoutInflater)
        setContentView(binding.root)
        webTts = TextToSpeech(this, this)

        val title = intent.getStringExtra(EXTRA_TITLE).orEmpty().ifBlank { "Device Page" }
        val url = intent.getStringExtra(EXTRA_URL).orEmpty().ifBlank { "http://192.168.4.1" }

        binding.toolbar.title = title
        binding.toolbar.setNavigationOnClickListener { finish() }

        onBackPressedDispatcher.addCallback(this) {
            if (binding.webView.canGoBack()) {
                binding.webView.goBack()
            } else {
                finish()
            }
        }

        configureWebView(url)
    }

    @SuppressLint("SetJavaScriptEnabled")
    private fun configureWebView(url: String) {
        binding.webView.settings.javaScriptEnabled = true
        binding.webView.settings.domStorageEnabled = true
        binding.webView.settings.loadWithOverviewMode = true
        binding.webView.settings.useWideViewPort = true
        binding.webView.addJavascriptInterface(AndroidSpeechBridge(), "SignaSenseAndroid")

        binding.webView.webViewClient =
            object : WebViewClient() {
                override fun onPageStarted(view: WebView?, url: String?, favicon: Bitmap?) {
                    binding.statusText.isVisible = true
                    binding.statusText.text = "Connecting to $url"
                    binding.progressBar.visibility = View.VISIBLE
                }

                override fun onPageFinished(view: WebView?, url: String?) {
                    binding.statusText.text = "Connected to $url"
                    binding.progressBar.visibility = View.GONE
                }

                override fun onReceivedError(
                    view: WebView?,
                    request: WebResourceRequest?,
                    error: WebResourceError?,
                ) {
                    if (request?.isForMainFrame == true) {
                        binding.statusText.text =
                            "Could not open the device page. Join the SmartGlove Wi-Fi network, then reload."
                        binding.progressBar.visibility = View.GONE
                    }
                }
            }

        binding.webView.loadUrl(url)
    }

    override fun onInit(status: Int) {
        webTtsReady = status == TextToSpeech.SUCCESS
        if (!webTtsReady) {
            return
        }

        webTts?.language = Locale.US
        webTts?.setSpeechRate(0.95f)
        webTts?.setPitch(1.0f)
    }

    inner class AndroidSpeechBridge {
        @JavascriptInterface
        fun speak(text: String) {
            val message = text.trim().take(180)
            if (message.isBlank()) {
                return
            }

            runOnUiThread {
                if (webTtsReady) {
                    webTts?.speak(message, TextToSpeech.QUEUE_FLUSH, null, "signasense-device-page")
                }
            }
        }
    }

    override fun onDestroy() {
        binding.webView.removeJavascriptInterface("SignaSenseAndroid")
        webTts?.stop()
        webTts?.shutdown()
        super.onDestroy()
    }

    companion object {
        const val EXTRA_TITLE = "extra_title"
        const val EXTRA_URL = "extra_url"
    }
}
