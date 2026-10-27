package ru.moondesk.near

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class PureTest {
    @Test
    fun link() {
        val b = Pure.parseLink("https://192.168.0.5:8801/d/AbCdEf_-12345678/")!!
        assertEquals("https://192.168.0.5:8801/d/AbCdEf_-12345678/", b.url)
        // http-страница - на https того же сдвига
        assertEquals(8802, Pure.parseLink("http://10.0.0.2:8782/d/AbCdEf_-12345678")!!.port)
        val app = Pure.parseLink("moondesk://pair?u=https%3A%2F%2F192.168.1.4%3A8800%2Fd%2Fkkkkkkkkkk%2F")!!
        assertEquals("192.168.1.4", app.host)
        assertEquals("/d/kkkkkkkkkk/", app.path)
        assertNull(Pure.parseLink("http://10.0.0.2:80/d/AbCdEf_-12345678"))  // не порт сервера ПК
        assertNull(Pure.parseLink("https://10.0.0.2:8800/abc/"))              // ключ сессии, а не запомненный
        assertNull(Pure.parseLink("https://10.0.0.2/d/AbCdEf_-12345678"))      // без порта
        assertNull(Pure.parseLink("moondesk://pair"))
        assertNull(Pure.parseLink("javascript:alert(1)"))
        // только адреса домашней сети: чужой сервер не перехватит SMS и уведомления
        assertNull(Pure.parseLink("https://evil.example:8800/d/AbCdEf_-12345678/"))
        assertNull(Pure.parseLink("https://8.8.8.8:8800/d/AbCdEf_-12345678/"))
        assertNull(Pure.parseLink("moondesk://pair?u=https%3A%2F%2F93.184.216.34%3A8800%2Fd%2Fkkkkkkkkkk%2F"))
        assertTrue(Pure.lanHost("172.20.1.1") && Pure.lanHost("169.254.3.4") && Pure.lanHost("10.255.0.1"))
        assertTrue(!Pure.lanHost("172.32.0.1") && !Pure.lanHost("192.169.0.1") && !Pure.lanHost("1.2.3.999"))
    }

    @Test
    fun form() {
        assertEquals("kind=sms&text=%D0%BA%D0%BE%D0%B4+1", Pure.form("kind" to "sms", "app" to null, "text" to "код 1"))
    }

    @Test
    fun magic() {
        val p = Pure.magicPacket("0A:1b:2C:3D:4E:FF")!!
        assertEquals(102, p.size)
        assertArrayEquals(ByteArray(6) { 0xFF.toByte() }, p.copyOfRange(0, 6))
        val mac = byteArrayOf(0x0A, 0x1B, 0x2C, 0x3D, 0x4E, 0xFF.toByte())
        for (r in 0 until 16) assertArrayEquals(mac, p.copyOfRange(6 + r * 6, 12 + r * 6))
        assertTrue(Pure.magicPacket("0A-1B-2C-3D-4E-FF") != null)
        assertNull(Pure.magicPacket("0A:1B:2C"))
        assertNull(Pure.magicPacket("ZZ:1B:2C:3D:4E:FF"))
    }

    @Test
    fun upload() {
        val id = Pure.uploadId(1, 1)
        assertTrue(id.length in 8..40 && id.matches(Regex("[A-Za-z0-9_-]+")))
        assertTrue(Pure.uploadId(Long.MAX_VALUE, Long.MAX_VALUE).length <= 40)
        assertEquals(1234L, Pure.haveOf("{\"have\":1234,\"done\":true}"))
        assertEquals(-1L, Pure.haveOf("нет"))
        assertTrue(Pure.cameraPath("DCIM/Camera/") && !Pure.cameraPath("Pictures/Telegram/") && !Pure.cameraPath(null))
    }

    @Test
    fun calls() {
        assertEquals("call", Pure.callEvent("IDLE", "RINGING"))
        assertNull(Pure.callEvent("RINGING", "RINGING"))  // второе оповещение (с номером)
        assertEquals("end", Pure.callEvent("RINGING", "OFFHOOK"))
        assertNull(Pure.callEvent("OFFHOOK", "IDLE"))  // поговорили
        assertEquals("missed", Pure.callEvent("RINGING", "IDLE"))
        assertNull(Pure.callEvent(null, "OFFHOOK"))  // свой исходящий
    }

    @Test
    fun noticeId() {
        val id = Pure.noticeId("org.telegram.messenger", 42, "привет")
        assertTrue(id.startsWith("org.telegram.messenger:42:") && id.matches(Regex("[A-Za-z0-9._:|-]+")))
        assertEquals(id, Pure.noticeId("org.telegram.messenger", 42, "привет"))
        assertTrue(Pure.noticeId("x".repeat(300), -1, "").length <= 120)
    }

    @Test
    fun noticeFilter() {
        val def = Pure.skipDefault
        assertTrue(Pure.skipApp("ru.sberbankmobile", "СберБанк Онлайн", def))
        assertTrue(Pure.skipApp("com.idamob.tinkoff.android", "Т-Банк", def))
        assertTrue(Pure.skipApp("com.google.android.apps.authenticator2", "Authenticator", def))
        assertTrue(Pure.skipApp("ru.example.app", "Мой Банк", def))  // по названию
        assertTrue(!Pure.skipApp("org.telegram.messenger", "Telegram", def))
        assertTrue(!Pure.skipApp("ru.sberbankmobile", "Сбер", listOf()))  // пользователь очистил список
        assertEquals(listOf("sber", "vk.com", "telegram"), Pure.parseList(" Sber \nvk.com, ,telegram;sber\n"))
        assertTrue(Pure.hasCode("Код для входа: 123456"))
        assertTrue(Pure.hasCode("Your verification code is 482-913"))
        assertTrue(Pure.hasCode("Пароль 4821 никому не говорите"))
        assertTrue(!Pure.hasCode("Купил 2 билета за 1500"))     // нет слова «код»
        assertTrue(!Pure.hasCode("Ваш код: 12"))                // слишком коротко
        assertTrue(!Pure.hasCode("Код заказа 1234567890123"))   // длиннее 8 цифр - номер, не код
        assertTrue(!Pure.hasCode(null))
    }

    @Test
    fun auditFixes() {
        // ведущие нули: «010» Android может прочесть восьмерично - чужой адрес под видом домашнего
        assertNull(Pure.parseLink("https://010.1.2.3:8800/d/AbCdEf_-12345678/"))
        assertNull(Pure.parseLink("https://192.168.01.5:8800/d/AbCdEf_-12345678/"))
        assertEquals("10.0.0.2", Pure.parseLink("https://10.0.0.2:8800/d/AbCdEf_-12345678/")!!.host)
        // ключ на экране - под точками
        val shown = Pure.shownLink("https://192.168.0.5:8800/d/AbCdEf_-12345678/")
        assertTrue(shown.startsWith("https://192.168.0.5:8800/d/") && !shown.contains("AbCdEf"))
        // последние знаки отпечатка вписывают с карточки ПК
        val fp = "BA78 16BF 8F01 CFEA 4141 40DE"
        assertEquals("BA78 16BF 8F01 CFEA 4141 ????", Pure.fingerprintHint(fp))
        assertTrue(Pure.fingerprintTail(fp, " 40de "))
        assertTrue(!Pure.fingerprintTail(fp, "4141") && !Pure.fingerprintTail(fp, ""))
        // снимки экрана - не на ПК
        assertTrue(!Pure.cameraPath("DCIM/Screenshots/") && Pure.cameraPath("DCIM/Camera/"))
        // коды: буквы с цифрами, три группы, новые слова
        assertTrue(Pure.hasCode("Ваш код входа: AB12CD"))
        assertTrue(Pure.hasCode("Login code 12 34 56"))
        assertTrue(Pure.hasCode("Одноразовый ключ 7731"))
        assertTrue(!Pure.hasCode("Встретимся у входа в 18"))
        assertTrue(Pure.skipApp("com.beemdevelopment.aegis", "Aegis", Pure.skipDefault))
    }

    @Test
    fun fingerprint() {
        // тот же отпечаток считает ПК (phone::CaFingerprint): SHA-256("abc") = ba7816bf8f01cfea414140de...
        assertEquals("BA78 16BF 8F01 CFEA 4141 40DE", Pure.fingerprint("abc".toByteArray()))
    }
}
