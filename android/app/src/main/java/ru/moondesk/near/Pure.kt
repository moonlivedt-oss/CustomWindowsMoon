package ru.moondesk.near

import java.net.URLDecoder
import java.net.URLEncoder

// Чистая логика без Android (под тестами app/src/test): ссылка с ПК, форма, пакет Wake-on-LAN, звонки, фото.
object Pure {
    fun autoTransferAllowed(gentle: Boolean, charging: Boolean, metered: Boolean, pcPause: Boolean): Boolean =
        !gentle || (charging && !metered && !pcPause)

    // Монотонное время (elapsedRealtime), чтобы перевод часов не остановил переподключение.
    class Reconnect {
        private var failures = 0
        private var searchedAt: Long? = null
        // В чужой Wi-Fi (работа, кафе) ПК нет: каждый пустой поиск удваивает паузу до часа, иначе телефон весь день
        // перебирал бы 254 адреса раз в 2 минуты. Новая сеть или связь с ПК - снова 2 минуты.
        private var searchGap = SEARCH_MIN

        fun networkChanged() { failures = 0; searchGap = SEARCH_MIN }
        fun connected() { failures = 0; searchGap = SEARCH_MIN }
        fun failed(): Long {
            val delay = minOf(60000L, 2000L shl minOf(failures, 5))
            failures = minOf(failures + 1, 6)
            return delay
        }

        // Сначала повторить сохранённый адрес: ПК мог только просыпаться. Поиск не чаще раза в 2 минуты,
        // в том числе при серии смен сети; вызов только когда у телефона есть адрес локальной сети.
        fun searchDue(now: Long): Boolean {
            if (failures < 2 || searchedAt?.let { now - it < searchGap } == true) return false
            if (searchedAt != null) searchGap = minOf(searchGap * 2, SEARCH_MAX)
            searchedAt = now
            return true
        }

        companion object {
            const val SEARCH_MIN = 120000L
            const val SEARCH_MAX = 3600000L
        }
    }

    // Адрес запомненного телефона: https://192.168.0.5:8800/d/<ключ>/
    data class Base(val host: String, val port: Int, val path: String) {
        val url: String get() = "https://$host:$port$path"
    }

    // ПК отдаёт ссылку только с IPv4 своей сети: имя хоста или внешний адрес - чужой сервер (ссылка с сайта или
    // из сообщения перепривязала бы приложение и уводила SMS с кодами)
    private val linkRe =
        Regex("^(https?)://([0-9]{1,3}(?:\\.[0-9]{1,3}){3})(?::([0-9]{1,5}))?(/d/[A-Za-z0-9_-]{8,64})/?")

    // 10/8, 172.16/12, 192.168/16, 169.254/16 (раздача без DHCP). Без ведущих нулей: «010.1.2.3» разборщик Android
    // может прочесть восьмерично (8.1.2.3 - чужой сервер), а здесь оно выглядело бы как 10.1.2.3
    fun lanHost(host: String): Boolean {
        val p = host.split('.').map { if (it.length > 1 && it[0] == '0') return false; it.toIntOrNull() ?: return false }
        if (p.size != 4 || p.any { it !in 0..255 }) return false
        return p[0] == 10 || (p[0] == 172 && p[1] in 16..31) || (p[0] == 192 && p[1] == 168) ||
            (p[0] == 169 && p[1] == 254)
    }

    // Ссылка со страницы (или moondesk://pair?u=...). По http сервер ПК слушает 8780-8799, по https - тот же сдвиг
    // от 8800: приложение ходит только по https. null - не та ссылка
    fun parseLink(s: String): Base? {
        var t = s.trim()
        if (t.startsWith("moondesk://")) {
            val q = t.substringAfter("u=", "").substringBefore('&')
            if (q.isEmpty()) return null
            t = URLDecoder.decode(q, "UTF-8")
        }
        val m = linkRe.find(t) ?: return null
        if (!lanHost(m.groupValues[2])) return null
        val https = m.groupValues[1] == "https"
        var port = m.groupValues[3].toIntOrNull() ?: return null
        if (!https) {
            if (port !in 8780..8799) return null
            port += 20
        }
        if (port !in 1..65535) return null
        // адрес - из разобранных чисел, а не строкой из ссылки
        val host = m.groupValues[2].split('.').joinToString(".") { it.toInt().toString() }
        return Base(host, port, m.groupValues[4] + "/")
    }

