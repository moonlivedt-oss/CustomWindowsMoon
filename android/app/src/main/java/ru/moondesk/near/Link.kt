package ru.moondesk.near

import android.content.Context
import android.content.SharedPreferences
import android.util.Base64
import java.io.ByteArrayOutputStream
import java.io.IOException
import java.io.InputStream
import java.net.URL
import java.security.SecureRandom
import java.security.cert.CertificateException
import java.security.cert.CertificateFactory
import java.security.cert.X509Certificate
import java.util.concurrent.Executors
import javax.net.ssl.HttpsURLConnection
import javax.net.ssl.SSLContext
import javax.net.ssl.SSLSocketFactory
import javax.net.ssl.X509TrustManager

// Настройки приложения (что включено, адрес ПК, закреплённый сертификат, что уже отправлено)
class Prefs(ctx: Context) {
    var gentleTransfer: Boolean
        get() = p.getBoolean("gentleTransfer", true)
        set(v) = p.edit().putBoolean("gentleTransfer", v).apply()
    val p: SharedPreferences = ctx.applicationContext.getSharedPreferences("near", Context.MODE_PRIVATE)
    var base: String?  // адрес ПК с ключом запомненного телефона - под Vault (Android Keystore)
        get() = secret("base")
        set(v) = putSecret("base", v)
    var ca: String?  // корневой сертификат ПК (DER в base64): знакомство один раз, дальше - только он; под Vault
        get() = secret("ca")
        set(v) = putSecret("ca", v)
    var codes: Boolean  // уведомления с кодами подтверждения - на ПК (по умолчанию нет)
        get() = p.getBoolean("codes", false)
        set(v) = p.edit().putBoolean("codes", v).apply()
    var skip: String  // чьи уведомления не отправлять: часть имени пакета или названия, по строке
        get() = p.getString("skip", null) ?: Pure.skipDefault.joinToString("\n")
        set(v) = p.edit().putString("skip", v).apply()

    // Старая запись открытым текстом переписывается зашифрованной при первом чтении
    private fun secret(k: String): String? {
        val raw = p.getString(k, null) ?: return null
        if (Vault.sealed(raw)) return Vault.open(raw)
        putSecret(k, raw)
        return raw
    }

    private fun putSecret(k: String, v: String?) {
        if (v == null) {
            p.edit().remove(k).apply()
            return
        }
        val sealed = try {
            Vault.seal(v)
        } catch (_: Exception) {
            null  // Keystore не работает - не хранить вовсе, чем хранить открыто
        }
        if (sealed == null) p.edit().remove(k).apply() else p.edit().putString(k, sealed).apply()
    }
    var pc: String
        get() = p.getString("pc", "") ?: ""
        set(v) = p.edit().putString("pc", v).apply()
    var wol: String  // последний ответ /pcinfo: MAC и широковещательный адрес, чтобы будить ПК, когда он спит
        get() = p.getString("wol", "") ?: ""
        set(v) = p.edit().putString("wol", v).apply()
    var notes: Boolean
        get() = p.getBoolean("notes", false)
        set(v) = p.edit().putBoolean("notes", v).apply()
    var sms: Boolean
        get() = p.getBoolean("sms", false)
        set(v) = p.edit().putBoolean("sms", v).apply()
    var calls: Boolean
        get() = p.getBoolean("calls", false)
        set(v) = p.edit().putBoolean("calls", v).apply()
    var photos: Boolean
        get() = p.getBoolean("photos", false)
        set(v) = p.edit().putBoolean("photos", v).apply()
    var clip: Boolean
        get() = p.getBoolean("clip", false)
        set(v) = p.edit().putBoolean("clip", v).apply()
    var callState: String?
        get() = p.getString("callState", "IDLE")
        set(v) = p.edit().putString("callState", v).apply()
    // метка этой установки: окно буфера читает буфер телефона только для своих (ClipActivity.own)
    val clipToken: String
        get() = p.getString("clipToken", null) ?: java.util.UUID.randomUUID().toString().also {
            p.edit().putString("clipToken", it).apply()
        }
    var callWho: String
        get() = p.getString("callWho", "") ?: ""
        set(v) = p.edit().putString("callWho", v).apply()
    var callNum: String
        get() = p.getString("callNum", "") ?: ""
        set(v) = p.edit().putString("callNum", v).apply()

