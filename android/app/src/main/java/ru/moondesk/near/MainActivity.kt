package ru.moondesk.near

import android.Manifest
import android.annotation.SuppressLint
import android.app.Activity
import android.app.AlertDialog
import android.content.ComponentName
import android.content.Intent
import android.content.res.ColorStateList
import android.graphics.Color
import android.graphics.Typeface
import android.graphics.drawable.GradientDrawable
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.os.PowerManager
import android.provider.Settings
import android.text.InputType
import android.util.TypedValue
import android.view.View
import android.widget.Button
import android.widget.EditText
import android.widget.ImageView
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.Switch
import android.widget.TextView

// Главный экран: подключить ПК по ссылке со страницы, что отправлять на ПК, разбудить ПК.
class MainActivity : Activity() {
    private lateinit var prefs: Prefs
    private lateinit var status: TextView
    private lateinit var link: EditText
    private lateinit var waiting: ImageView  // ноутбук и телефон ещё не связаны - пока ПК не подключён
    private lateinit var col: LinearLayout  // куда добавляются строки: корень или текущая карточка
    private val switches = mutableMapOf<String, Switch>()
    private var asking: String? = null  // для какого переключателя спросили разрешения

    // те же цвета, что у страницы «Телефона рядом» и темы «Багровое поле» на ПК
    private object C {
        val BG = Color.parseColor("#0F0D12")
        val CARD = Color.parseColor("#1A1620")
        val CARD2 = Color.parseColor("#241E2B")
        val LINE = Color.parseColor("#2F2838")
        val FG = Color.parseColor("#F4EFF7")
        val MUT = Color.parseColor("#9E94A8")
        val AC = Color.parseColor("#E0414F")
        val AC2 = Color.parseColor("#FF7B5C")
        val OK = Color.parseColor("#3FBF7A")
    }