    // Роутер выдал ПК другой адрес - где его искать: остальные адреса той же /24, что у телефона, кроме самого телефона
    // и прежнего адреса ПК. Только домашние сети (lanHost): в чужой сети приложение никого не перебирает
    fun subnetHosts(mine: String, skip: String): List<String> {
        if (!lanHost(mine)) return emptyList()
        val p = mine.split('.')
        val prefix = "${p[0]}.${p[1]}.${p[2]}."
        return (1..254).map { prefix + it }.filter { it != mine && it != skip }
    }

    // Окно «Экран ПК» показывает страницу ПК и больше ничего: переход внутри - только https на тот же адрес, порт и под
    // тот же ключ (base - адрес из настроек, со слешем в конце). Остальное (ссылка на сайт, другой ключ) - мимо окна
    fun insidePc(base: String, url: String): Boolean {
        val b = try { java.net.URI(base) } catch (_: Exception) { return false }
        val u = try { java.net.URI(url) } catch (_: Exception) { return false }
        val path = u.rawPath ?: return false
        return u.scheme == "https" && b.scheme == "https" && u.host == b.host && u.port == b.port &&
            u.rawUserInfo == null && !path.contains("..") && path.startsWith(b.rawPath ?: return false)
    }

    // Отпечаток корневого сертификата ПК для сверки глазами: первые 12 байт SHA-256 DER - 6 групп по 4 знака hex,
    // как на карточке ПК («BA78 16BF 8F01 CFEA 4141 40DE»)
    fun fingerprint(der: ByteArray): String {
        val h = java.security.MessageDigest.getInstance("SHA-256").digest(der)
        val sb = StringBuilder()
        for (i in 0 until 12) {
            if (i > 0 && i % 2 == 0) sb.append(' ')
            sb.append(String.format("%02X", h[i].toInt() and 0xFF))
        }
        return sb.toString()
    }

    // Уведомления, которые по умолчанию не уходят на ПК: банки, кошельки, приложения кодов и паролей. Часть имени
    // пакета или названия приложения, без регистра; список правится на главном экране
    val skipDefault = listOf(
        "bank", "sber", "tinkoff", "tbank", "vtb", "alfa", "raiffeisen", "gazprombank", "pochtabank", "otpbank",
        "sovcom", "openbank", "psbank", "rosbank", "uralsib", "mtsbank", "rshb", "qiwi", "yoomoney", "paypal",
        "revolut", "wallet", "банк", "authenticator", "authy", "yandex.key", "bitwarden", "keepass", "1password",
        "lastpass", "госуслуги", "gosuslugi", "aegis", "proton.pass", "enpass", "dashlane", "kaspersky", "2fa",
        "otp", "andotp", "freeotp",
    )

    // список из поля на главном экране: строки или через запятую, пустое - прочь
    fun parseList(text: String): List<String> =
        text.split('\n', ',', ';').map { it.trim().lowercase() }.filter { it.isNotEmpty() }.distinct()

    fun skipApp(pkg: String, label: String?, list: List<String>): Boolean {
        val p = pkg.lowercase()
        val l = label?.lowercase() ?: ""
        return list.any { it.isNotEmpty() && (p.contains(it) || l.contains(it)) }
    }

    // Похоже на код подтверждения: слово «код», «code», «OTP», «пароль» и рядом 4-8 цифр (можно «123-456»). Такие
    // уведомления по умолчанию не уходят на ПК - код увидел бы любой, кто смотрит на экран компьютера
    private val codeWord = Regex(
        "(код|code|otp|парол|password|pin|подтвержд|verif|ключ|token|2fa|одноразов|login|вход|sign.?in)",
        RegexOption.IGNORE_CASE,
    )
    private val codeDigits = Regex("(?<![0-9])[0-9]{2,4}(?:[- ]?[0-9]{2,4}){1,2}(?![0-9])")
    // буквы с цифрами вперемешку, 5-10 знаков: «AB12CD», «X7K9-2PQ»
    private val codeMixed = Regex("(?<![A-Za-z0-9])(?=[A-Z0-9-]*[0-9])(?=[A-Z0-9-]*[A-Z])[A-Z0-9][A-Z0-9-]{4,9}(?![A-Za-z0-9])")