    fun lastMedia(kind: String): Long = p.getLong("last_$kind", -1)
    fun setLastMedia(kind: String, id: Long) = p.edit().putLong("last_$kind", id).apply()
}

class Resp(val code: Int, val body: ByteArray) {
    val text: String get() = String(body, Charsets.UTF_8)
}

// Связь с сервером ПК: только https, только свой корневой сертификат ПК (его выдаёт /ca.crt при знакомстве)
object Link {
    private val pool = Executors.newSingleThreadExecutor()  // уведомления, SMS, звонки - по очереди, не в главном

    private class Pin(val ca: X509Certificate?) : X509TrustManager {
        var seen: X509Certificate? = null
        override fun checkClientTrusted(chain: Array<out X509Certificate>?, authType: String?) =
            throw CertificateException("нет")

        override fun checkServerTrusted(chain: Array<out X509Certificate>?, authType: String?) {
            val leaf = chain?.firstOrNull() ?: throw CertificateException("пусто")
            if (ca == null) {  // знакомство: запомним, кто ответил, и сверим с /ca.crt
                seen = leaf
                return
            }
            leaf.checkValidity()
            try {
                leaf.verify(ca.publicKey)
            } catch (e: Exception) {
                throw CertificateException("чужой сертификат: это не тот ПК")
            }
        }

        override fun getAcceptedIssuers(): Array<X509Certificate> = arrayOf()
    }

    private fun factory(pin: Pin): SSLSocketFactory {
        val c = SSLContext.getInstance("TLS")
        c.init(null, arrayOf(pin), SecureRandom())
        return c.socketFactory
    }

    private fun caOf(prefs: Prefs): X509Certificate? = prefs.ca?.let {
        CertificateFactory.getInstance("X.509")
            .generateCertificate(Base64.decode(it, Base64.NO_WRAP).inputStream()) as X509Certificate
    }

    private fun open(url: String, pin: Pin, timeout: Int, connect: Int = 5000): HttpsURLConnection {
        val c = URL(url).openConnection() as HttpsURLConnection
        c.sslSocketFactory = factory(pin)
        c.setHostnameVerifier { _, _ -> true }  // адрес ПК меняется; подлинность - по корневому сертификату
        c.connectTimeout = connect
        c.readTimeout = timeout
        c.useCaches = false
        c.instanceFollowRedirects = false  // ключ телефона и содержимое запросов не уходят на адрес из Location
        return c
    }

    fun readAll(s: InputStream, max: Int = 32 shl 20): ByteArray {
        val out = ByteArrayOutputStream()
        val buf = ByteArray(64 shl 10)
        while (true) {
            val k = s.read(buf)
            if (k < 0) break
            out.write(buf, 0, k)
            if (out.size() > max) throw IOException("слишком много")
        }
        return out.toByteArray()
    }

    // запрос к ПК: path - после ключа («poll?seq=3»); IOException - ПК не на связи
    fun call(
        ctx: Context, method: String, path: String, body: ByteArray? = null,
        type: String = "application/x-www-form-urlencoded; charset=utf-8", timeout: Int = 10000,
    ): Resp {
        val prefs = Prefs(ctx)
        val base = prefs.base ?: throw IOException("ПК не подключён")
        val ca = caOf(prefs) ?: throw IOException("ПК не подключён")
        val c = open(base + path, Pin(ca), timeout)
        try {
            c.requestMethod = method
            if (body != null) {
                c.doOutput = true
                c.setRequestProperty("Content-Type", type)
                c.setFixedLengthStreamingMode(body.size)
                c.outputStream.use { it.write(body) }
            }
            val code = c.responseCode
            val s = if (code < 400) c.inputStream else c.errorStream
            return Resp(code, s?.use { readAll(it) } ?: ByteArray(0))
        } finally {
            c.disconnect()
        }
    }

