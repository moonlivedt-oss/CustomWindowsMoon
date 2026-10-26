package ru.moondesk.near

import android.content.Context
import java.net.DatagramPacket
import java.net.DatagramSocket
import java.net.InetAddress

// Wake-on-LAN: «волшебный пакет» на широковещательный адрес сети ПК (порты 9 и 7). MAC и адрес приложение запомнило,
// пока ПК был на связи (/pcinfo). Разбудит, если в BIOS и у сетевой карты включено пробуждение по сети.
object Wol {
    // вернёт, сколько адаптеров позвали; 0 - нечего (ПК не сказал MAC: только Wi-Fi со случайным MAC или USB)
    fun wake(ctx: Context): Int {
        val json = Prefs(ctx).wol
        if (json.isEmpty()) return 0
        var sent = 0
        try {
            val list = org.json.JSONObject(json).optJSONArray("wol") ?: return 0
            DatagramSocket().use { s ->
                s.broadcast = true
                for (k in 0 until list.length()) {
                    val o = list.getJSONObject(k)
                    val p = Pure.magicPacket(o.optString("mac")) ?: continue
                    for (host in listOf(o.optString("bc"), "255.255.255.255").filter { it.isNotEmpty() }.distinct())
                        for (port in intArrayOf(9, 7))
                            try {
                                s.send(DatagramPacket(p, p.size, InetAddress.getByName(host), port))
                            } catch (_: Exception) {
                            }
                    ++sent
                }
            }
        } catch (_: Exception) {
        }
        return sent
    }
}