    private fun dp(v: Int) = TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, v.toFloat(), resources.displayMetrics).toInt()

    private fun round(color: Int, r: Int, stroke: Int = 0) = GradientDrawable().apply {
        setColor(color)
        cornerRadius = dp(r).toFloat()
        if (stroke != 0) setStroke(dp(1), stroke)
    }

    private fun text(s: String, size: Float = 15f, top: Int = 0, color: Int = C.FG): TextView = TextView(this).apply {
        this.text = s
        textSize = size
        setTextColor(color)
        setLineSpacing(0f, 1.15f)
        setPadding(0, dp(top), 0, dp(4))
        col.addView(this)
    }

    private fun hint(s: String, top: Int = 0) = text(s, 13f, top, C.MUT)

    // карточка с подписью сверху (как section на странице); строки дальше идут в неё
    private fun card(title: String, root: LinearLayout) {
        col = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            background = round(C.CARD, 20, C.LINE)
            setPadding(dp(16), dp(14), dp(16), dp(14))
        }
        root.addView(col, LinearLayout.LayoutParams(-1, -2).apply { topMargin = dp(12) })
        text(title.uppercase(), 12f, 0, C.MUT).apply {
            letterSpacing = 0.08f
            typeface = Typeface.DEFAULT_BOLD
        }
    }

    private fun button(s: String, primary: Boolean = false, danger: Boolean = false, onClick: () -> Unit): Button =
        Button(this).apply {
            text = s
            isAllCaps = false
            textSize = 16f
            stateListAnimator = null
            minHeight = dp(52)
            setTextColor(if (danger) C.AC2 else C.FG)
            background = if (primary)
                GradientDrawable(GradientDrawable.Orientation.TL_BR, intArrayOf(C.AC, C.AC2)).apply {
                    cornerRadius = dp(14).toFloat()
                }
            else round(if (danger) C.CARD else C.CARD2, 14, if (danger) C.LINE else 0)
            setOnClickListener { onClick() }
            col.addView(this, LinearLayout.LayoutParams(-1, -2).apply { topMargin = dp(8) })
        }

    @SuppressLint("UseSwitchCompatOrMaterialCode")
    private fun switch(key: String, title: String, hint: String) {
        val s = Switch(this).apply {
            text = title
            textSize = 16f
            setTextColor(C.FG)
            minHeight = dp(48)
            setPadding(0, dp(10), 0, 0)
            val states = arrayOf(intArrayOf(android.R.attr.state_checked), intArrayOf())
            thumbTintList = ColorStateList(states, intArrayOf(C.AC2, C.MUT))
            trackTintList = ColorStateList(states, intArrayOf(C.AC, C.LINE))
            setOnClickListener { toggle(key, isChecked) }
        }
        col.addView(s, LinearLayout.LayoutParams(-1, -2))
        hint(hint)
        switches[key] = s
    }

    override fun onCreate(b: Bundle?) {
        super.onCreate(b)
        prefs = Prefs(this)
        window.statusBarColor = C.BG
        window.navigationBarColor = C.BG
        val root = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dp(16), dp(20), dp(16), dp(32))
        }
        col = root
        setContentView(ScrollView(this).apply {
            setBackgroundColor(C.BG)
            addView(root)
        })
        text("Компьютер рядом", 26f).typeface = Typeface.DEFAULT_BOLD
        status = text("", 14f, 6).apply {
            background = round(C.CARD, 99, C.LINE)
            setPadding(dp(14), dp(8), dp(14), dp(8))
            compoundDrawablePadding = dp(8)
        }
        (status.layoutParams as LinearLayout.LayoutParams).apply {
            width = -2
            topMargin = dp(6)
        }

        card("Компьютер", root)
        waiting = ImageView(this).apply {
            setImageResource(R.drawable.app_empty)
            adjustViewBounds = true
            contentDescription = "ПК ещё не подключён"
        }
        col.addView(waiting, LinearLayout.LayoutParams(-1, -2).apply {
            topMargin = dp(4)
            bottomMargin = dp(8)
        })
        link = EditText(this).apply {
            hint = "https://адрес-ПК:8800/d/..."
            inputType = InputType.TYPE_TEXT_VARIATION_URI
            isSingleLine = true
            setTextColor(C.FG)
            setHintTextColor(C.MUT)
            background = round(Color.parseColor("#120F16"), 14, C.LINE)
            setPadding(dp(14), dp(12), dp(14), dp(12))
        }
        col.addView(link, LinearLayout.LayoutParams(-1, -2).apply { topMargin = dp(6) })
        button("Подключить", primary = true) { pair(link.text.toString()) }
        hint(
            "Ссылку даёт страница «Телефона рядом»: «Запомнить этот телефон», потом «Подключить приложение». " +
                "Связь идёт только с вашим ПК в домашней сети, без чужих серверов.", 8,
        )

        card("Что отправлять на ПК", root)
        switch("notes", "Уведомления", "Карточка справа внизу экрана ПК: от кого и что пришло")
        switch("sms", "SMS", "Код подтверждения ПК предложит скопировать одной кнопкой")
        switch("calls", "Звонки", "Кто звонит и пропущенные")
        switch("photos", "Новые фото и видео", "Снятое камерой - в «Изображения\\Телефон\\дата», когда ПК на связи")
        switch("clip", "Буфер ПК - на телефон", "Скопировали на ПК - можно вставлять на телефоне")

        card("Действия", root)
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

        card("Если что-то не включается", root)
        hint(
            "Android не даёт включить SMS или уведомления («Ограниченная настройка»): О приложении, меню справа " +
                "вверху, «Разрешить ограниченные настройки». Так Android делает со всеми приложениями не из магазина.",
        )
        button("О приложении") {
            startActivity(Intent(Settings.ACTION_APPLICATION_DETAILS_SETTINGS, Uri.parse("package:$packageName")))
        }
        button("Отключить ПК", danger = true) {
            if (!Link.connected(this)) return@button
            AlertDialog.Builder(this)
                .setTitle("Отключить ПК?")
                .setMessage("Приложение забудет адрес и сертификат ПК. Подключить снова можно со страницы «Телефона рядом».")
                .setPositiveButton("Отключить") { _, _ ->
                    prefs.base = null
                    prefs.ca = null
                    LinkService.stop(this)
                    link.setText("")
                    refresh()
                }
                .setNegativeButton("Отмена", null)
                .show()
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
        if (data.scheme != "moondesk") return
        i.data = null  // поворот экрана не должен спросить ещё раз
        val b = Pure.parseLink(data.toString())
        if (b == null) {
            status.text = "Ссылка не из вашей сети - не подключаю"
            return
        }
        link.setText(b.url)
        // ссылку может открыть любой сайт или сообщение: подключаем только после явного «да»
        val now = prefs.base
        val msg = if (now != null && now != b.url)
            "Сейчас подключён другой ПК ($now). Переключиться на ${b.host}? Уведомления, SMS и фото пойдут туда."
        else "Подключить ПК ${b.host}? На него пойдут включённые ниже уведомления, SMS, звонки и фото."
        AlertDialog.Builder(this)
            .setTitle("Подключить компьютер?")
            .setMessage(msg)
            .setPositiveButton("Подключить") { _, _ -> pair(b.url) }
            .setNegativeButton("Отмена", null)
            .show()
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
        waiting.visibility = if (Link.connected(this)) View.GONE else View.VISIBLE
        status.text = when {
            !Link.connected(this) -> "ПК не подключён"
            LinkService.online -> "Подключено: $pc на связи"
            else -> "Подключено: $pc сейчас не на связи (спит или другая сеть)"
        }
        // точка слева: зелёная - на связи, багровая - нет
        val dot = GradientDrawable().apply {
            shape = GradientDrawable.OVAL
            setColor(if (Link.connected(this@MainActivity) && LinkService.online) C.OK else C.AC)
            setSize(dp(8), dp(8))
        }
        status.setCompoundDrawablesRelativeWithIntrinsicBounds(dot, null, null, null)
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
            try {
                val der = Link.probe(b)
                runOnUiThread { confirm(b, der, s) }
            } catch (e: Exception) {
                runOnUiThread { status.text = "Не подключилось: " + (e.message ?: e.javaClass.simpleName) }
            }
        }.start()
    }

    // Защита первого знакомства: тот, кто в одной сети, мог ответить вместо ПК. Отпечаток сертификата должен совпасть
    // с карточкой «Телефон рядом» на экране ПК - только тогда приложение его запомнит
    private fun confirm(b: Pure.Base, der: ByteArray, s: String) {
        AlertDialog.Builder(this)
            .setTitle("Сверьте отпечаток")
            .setMessage(
                "Отпечаток ПК:\n\n" + Pure.fingerprint(der) + "\n\nНа экране компьютера, на карточке «Телефон рядом», " +
                    "должно быть то же самое. Не совпадает - нажмите «Нет»: отвечает не ваш ПК.",
            )
            .setPositiveButton("Совпадает") { _, _ ->
                status.text = "Подключаю..."
                Thread {
                    val msg = try {
                        val pc = Link.pair(this, b, der)
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
            .setNegativeButton("Нет") { _, _ -> status.text = "Не подключено: отпечаток не совпал" }
            .setCancelable(false)
            .show()
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