    // кусок файла потоком (фото): не держим 4 МБ в памяти дважды
    fun put(ctx: Context, path: String, data: ByteArray, len: Int): Resp {
        val prefs = Prefs(ctx)
        val c = open((prefs.base ?: throw IOException("нет ПК")) + path, Pin(caOf(prefs)
            ?: throw IOException("нет ПК")), 60000)
        try {
            c.requestMethod = "PUT"
            c.doOutput = true
            c.setRequestProperty("Content-Type", "application/octet-stream")
            c.setFixedLengthStreamingMode(len)
            c.outputStream.use { it.write(data, 0, len) }
            val code = c.responseCode
            val s = if (code < 400) c.inputStream else c.errorStream
            return Resp(code, s?.use { readAll(it, 1 shl 16) } ?: ByteArray(0))
        } finally {
            c.disconnect()
        }
    }

    // Знакомство по ссылке со страницы, шаг 1: берём /ca.crt и сверяем, что сертификат сервера подписан им. Ничего не
    // сохраняет: пользователь сначала сверяет отпечаток (Pure.fingerprint) с карточкой на ПК. IOException - с текстом
    fun probe(b: Pure.Base): ByteArray {
        val pin = Pin(null)
        val c = open(b.url + "ca.crt", pin, 10000)
        val der = try {
            if (c.responseCode != 200) throw IOException("ПК не узнал ссылку (код ${c.responseCode})")
            c.inputStream.use { readAll(it, 1 shl 16) }
        } finally {
            c.disconnect()
        }
        val ca = CertificateFactory.getInstance("X.509").generateCertificate(der.inputStream()) as X509Certificate
        val leaf = pin.seen ?: throw IOException("ПК не показал сертификат")
        try {
            leaf.verify(ca.publicKey)
        } catch (e: Exception) {
            throw IOException("сертификат сервера не от этого ПК")
        }
        return der
    }

    // Шаг 2, после «Совпадает»: запомнить ПК и спросить его имя и MAC. Вернёт имя ПК
    fun pair(ctx: Context, b: Pure.Base, der: ByteArray): String {
        val prefs = Prefs(ctx)
        prefs.base = b.url
        prefs.ca = Base64.encodeToString(der, Base64.NO_WRAP)
        val info = call(ctx, "GET", "pcinfo")
        if (info.code != 200) throw IOException("RadialMenu на ПК старый: нет /pcinfo")
        savePcInfo(ctx, info.text)
        return prefs.pc
    }

    fun savePcInfo(ctx: Context, json: String) {
        val prefs = Prefs(ctx)
        try {
            val o = org.json.JSONObject(json)
            prefs.pc = o.optString("pc", "")
            if ((o.optJSONArray("wol")?.length() ?: 0) > 0) prefs.wol = json  // ПК на USB-раздаче - не затирать
        } catch (_: Exception) {
        }
    }

    // уведомление, SMS или звонок - на ПК; done - в любом случае (BroadcastReceiver.goAsync)
    fun notice(
        ctx: Context, kind: String, app: String?, title: String?, text: String?, num: String?, id: String?,
        done: (() -> Unit)? = null,
    ) {
        val app0 = ctx.applicationContext
        pool.execute {
            try {
                val body = Pure.form(
                    "kind" to kind, "app" to app, "title" to title, "text" to text, "num" to num, "id" to id,
                ).toByteArray()
                call(app0, "POST", "notice", body, timeout = 6000)
            } catch (_: Exception) {  // ПК спит или не в этой сети - уведомление просто не дойдёт
            } finally {
                done?.invoke()
            }
        }
    }

