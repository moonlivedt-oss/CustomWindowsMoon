package ru.moondesk.near

import android.Manifest
import android.annotation.SuppressLint
import android.app.Activity
import android.content.ComponentName
import android.content.Intent
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.os.PowerManager
import android.provider.Settings
import android.text.InputType
import android.util.TypedValue
import android.view.Gravity
import android.widget.Button
import android.widget.EditText
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.Switch
import android.widget.TextView

// Главный экран: подключить ПК по ссылке со страницы, что отправлять на ПК, разбудить ПК.
class MainActivity : Activity() {
    private lateinit var prefs: Prefs
    private lateinit var status: TextView
    private lateinit var link: EditText
    private lateinit var col: LinearLayout
    private val switches = mutableMapOf<String, Switch>()
    private var asking: String? = null  // для какого переключателя спросили разрешения

    private fun dp(v: Int) = TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, v.toFloat(), resources.displayMetrics).toInt()

    private fun text(s: String, size: Float = 15f, top: Int = 0): TextView = TextView(this).apply {
        this.text = s
        textSize = size
        setPadding(0, dp(top), 0, dp(4))
        col.addView(this)
    }

    private fun button(s: String, onClick: () -> Unit): Button = Button(this).apply {
        text = s
        isAllCaps = false
        setOnClickListener { onClick() }
        col.addView(this, LinearLayout.LayoutParams(-1, -2).apply { topMargin = dp(6) })
    }

    @SuppressLint("UseSwitchCompatOrMaterialCode")
    private fun switch(key: String, title: String, hint: String) {
        val s = Switch(this).apply {
            text = title
            textSize = 17f
            setPadding(0, dp(14), 0, dp(2))
            setOnClickListener { toggle(key, isChecked) }
        }
        col.addView(s, LinearLayout.LayoutParams(-1, -2))
        text(hint, 13f).alpha = 0.7f
        switches[key] = s
    }

    override fun onCreate(b: Bundle?) {
        super.onCreate(b)
        prefs = Prefs(this)
        col = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dp(20), dp(24), dp(20), dp(32))
        }
        setContentView(ScrollView(this).apply { addView(col) })
        text("Компьютер рядом", 26f).gravity = Gravity.START
        status = text("", 15f, 4)
        text("Ссылка с ПК", 13f, 18).alpha = 0.7f
        link = EditText(this).apply {
            hint = "https://192.168.0.5:8800/d/..."
            inputType = InputType.TYPE_TEXT_VARIATION_URI
            isSingleLine = true
        }
        col.addView(link, LinearLayout.LayoutParams(-1, -2))
        button("Подключить") { pair(link.text.toString()) }
        text(
            "Ссылку даёт страница «Телефона рядом»: запомните телефон, потом нажмите «Подключить приложение». " +
                "Связь идёт только с вашим ПК в вашей сети, без чужих серверов.", 13f,
        ).alpha = 0.7f

        switch("notes", "Уведомления на ПК", "Карточка справа внизу экрана ПК: от кого и что пришло")
        switch("sms", "SMS на ПК", "Код подтверждения ПК предложит скопировать одной кнопкой")
        switch("calls", "Звонки на ПК", "Кто звонит и пропущенные")
        switch("photos", "Новые фото и видео на ПК", "Снятое камерой - в «Изображения\\Телефон\\дата», когда ПК на связи")
        switch("clip", "Буфер ПК - на телефон", "Скопировали на ПК - можно вставлять на телефоне")

        text("Действия", 13f, 22).alpha = 0.7f
        button("Буфер телефона - на ПК") { startActivity(Intent(this, ClipActivity::class.java)) }
        button("Разбудить ПК") {
            Thread {
                val n = Wol.wake(this)
                ClipActivity.say(
                    applicationContext,
                    if (n > 0) "Сигнал отправлен" else "ПК ещё не сказал свой MAC: подключитесь, пока он включён по проводу",
                )
            }.start()
        }
        button("Не усыплять приложение") { noSleep() }
        button("Отключить ПК") {
            prefs.base = null
            prefs.ca = null
            LinkService.stop(this)
            refresh()
        }
        text(
            "Если Android не даёт включить SMS или уведомления («Ограниченная настройка»): О приложении, меню справа " +
                "вверху, «Разрешить ограниченные настройки». Так Android делает со всеми приложениями не из магазина.",
            13f, 18,
        ).alpha = 0.7f
        button("О приложении") {
            startActivity(Intent(Settings.ACTION_APPLICATION_DETAILS_SETTINGS, Uri.parse("package:$packageName")))
        }

        if (Build.VERSION.SDK_INT >= 33 && !granted(Manifest.permission.POST_NOTIFICATIONS))
            requestPermissions(arrayOf(Manifest.permission.POST_NOTIFICATIONS), 1)
        if (Link.connected(this)) LinkService.start(this)
        handle(intent)
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        handle(intent)
    }

    private fun handle(i: Intent?) {
        val data = i?.data ?: return
        if (data.scheme == "moondesk") {
            link.setText(Pure.parseLink(data.toString())?.url ?: "")
            pair(data.toString())
        }
    }

    override fun onResume() {
        super.onResume()
        refresh()
    }

    private fun listenerOn(): Boolean =
        Settings.Secure.getString(contentResolver, "enabled_notification_listeners")
            ?.contains(ComponentName(this, NoticeService::class.java).flattenToString()) == true

    private fun refresh() {
        val pc = prefs.pc.ifEmpty { "ПК" }
        status.text = when {
            !Link.connected(this) -> "ПК не подключён"
            LinkService.online -> "Подключено: $pc на связи"
            else -> "Подключено: $pc сейчас не на связи (спит или другая сеть)"
        }
        if (Link.connected(this) && link.text.isEmpty()) link.setText(prefs.base)
        switches["notes"]?.isChecked = prefs.notes && listenerOn()
        switches["sms"]?.isChecked = prefs.sms && granted(Manifest.permission.RECEIVE_SMS)
        switches["calls"]?.isChecked = prefs.calls && granted(Manifest.permission.READ_PHONE_STATE)
        switches["photos"]?.isChecked = prefs.photos && mediaGranted()
        switches["clip"]?.isChecked = prefs.clip
    }

    private fun mediaPerms() = if (Build.VERSION.SDK_INT >= 33)
        arrayOf(Manifest.permission.READ_MEDIA_IMAGES, Manifest.permission.READ_MEDIA_VIDEO)
    else arrayOf(Manifest.permission.READ_EXTERNAL_STORAGE)

    private fun mediaGranted() = mediaPerms().all { granted(it) }

    private fun permsFor(key: String): Array<String> = when (key) {
        "sms" -> arrayOf(Manifest.permission.RECEIVE_SMS, Manifest.permission.READ_CONTACTS)
        "calls" -> arrayOf(
            Manifest.permission.READ_PHONE_STATE, Manifest.permission.READ_CALL_LOG, Manifest.permission.READ_CONTACTS,
        )
        "photos" -> mediaPerms()
        else -> arrayOf()
    }

    private fun set(key: String, on: Boolean) {
        when (key) {
            "notes" -> prefs.notes = on
            "sms" -> prefs.sms = on
            "calls" -> prefs.calls = on
            "photos" -> {
                if (on && !prefs.photos) {  // заново включили - только снятое с этой минуты
                    prefs.setLastMedia("img", -1)
                    prefs.setLastMedia("vid", -1)
                }
                prefs.photos = on
            }
            "clip" -> prefs.clip = on
        }
    }

    private fun toggle(key: String, on: Boolean) {
        set(key, on)
        if (on && key == "notes" && !listenerOn()) {
            val i = if (Build.VERSION.SDK_INT >= 30)
                Intent(Settings.ACTION_NOTIFICATION_LISTENER_DETAIL_SETTINGS).putExtra(
                    Settings.EXTRA_NOTIFICATION_LISTENER_COMPONENT_NAME,
                    ComponentName(this, NoticeService::class.java).flattenToString(),
                )
            else Intent(Settings.ACTION_NOTIFICATION_LISTENER_SETTINGS)
            startActivity(i)
            return
        }
        val need = permsFor(key).filter { !granted(it) }
        if (on && need.isNotEmpty()) {
            asking = key
            requestPermissions(need.toTypedArray(), 2)
            return
        }
        if (on && key == "photos") LinkService.start(this)
        refresh()
    }

    override fun onRequestPermissionsResult(code: Int, perms: Array<out String>, results: IntArray) {
        super.onRequestPermissionsResult(code, perms, results)
        val key = asking ?: return refresh()
        asking = null
        if (permsFor(key).firstOrNull()?.let { granted(it) } != true) {
            set(key, false)
            ClipActivity.say(applicationContext, "Без разрешения не выйдет")
        }
        refresh()
    }

    private fun pair(s: String) {
        val b = Pure.parseLink(s)
        if (b == null) {
            status.text = "Это не ссылка запомненного телефона (нужна вида https://адрес:8800/d/ключ/)"
            return
        }
        status.text = "Подключаю..."
        Thread {
            val msg = try {
                val pc = Link.pair(this, b)
                LinkService.start(this)
                "Подключено: " + pc.ifEmpty { "ПК" }
            } catch (e: Exception) {
                "Не подключилось: " + (e.message ?: e.javaClass.simpleName)
            }
            runOnUiThread {
                refresh()
                status.text = msg
                link.setText(prefs.base ?: s)
            }
        }.start()
    }

    @SuppressLint("BatteryLife")
    private fun noSleep() {
        val pm = getSystemService(PowerManager::class.java)
        if (pm.isIgnoringBatteryOptimizations(packageName)) {
            ClipActivity.say(applicationContext, "Уже не усыпляется")
            return
        }
        startActivity(Intent(Settings.ACTION_REQUEST_IGNORE_BATTERY_OPTIMIZATIONS, Uri.parse("package:$packageName")))
    }
}
