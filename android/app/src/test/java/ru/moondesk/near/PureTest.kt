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
}
