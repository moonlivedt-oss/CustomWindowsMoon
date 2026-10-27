package ru.moondesk.near

import android.app.Activity
import android.app.AlertDialog
import android.app.PendingIntent
import android.content.ClipboardManager
import android.content.Context
import android.content.Intent
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.service.quicksettings.Tile
import android.service.quicksettings.TileService
import android.widget.Toast

// Буфер телефона - на ПК. Android 10+ читает буфер только окну на экране: прозрачное окно открывается на миг,
// берёт буфер (текст или картинку) и закрывается. Им же - «Поделиться - На ПК» для текста и ссылок.
// Окно открыто для всех (так его видит «Поделиться»), поэтому: присланный текст уходит на ПК только после «Отправить»,
// а буфер телефона читается, только если окно открыли свои (плитка, уведомление, кнопка) - с меткой этой установки.
class ClipActivity : Activity() {
    private var done = false

    override fun onCreate(b: Bundle?) {
        super.onCreate(b)
        if (intent?.action == Intent.ACTION_SEND) {
            done = true
            val text = intent.getStringExtra(Intent.EXTRA_TEXT)
            if (text.isNullOrEmpty()) return finish()
            // чужое приложение могло прислать текст само, без «Поделиться»: подменить буфер ПК - только с согласия
            AlertDialog.Builder(this)
                .setTitle("Отправить в буфер ПК?")
                .setMessage(if (text.length > 300) text.take(300) + "..." else text)
                .setPositiveButton("Отправить") { _, _ -> sendText(applicationContext, text) }
                .setNegativeButton("Отмена", null)
                .setOnDismissListener { finish() }
                .show()
            return
        }
        if (intent?.getStringExtra(OWN) != Prefs(this).clipToken) {  // не свои - буфер телефона не читаем
            done = true
            finish()
        }
    }

    override fun onWindowFocusChanged(hasFocus: Boolean) {
        super.onWindowFocusChanged(hasFocus)
        if (!hasFocus || done) return
        done = true
        val cm = getSystemService(ClipboardManager::class.java)
        val item = cm.primaryClip?.takeIf { it.itemCount > 0 }?.getItemAt(0)
        val uri = item?.uri
        val app = applicationContext
        // картинка - только из чужого провайдера: file:// и свои файлы приложения на ПК не уходят
        if (uri != null && uri.scheme == "content" && uri.authority?.startsWith(packageName) != true &&
            cm.primaryClipDescription?.hasMimeType("image/*") == true
        ) {
            Thread {  // картинку читаем, пока окно на экране (право на неё - у окна)
                val bytes = try {
                    contentResolver.openInputStream(uri)?.use { Link.readAll(it, 30 shl 20) }
                } catch (_: Exception) {
                    null
                }
                runOnUiThread { finish() }
                if (bytes == null) say(app, "Картинка из буфера не прочиталась")
                else Link.send {
                    val code = try {
                        Link.call(app, "POST", "clipimg", bytes, "application/octet-stream", 30000).code
                    } catch (_: Exception) {
                        0
                    }
                    say(app, if (code == 200) "Картинка - в буфере ПК" else "ПК не принял картинку")
                }
            }.start()
            return
        }
        sendText(app, item?.coerceToText(this)?.toString())
        finish()
    }

    companion object {
        private const val OWN = "ru.moondesk.near.own"
        private val main = Handler(Looper.getMainLooper())

        // окно «буфер телефона - на ПК» для своих: плитка, уведомление, кнопка на главном экране
        fun own(ctx: Context): Intent =
            Intent(ctx, ClipActivity::class.java).putExtra(OWN, Prefs(ctx).clipToken)

        fun say(ctx: Context, text: String) = main.post { Toast.makeText(ctx, text, Toast.LENGTH_SHORT).show() }

        fun sendText(app: Context, text: String?) {
            if (text.isNullOrEmpty()) {
                say(app, "Буфер пуст")
                return
            }
            if (!Link.connected(app)) {
                say(app, "ПК не подключён: откройте «Компьютер рядом»")
                return
            }
            LinkService.lastClip = text  // ПК вернёт его в /poll - назад не ставить
            Link.send {
                val ok = try {
                    Link.call(app, "POST", "clip", text.toByteArray(), "text/plain; charset=utf-8").code == 200
                } catch (_: Exception) {
                    false
                }
                say(app, if (ok) "Буфер - на ПК" else "ПК не на связи")
            }
        }
    }
}

// Плитка в шторке «Буфер на ПК»
class ClipTile : TileService() {
    override fun onStartListening() {
        qsTile?.let {
            it.state = if (Link.connected(this)) Tile.STATE_INACTIVE else Tile.STATE_UNAVAILABLE
            it.updateTile()
        }
    }

    override fun onClick() {
        val i = ClipActivity.own(this).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
        if (Build.VERSION.SDK_INT >= 34)
            startActivityAndCollapse(PendingIntent.getActivity(this, 0, i, PendingIntent.FLAG_IMMUTABLE))
        else
            @Suppress("DEPRECATION") startActivityAndCollapse(i)
    }
}
