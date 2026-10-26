package ru.moondesk.near

import android.security.keystore.KeyGenParameterSpec
import android.security.keystore.KeyProperties
import android.util.Base64
import java.security.KeyStore
import javax.crypto.Cipher
import javax.crypto.KeyGenerator
import javax.crypto.SecretKey
import javax.crypto.spec.GCMParameterSpec

// Секреты приложения - адрес ПК с ключом и сертификат ПК - в настройках лежат зашифрованными ключом AES из Android
// Keystore: ключ не покидает телефон (и аппаратное хранилище, где оно есть), файл настроек без него не прочитать.
object Vault {
    private const val ALIAS = "near-prefs"
    private const val PREFIX = "k1:"  // зашифровано; без метки - старая запись открытым текстом

    // расшифрованное (запись -> текст): адрес и сертификат читаются на каждый запрос к ПК
    private val cache = java.util.concurrent.ConcurrentHashMap<String, String>()

    private fun key(): SecretKey {
        val ks = KeyStore.getInstance("AndroidKeyStore").apply { load(null) }
        (ks.getKey(ALIAS, null) as? SecretKey)?.let { return it }
        val g = KeyGenerator.getInstance(KeyProperties.KEY_ALGORITHM_AES, "AndroidKeyStore")
        g.init(
            KeyGenParameterSpec.Builder(ALIAS, KeyProperties.PURPOSE_ENCRYPT or KeyProperties.PURPOSE_DECRYPT)
                .setBlockModes(KeyProperties.BLOCK_MODE_GCM)
                .setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_NONE)
                .setKeySize(256)
                .build(),
        )
        return g.generateKey()
    }

    fun sealed(v: String): Boolean = v.startsWith(PREFIX)

    fun seal(plain: String): String {
        val c = Cipher.getInstance("AES/GCM/NoPadding")
        c.init(Cipher.ENCRYPT_MODE, key())
        val out = c.iv + c.doFinal(plain.toByteArray(Charsets.UTF_8))
        return PREFIX + Base64.encodeToString(out, Base64.NO_WRAP)
    }

    // null - не расшифровать (ключ Keystore пропал: сброс телефона, перенос настроек) - ПК придётся подключить заново
    fun open(v: String): String? {
        if (!sealed(v)) return null
        cache[v]?.let { return it }
        return try {
            val raw = Base64.decode(v.substring(PREFIX.length), Base64.NO_WRAP)
            val c = Cipher.getInstance("AES/GCM/NoPadding")
            c.init(Cipher.DECRYPT_MODE, key(), GCMParameterSpec(128, raw, 0, 12))
            String(c.doFinal(raw, 12, raw.size - 12), Charsets.UTF_8).also {
                if (cache.size > 8) cache.clear()
                cache[v] = it
            }
        } catch (_: Exception) {
            null
        }
    }
}