    fun send(work: () -> Unit) = pool.execute {
        try {
            work()
        } catch (_: Exception) {
        }
    }

    fun connected(ctx: Context): Boolean = Prefs(ctx).base != null && Prefs(ctx).ca != null

    // Адрес телефона в домашней Wi-Fi (IPv4) - от него ищется ПК, если тот сменил адрес
    fun hasLan(ctx: Context): Boolean = myLanIp(ctx) != null

    private fun myLanIp(ctx: Context): String? {
        val cm = ctx.getSystemService(android.net.ConnectivityManager::class.java)
        for (n in cm.allNetworks) {
            val caps = cm.getNetworkCapabilities(n) ?: continue
            if (!caps.hasTransport(android.net.NetworkCapabilities.TRANSPORT_WIFI) &&
                !caps.hasTransport(android.net.NetworkCapabilities.TRANSPORT_ETHERNET)) continue
            for (a in cm.getLinkProperties(n)?.linkAddresses ?: continue) {
                val ip = a.address
                if (ip is java.net.Inet4Address && Pure.lanHost(ip.hostAddress ?: "")) return ip.hostAddress
            }
        }
        return null
    }

    // Роутер выдал ПК другой адрес, и прежний молчит: найти ПК в той же /24 по тому же ключу. Чужое устройство не
    // пройдёт - сертификат должен быть подписан закреплённым корнем ПК, а ключ - подойти к /pcinfo. Нашли - новый адрес
    // запоминается. Сразу https с короткими сроками, 64 адреса одновременно: простая проверка порта врёт, если на
    // телефоне VPN-прокси (ByeByeDPI и подобные принимают любое соединение сами и рвут его только потом).
    fun relocate(ctx: Context, current: () -> Boolean = { true }): Boolean {
        val prefs = Prefs(ctx)
        val originalBase = prefs.base ?: return false
        val originalCa = prefs.ca ?: return false
        val b = Pure.parseLink(originalBase) ?: return false
        val ca = caOf(prefs) ?: return false
        val mine = myLanIp(ctx) ?: return false
        val found = java.util.concurrent.atomic.AtomicReference<Pair<String, String>?>()
        val pool = Executors.newFixedThreadPool(64)
        for (h in Pure.subnetHosts(mine, b.host)) pool.execute {
            if (found.get() != null || !current()) return@execute
            val url = Pure.Base(h, b.port, b.path).url
            try {
                val c = open(url + "pcinfo", Pin(ca), 2500, connect = 1500)
                try {
                    if (c.responseCode == 200) {
                        val info = String(c.inputStream.use { readAll(it, 1 shl 16) }, Charsets.UTF_8)
                        found.compareAndSet(null, url to info)
                    }
                } finally {
                    c.disconnect()
                }
            } catch (_: Exception) {  // не наш сертификат или не тот ключ - мимо
            }
        }
        pool.shutdown()
        try {
            pool.awaitTermination(30, java.util.concurrent.TimeUnit.SECONDS)
        } finally {
            pool.shutdownNow()
        }
        val (url, info) = found.get() ?: return false
        // Пользователь мог за время поиска отключить ПК или познакомиться с другим.
        if (!current() || prefs.base != originalBase || prefs.ca != originalCa) return false
        prefs.base = url
        savePcInfo(ctx, info)
        return true
    }

    // Окно «Экран ПК» (WebView): сертификат сервера, который показал ПК, - подписан ли он корнем этого ПК (тот же
    // закреплённый сертификат, что у всех запросов приложения). Тогда страницу можно открыть без установки корня в
    // Android
    fun trusted(ctx: Context, leaf: X509Certificate?): Boolean {
        val ca = caOf(Prefs(ctx)) ?: return false
        if (leaf == null) return false
        return try {
            leaf.checkValidity()
            leaf.verify(ca.publicKey)
            true
        } catch (_: Exception) {
            false
        }
    }
}
