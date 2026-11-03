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
import android.os.Handler
import android.os.Looper
import android.os.PowerManager
import android.os.SystemClock
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
    private lateinit var pcName: TextView
    private lateinit var acts: LinearLayout       // «Экран ПК» и «Пульт» - когда ПК на связи
    private lateinit var wakeBox: LinearLayout    // «Разбудить ПК» - когда подключён, но не отвечает
    private lateinit var changeBtn: LinearLayout  // «Подключить другой ПК» - открывает поле ссылки
    private lateinit var connectBox: LinearLayout // поле ссылки и «Подключить»
    private var connectOpen = false
    private var msg = ""
    private var msgUntil = 0L

    private fun say(s: String) {
        msg = s
        msgUntil = SystemClock.elapsedRealtime() + 10000
        status.text = s
    }
    private val ui = Handler(Looper.getMainLooper())
    private val tick = object : Runnable {  // ПК уснул или проснулся - карточка меняется сама, пока приложение открыто
        override fun run() {
            refresh()
            ui.postDelayed(this, 3000)
        }
    }

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
    private fun card(title: String?, root: LinearLayout) {
        col = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            background = round(C.CARD, 20, C.LINE)
            setPadding(dp(16), dp(14), dp(16), dp(14))
        }
        root.addView(col, LinearLayout.LayoutParams(-1, -2).apply { topMargin = dp(12) })
        if (title == null) return
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

    // кусок карточки, который можно прятать целиком; строки внутри block идут в него
    private fun group(parent: LinearLayout, block: () -> Unit): LinearLayout {
        val g = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL }
        parent.addView(g, LinearLayout.LayoutParams(-1, -2))
        col = g
        block()
        col = parent
        return g
    }

    // кнопки в ряд, поровну
    private fun row(block: () -> Unit) {
        val parent = col
        val r = LinearLayout(this).apply { orientation = LinearLayout.HORIZONTAL }
        parent.addView(r, LinearLayout.LayoutParams(-1, -2))
        col = r
        block()
        col = parent
        for (i in 0 until r.childCount)
            r.getChildAt(i).layoutParams = LinearLayout.LayoutParams(0, -2, 1f).apply {
                topMargin = dp(8)
                if (i > 0) marginStart = dp(8)
            }
    }

    // значок слева от подписи кнопки (картинки из drawable-nodpi - приводим к 26 dp)
    private fun icon(b: Button, res: Int) {
        val d = getDrawable(res) ?: return
        d.setBounds(0, 0, dp(26), dp(26))
        b.setCompoundDrawablesRelative(d, null, null, null)
        b.compoundDrawablePadding = dp(8)
        b.setPadding(dp(14), 0, dp(14), 0)
    }

    private fun wake() {
        Thread {
            val n = Wol.wake(this)
            ClipActivity.say(
                applicationContext,
                if (n > 0) "Сигнал отправлен" else "ПК ещё не сказал свой MAC: подключитесь, пока он включён по проводу",
            )
        }.start()
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
            // фокус при открытии - на самой странице: иначе его брало поле списка внизу, и экран открывался прокрученным
            isFocusableInTouchMode = true
            descendantFocusability = LinearLayout.FOCUS_BEFORE_DESCENDANTS
        }
        col = root
        // ключ к ПК не попадает в снимок «Недавних»; касания сквозь чужое окно поверх (подсунуть «Подключить») - мимо
        if (Build.VERSION.SDK_INT >= 33) setRecentsScreenshotEnabled(false)
        setContentView(ScrollView(this).apply {
            filterTouchesWhenObscured = true
            setBackgroundColor(C.BG)
            addView(root)
        })
        root.requestFocus()
        // Шапка - одна строка: главное на экране - сам ПК, а не название приложения
        text("Компьютер рядом", 15f, 0, C.MUT).typeface = Typeface.DEFAULT_BOLD

        // Сначала - сам ПК: имя, строкой ниже - на связи ли, дальше главное, что с ним можно сделать.
        // Поле ссылки - только когда оно нужно
        card(null, root)
        pcName = text("", 24f).apply { typeface = Typeface.DEFAULT_BOLD }
        status = text("", 14f, 0, C.MUT).apply { compoundDrawablePadding = dp(8) }
        // картинка состояния - полосой под именем: на связи, не отвечает, не подключён. Невысокая, чтобы кнопки
        // оставались на первом экране
        waiting = ImageView(this).apply {
            setImageResource(R.drawable.app_empty)
            scaleType = ImageView.ScaleType.CENTER_CROP
            background = round(C.CARD2, 16)
            clipToOutline = true
            importantForAccessibility = View.IMPORTANT_FOR_ACCESSIBILITY_NO
        }
        col.addView(waiting, LinearLayout.LayoutParams(-1, dp(150)).apply {
            topMargin = dp(8)
            bottomMargin = dp(4)
        })
        val hero = col
        acts = group(hero) {
            row {
                icon(button("Экран ПК", primary = true) { RemoteActivity.open(this, screen = true) }, R.drawable.ic_screen)
                icon(button("Пульт") { RemoteActivity.open(this, screen = false) }, R.drawable.ic_remote)
            }
            hint("Экран ПК - во весь экран и боком: касание - щелчок, клавиатура открывается сама, кнопки громкости " +
                "телефона - громкость ПК. Пульт - вся страница «Телефона рядом» без установки сертификата.", 4)
        }
        wakeBox = group(hero) {
            button("Разбудить ПК", primary = true) { wake() }
            hint("Спит, телефон в другой сети или на ПК домашняя Wi-Fi помечена «общественной» (там будет карточка).", 4)
        }
        changeBtn = group(hero) {
            button("Подключить другой ПК") {
                connectOpen = true
                refresh()
            }
        }
        connectBox = group(hero) {}
        col = connectBox
        link = EditText(this).apply {
            importantForAutofill = View.IMPORTANT_FOR_AUTOFILL_NO_EXCLUDE_DESCENDANTS
            hint = "https://адрес-ПК:8800/d/..."
            inputType = InputType.TYPE_TEXT_VARIATION_URI
            isSingleLine = true
            setTextColor(C.FG)
            setHintTextColor(C.MUT)
            background = round(Color.parseColor("#120F16"), 14, C.LINE)
            setPadding(dp(14), dp(12), dp(14), dp(12))
        }
        col.addView(link, LinearLayout.LayoutParams(-1, -2).apply { topMargin = dp(6) })
        button("Подключить", primary = true) {
            val t = link.text.toString()
            pair(if (prefs.base != null && t == Pure.shownLink(prefs.base)) prefs.base!! else t)
        }
        hint(
            "Ссылку даёт страница «Телефона рядом»: «Запомнить этот телефон», потом «Подключить приложение». " +
                "Связь идёт только с вашим ПК в домашней сети, без чужих серверов.", 8,
        )

        card("Что отправлять на ПК", root)
        switch("notes", "Уведомления", "Карточка справа внизу экрана ПК: от кого и что пришло")
        switch(
            "codes", "Коды подтверждения",
            "SMS и уведомления с кодами и паролями. По умолчанию не отправляются: код увидит любой, кто смотрит на экран ПК",
        )
        hint("Не отправлять уведомления этих приложений (часть имени пакета или названия, по строке):", 8)
        val skip = EditText(this).apply {
            setText(prefs.skip)
            inputType = InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_FLAG_MULTI_LINE
            minLines = 3
            maxLines = 8
            textSize = 14f
            setTextColor(C.FG)
            background = round(Color.parseColor("#120F16"), 14, C.LINE)
            setPadding(dp(14), dp(10), dp(14), dp(10))
        }
        col.addView(skip, LinearLayout.LayoutParams(-1, -2).apply { topMargin = dp(4) })
        button("Сохранить список") {
            prefs.skip = Pure.parseList(skip.text.toString()).joinToString("\n")
            skip.setText(prefs.skip)
            ClipActivity.say(applicationContext, "Список сохранён")
        }
        button("Вернуть список по умолчанию") {
            prefs.skip = Pure.skipDefault.joinToString("\n")
            skip.setText(prefs.skip)
        }
        hint("Банки, кошельки, приложения кодов и паролей уже в списке.")
        switch("sms", "SMS", "Кроме банков из списка ниже и кодов (если «Коды подтверждения» выключены)")
        switch("calls", "Звонки", "Кто звонит и пропущенные")
        switch("photos", "Новые фото и видео", "Снятое камерой - в «Изображения\\Телефон\\дата», когда ПК на связи")
        switch("gentleTransfer", "Бережная отправка фото", "Автоматически — на зарядке, без лимитной сети, когда ПК готов. После паузы продолжим с того же места")
        switch("clip", "Буфер ПК - на телефон", "Скопировали на ПК - можно вставлять на телефоне")

        card("Действия", root)
        button("Буфер телефона - на ПК") { startActivity(ClipActivity.own(this)) }
        button("Разбудить ПК") { wake() }
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
            say("Ссылка не из вашей сети - не подключаю")
            return
        }
        link.setText(Pure.shownLink(b.url))
        // ссылку может открыть любой сайт или сообщение: подключаем только после явного «да»
        val now = prefs.base
        val msg = if (now != null && now != b.url)
            "Сейчас подключён другой ПК (${Pure.shownLink(now)}). Переключиться на ${b.host}? Отправка уведомлений, SMS, " +
                "звонков и фото выключится - включите заново, если это ваш ПК."
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
        ui.post(tick)
    }

    override fun onPause() {
        ui.removeCallbacks(tick)
        super.onPause()
    }

    private fun listenerOn(): Boolean =
        Settings.Secure.getString(contentResolver, "enabled_notification_listeners")
            ?.contains(ComponentName(this, NoticeService::class.java).flattenToString()) == true

    private fun refresh() {
        val pc = prefs.pc.ifEmpty { "ПК" }
        val connected = Link.connected(this)
        fun show(v: View, on: Boolean) {
            v.visibility = if (on) View.VISIBLE else View.GONE
        }
        pcName.text = if (connected) pc else "Подключите свой ПК"
        show(acts, connected && LinkService.online)
        show(wakeBox, connected && !LinkService.online)
        show(connectBox, !connected || connectOpen)
        show(changeBtn, connected && !connectOpen)
        val pic = if (!connected) R.drawable.app_empty else if (LinkService.online) R.drawable.app_online else R.drawable.app_offline
        if (waiting.tag != pic) {  // раз в 3 с - только если поменялось, без лишней перерисовки
            waiting.tag = pic
            waiting.setImageResource(pic)
        }
        // сообщение о подключении («Подключаю...», ошибка) держится 10 с, потом - снова короткое состояние
        status.text = when {
            SystemClock.elapsedRealtime() < msgUntil -> msg
            !connected -> "не подключён"
            LinkService.online -> "на связи"
            else -> "не отвечает"
        }
        // точка слева: зелёная - на связи, багровая - нет
        val dot = GradientDrawable().apply {
            shape = GradientDrawable.OVAL
            setColor(if (Link.connected(this@MainActivity) && LinkService.online) C.OK else C.AC)
            setSize(dp(8), dp(8))
        }
        status.setCompoundDrawablesRelativeWithIntrinsicBounds(dot, null, null, null)
        // адрес мог смениться сам (Link.relocate) - поле показывает нынешний, пока его не правят руками
        val shown = Pure.shownLink(prefs.base)
        if (connected && !connectOpen && link.text.toString() != shown) link.setText(shown)
        switches["notes"]?.isChecked = prefs.notes && listenerOn()
        switches["codes"]?.isChecked = prefs.codes
        switches["sms"]?.isChecked = prefs.sms && granted(Manifest.permission.RECEIVE_SMS)
        switches["calls"]?.isChecked = prefs.calls && granted(Manifest.permission.READ_PHONE_STATE)
        switches["photos"]?.isChecked = prefs.photos && mediaGranted()
        switches["gentleTransfer"]?.isChecked = prefs.gentleTransfer
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
            "codes" -> prefs.codes = on
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
            "gentleTransfer" -> prefs.gentleTransfer = on
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
        if ((on && key == "photos") || key == "gentleTransfer") LinkService.start(this)
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
            say("Это не ссылка запомненного телефона (нужна вида https://адрес:8800/d/ключ/)")
            return
        }
        say("Подключаю...")
        Thread {
            try {
                val der = Link.probe(b)
                runOnUiThread { confirm(b, der, s) }
            } catch (e: Exception) {
                runOnUiThread { say("Не подключилось: " + (e.message ?: e.javaClass.simpleName)) }
            }
        }.start()
    }

    // Защита первого знакомства: тот, кто в одной сети, мог ответить вместо ПК. Отпечаток сертификата должен совпасть
    // с карточкой «Телефон рядом» на экране ПК - только тогда приложение его запомнит
    private fun confirm(b: Pure.Base, der: ByteArray, s: String) {
        val fp = Pure.fingerprint(der)
        val tail = EditText(this).apply {
            hint = "4 знака"
            isSingleLine = true
            importantForAutofill = View.IMPORTANT_FOR_AUTOFILL_NO
            inputType = InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_FLAG_CAP_CHARACTERS
        }
        AlertDialog.Builder(this)
            .setTitle("Сверьте отпечаток")
            .setMessage(
                "Отпечаток ПК:\n\n" + Pure.fingerprintHint(fp) + "\n\nНа экране компьютера, на карточке «Телефон рядом», " +
                    "начало должно быть тем же. Впишите последние 4 знака оттуда. Начало не совпадает - «Нет»: " +
                    "отвечает не ваш ПК.",
            )
            .setView(tail)
            .setPositiveButton("Подключить") { _, _ ->
                if (!Pure.fingerprintTail(fp, tail.text.toString())) {
                    say("Не подключено: последние знаки отпечатка не совпали с карточкой ПК")
                    return@setPositiveButton
                }
                say("Подключаю...")
                val switched = prefs.base != null && prefs.base != b.url
                Thread {
                    val msg = try {
                        val pc = Link.pair(this, b, der)
                        if (switched) {  // другой ПК - что уходило старому, само новому не пойдёт
                            runOnUiThread { for (k in listOf("notes", "codes", "sms", "calls", "photos", "clip")) set(k, false) }
                        }
                        LinkService.start(this)
                        connectOpen = false
                        "Подключено: " + pc.ifEmpty { "ПК" }
                    } catch (e: Exception) {
                        "Не подключилось: " + (e.message ?: e.javaClass.simpleName)
                    }
                    runOnUiThread {
                        refresh()
                        say(msg)
                        link.setText(Pure.shownLink(prefs.base ?: s))
                    }
                }.start()
            }
            .setNegativeButton("Нет") { _, _ -> say("Не подключено: отпечаток не совпал") }
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