    fun hasCode(text: String?): Boolean {
        if (text.isNullOrBlank() || !codeWord.containsMatchIn(text)) return false
        return codeDigits.findAll(text).any { m -> m.value.count { it.isDigit() } in 4..12 } ||
            codeMixed.containsMatchIn(text)
    }

    // адрес ПК для экрана: ключ под точками (его видно бы было в снимках экрана и «Недавних»)
    fun shownLink(url: String?): String {
        if (url == null) return ""
        val i = url.indexOf("/d/")
        return if (i < 0) url else url.substring(0, i + 3) + "••••/"
    }

    // отпечаток для ввода: последнюю группу человек переписывает с карточки ПК, а не жмёт «да» по привычке
    fun fingerprintHint(fp: String): String = fp.dropLast(4) + "????"

    fun fingerprintTail(fp: String, typed: String): Boolean =
        typed.filter { !it.isWhitespace() }.equals(fp.takeLast(4), ignoreCase = true)

    fun form(vararg kv: Pair<String, String?>): String =
        kv.filter { it.second != null }
            .joinToString("&") { URLEncoder.encode(it.first, "UTF-8") + "=" + URLEncoder.encode(it.second, "UTF-8") }

    // «Волшебный пакет»: 6 раз FF и 16 раз MAC; null - не MAC
    fun magicPacket(mac: String): ByteArray? {
        val hex = mac.replace(":", "").replace("-", "")
        if (!Regex("^[0-9A-Fa-f]{12}$").matches(hex)) return null
        val m = ByteArray(6) { hex.substring(it * 2, it * 2 + 2).toInt(16).toByte() }
        val p = ByteArray(102)
        for (i in 0 until 6) p[i] = 0xFF.toByte()
        for (r in 0 until 16) System.arraycopy(m, 0, p, 6 + r * 6, 6)
        return p
    }

    // id загрузки для PUT /up (на ПК - 8-40 знаков [A-Za-z0-9_-]): один и тот же файл - тот же id, докачка с места
    fun uploadId(mediaId: Long, size: Long): String = "photo-" + mediaId.toString(36) + "-" + size.toString(36)

    fun haveOf(json: String): Long = Regex("\"have\":(\\d+)").find(json)?.groupValues?.get(1)?.toLongOrNull() ?: -1

    // новые снимки камеры (и снимки экрана Samsung) - в DCIM; присланное в мессенджерах на ПК не отправляем
    // снимки экрана (у Samsung - DCIM/Screenshots) - нет: на них бывают банки и переписка
    fun cameraPath(relativePath: String?): Boolean = relativePath != null && relativePath.startsWith("DCIM/") &&
        !relativePath.startsWith("DCIM/Screenshots", ignoreCase = true)

    // Звонок: что сказать ПК при смене состояния (IDLE, RINGING, OFFHOOK). call - звонит, end - взяли (карточку
    // убрать), missed - отзвонил без ответа или сбросили
    fun callEvent(prev: String?, now: String): String? = when (now) {
        "RINGING" -> if (prev != "RINGING") "call" else null
        "OFFHOOK" -> if (prev == "RINGING") "end" else null
        "IDLE" -> if (prev == "RINGING") "missed" else null
        else -> null
    }

    // id уведомления для ПК: только [A-Za-z0-9._:|-] (ПК остальное выбросит); повтор того же текста - тот же id
    fun noticeId(pkg: String, id: Int, text: String): String =
        (pkg + ":" + id + ":" + Integer.toHexString(text.hashCode())).filter {
            it.isLetterOrDigit() && it.code < 128 || it in "._:|-"
        }.take(120)
}
