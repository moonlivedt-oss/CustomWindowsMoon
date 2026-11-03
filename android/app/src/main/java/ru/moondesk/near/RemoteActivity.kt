package ru.moondesk.near

import android.annotation.SuppressLint
import android.app.Activity
import android.content.Context
import android.content.Intent
import android.content.pm.ActivityInfo
import android.graphics.Color
import android.net.http.SslError
import android.os.Build
import android.os.Bundle
import android.view.KeyEvent
import android.view.View
import android.view.WindowInsets
import android.view.WindowInsetsController
import android.view.WindowManager
import android.view.inputmethod.InputMethodManager
import android.webkit.JavascriptInterface
import android.webkit.SslErrorHandler
import android.webkit.WebResourceRequest
import android.webkit.WebView
import android.webkit.WebViewClient

// Страница «Телефона рядом» внутри приложения. Зачем, если есть браузер:
// - корень ПК ставить в Android не нужно: сертификат сервера сверяется с тем, что приложение закрепило при знакомстве;
// - «Экран ПК» сразу во весь экран и боком, без панелей Android, экран телефона не гаснет;
// - клавиатура открывается сама, когда ПК ждёт текста (браузер открывает её только по нажатию);
// - кнопки громкости телефона - громкость ПК.
// Окно показывает только страницу своего ПК (Pure.insidePc): ссылки наружу в нём не открываются.
class RemoteActivity : Activity() {
    companion object {
        private const val SCREEN = "screen"

        fun open(ctx: Context, screen: Boolean) {
            ctx.startActivity(Intent(ctx, RemoteActivity::class.java).putExtra(SCREEN, screen))
        }
    }

    private lateinit var web: WebView
    private var screen = false

    @SuppressLint("SetJavaScriptEnabled")
    override fun onCreate(b: Bundle?) {
        super.onCreate(b)
        val base = Prefs(this).base
        if (base == null || !Link.connected(this)) {
            ClipActivity.say(applicationContext, "Сначала подключите ПК")
            finish()
            return
        }
        screen = intent.getBooleanExtra(SCREEN, false)
        if (screen) {
            requestedOrientation = ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE
            window.attributes = window.attributes.apply {
                layoutInDisplayCutoutMode = WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES
            }
        }
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        window.statusBarColor = Color.parseColor("#0F0D12")
        window.navigationBarColor = Color.parseColor("#0F0D12")
        if (Build.VERSION.SDK_INT >= 33) setRecentsScreenshotEnabled(false)  // экран ПК не в снимке «Недавних»

        web = WebView(this).apply {
            setBackgroundColor(if (screen) Color.BLACK else Color.parseColor("#0F0D12"))
            filterTouchesWhenObscured = true
            settings.javaScriptEnabled = true
            settings.domStorageEnabled = true
            settings.allowFileAccess = false
            settings.allowContentAccess = false
            settings.setSupportMultipleWindows(false)
            settings.mediaPlaybackRequiresUserGesture = true
            settings.userAgentString = settings.userAgentString + " MoonNear/1.1"
            addJavascriptInterface(Bridge(), "MoonApp")
            webViewClient = object : WebViewClient() {
                // корень ПК не стоит в Android - WebView спрашивает: пускаем, только если сертификат подписан
                // закреплённым корнем этого ПК и адрес - тот, что в настройках
                override fun onReceivedSslError(view: WebView, handler: SslErrorHandler, error: SslError) {
                    if (Pure.insidePc(base, error.url) && Link.trusted(this@RemoteActivity, error.certificate.x509Certificate))
                        handler.proceed()
                    else handler.cancel()
                }

                override fun shouldOverrideUrlLoading(view: WebView, req: WebResourceRequest): Boolean {
                    if (Pure.insidePc(base, req.url.toString())) return false
                    ClipActivity.say(applicationContext, "Здесь открывается только страница вашего ПК")
                    return true
                }

                override fun onReceivedError(view: WebView, req: WebResourceRequest, err: android.webkit.WebResourceError) {
                    if (req.isForMainFrame) {
                        ClipActivity.say(applicationContext, "ПК не отвечает: он спит или телефон в другой сети")
                        finish()
                    }
                }
            }
        }
        // во весь экран Android не ужимает окно под клавиатуру - отступ снизу на её высоту делаем сами, иначе она
        // закрыла бы низ экрана ПК, где обычно и поле ввода
        val frame = android.widget.FrameLayout(this).apply {
            setBackgroundColor(if (screen) Color.BLACK else Color.parseColor("#0F0D12"))
            addView(web)
            if (screen) setOnApplyWindowInsetsListener { v, ins ->
                val bottom = if (Build.VERSION.SDK_INT >= 30) ins.getInsets(WindowInsets.Type.ime()).bottom
                else @Suppress("DEPRECATION") ins.systemWindowInsetBottom
                v.setPadding(0, 0, 0, bottom)
                ins
            }
        }
        setContentView(frame)
        if (screen) immersive()
        web.loadUrl(base + if (screen) "#screen" else "")
    }

    // во весь экран: без строки состояния и кнопок Android, смахнуть с края - покажутся на время
    private fun immersive() {
        if (Build.VERSION.SDK_INT >= 30) {
            window.setDecorFitsSystemWindows(false)
            window.insetsController?.let {
                it.hide(WindowInsets.Type.systemBars())
                it.systemBarsBehavior = WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
            }
        } else {
            @Suppress("DEPRECATION")
            window.decorView.systemUiVisibility = View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY or
                View.SYSTEM_UI_FLAG_FULLSCREEN or View.SYSTEM_UI_FLAG_HIDE_NAVIGATION or
                View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN or View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
        }
    }

    override fun onWindowFocusChanged(hasFocus: Boolean) {
        super.onWindowFocusChanged(hasFocus)
        if (hasFocus && screen) immersive()
    }

    // кнопки громкости телефона - громкость ПК (как кнопки - и + на «Пульте»)
    override fun dispatchKeyEvent(e: KeyEvent): Boolean {
        val c = when (e.keyCode) {
            KeyEvent.KEYCODE_VOLUME_UP -> "vol+"
            KeyEvent.KEYCODE_VOLUME_DOWN -> "vol-"
            else -> return super.dispatchKeyEvent(e)
        }
        if (e.action == KeyEvent.ACTION_DOWN) web.evaluateJavascript("cmd('$c')", null)
        return true
    }

    override fun onDestroy() {
        if (::web.isInitialized) {
            web.removeJavascriptInterface("MoonApp")
            web.destroy()
        }
        super.onDestroy()
    }

    // То, что страница просит у приложения (window.MoonApp). Ничего опасного: клавиатура и закрыть окно
    private inner class Bridge {
        private val imm get() = getSystemService(InputMethodManager::class.java)

        @JavascriptInterface
        fun showKeyboard() = runOnUiThread {
            web.requestFocus()
            imm.showSoftInput(web, 0)
        }

        @JavascriptInterface
        fun hideKeyboard() = runOnUiThread { imm.hideSoftInputFromWindow(web.windowToken, 0) }

        @JavascriptInterface
        fun close() = runOnUiThread { finish() }
    }
}
