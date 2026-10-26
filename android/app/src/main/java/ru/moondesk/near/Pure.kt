package ru.moondesk.near

import java.net.URLDecoder
import java.net.URLEncoder

// Чистая логика без Android (под тестами app/src/test): ссылка с ПК, форма, пакет Wake-on-LAN, звонки, фото.
object Pure {

    // Адрес запомненного телефона: https://192.168.0.5:8800/d/<ключ>/
    data class Base(val host: String, val port: Int, val path: String) {
        val url: String get() = "https://$host:$port$path"
    }

    // ПК отдаёт ссылку только с IPv4 своей сети: имя хоста или внешний адрес - чужой сервер (ссылка с сайта или
    // из сообщения перепривязала бы приложение и уводила SMS с кодами)
    private val linkRe =
        Regex("^(https?)://([0-9]{1,3}(?:\\.[0-9]{1,3}){3})(?::([0-9]{1,5}))?(/d/[A-Za-z0-9_-]{8,64})/?")

    // 10/8, 172.16/12, 192.168/16, 169.254/16 (раздача без DHCP)
    fun lanHost(host: String): Boolean {
        val p = host.split('.').map { it.toIntOrNull() ?: return false }
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
        return Base(m.groupValues[2], port, m.groupValues[4] + "/")
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
    fun cameraPath(relativePath: String?): Boolean = relativePath != null && relativePath.startsWith("DCIM/")

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
