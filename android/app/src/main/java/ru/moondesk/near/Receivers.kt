package ru.moondesk.near

import android.Manifest
import android.app.Notification
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.net.Uri
import android.provider.ContactsContract
import android.provider.Telephony
import android.service.notification.NotificationListenerService
import android.service.notification.StatusBarNotification
import android.telephony.TelephonyManager

fun Context.granted(p: String) = checkSelfPermission(p) == PackageManager.PERMISSION_GRANTED

// имя из контактов (если разрешили), иначе пусто - ПК покажет номер
fun contactName(ctx: Context, number: String?): String? {
    if (number.isNullOrBlank() || !ctx.granted(Manifest.permission.READ_CONTACTS)) return null
    return try {
        val uri = Uri.withAppendedPath(ContactsContract.PhoneLookup.CONTENT_FILTER_URI, Uri.encode(number))
        ctx.contentResolver.query(uri, arrayOf(ContactsContract.PhoneLookup.DISPLAY_NAME), null, null, null)
            ?.use { if (it.moveToFirst()) it.getString(0) else null }
    } catch (_: Exception) {
        null
    }
}

// SMS на ПК (код подтверждения ПК вынет сам и предложит скопировать)
class SmsReceiver : BroadcastReceiver() {
    override fun onReceive(c: Context, i: Intent) {
        if (i.action != Telephony.Sms.Intents.SMS_RECEIVED_ACTION || !Prefs(c).sms) return
        val parts = Telephony.Sms.Intents.getMessagesFromIntent(i)
        if (parts.isNullOrEmpty()) return
        val from = parts[0].displayOriginatingAddress ?: ""
        val text = parts.joinToString("") { it.displayMessageBody ?: "" }
        val pending = goAsync()
        Link.notice(
            c, "sms", "SMS", contactName(c, from), text, from, "sms:" + parts[0].timestampMillis,
        ) { pending.finish() }
    }
}

// Входящий звонок: «Звонит» на ПК, взяли - карточка уходит, не взяли - «Пропущенный»
class CallReceiver : BroadcastReceiver() {
    override fun onReceive(c: Context, i: Intent) {
        if (i.action != TelephonyManager.ACTION_PHONE_STATE_CHANGED) return
        val prefs = Prefs(c)
        if (!prefs.calls) return
        val state = when (i.getStringExtra(TelephonyManager.EXTRA_STATE)) {
            TelephonyManager.EXTRA_STATE_RINGING -> "RINGING"
            TelephonyManager.EXTRA_STATE_OFFHOOK -> "OFFHOOK"
            else -> "IDLE"
        }
        @Suppress("DEPRECATION") val num = i.getStringExtra(TelephonyManager.EXTRA_INCOMING_NUMBER)
        // с правом на журнал звонков Android шлёт оповещение дважды: без номера и с номером - ждём второе
        if (state == "RINGING" && num == null && c.granted(Manifest.permission.READ_CALL_LOG)) return
        val ev = Pure.callEvent(prefs.callState, state)
        prefs.callState = state
        if (ev == "call") {
            prefs.callNum = num ?: "скрытый номер"
            prefs.callWho = contactName(c, num) ?: ""
        }
        if (ev == null) return
        val pending = goAsync()
        Link.notice(
            c, ev, "Телефон", prefs.callWho.ifEmpty { null }, null, prefs.callNum, "ring:" + prefs.callNum.hashCode(),
        ) { pending.finish() }
    }
}

// Уведомления телефона на ПК. Свои, постоянные (музыка, навигация), ход загрузок, звонки и SMS (у них свой путь) - нет
class NoticeService : NotificationListenerService() {
    override fun onNotificationPosted(sbn: StatusBarNotification) {
        val prefs = Prefs(this)
        if (!prefs.notes || sbn.packageName == packageName || sbn.isOngoing) return
        val n = sbn.notification
        if (n.flags and Notification.FLAG_GROUP_SUMMARY != 0) return
        if (n.category in setOf(
                Notification.CATEGORY_CALL, Notification.CATEGORY_PROGRESS, Notification.CATEGORY_TRANSPORT,
                Notification.CATEGORY_SERVICE, Notification.CATEGORY_NAVIGATION,
            )
        ) return
        if (prefs.sms && sbn.packageName == Telephony.Sms.getDefaultSmsPackage(this)) return
        val ex = n.extras
        val title = ex.getCharSequence(Notification.EXTRA_TITLE)?.toString()
        val text = (ex.getCharSequence(Notification.EXTRA_BIG_TEXT) ?: ex.getCharSequence(Notification.EXTRA_TEXT))
            ?.toString()
        if (title.isNullOrBlank() && text.isNullOrBlank()) return
        val app = try {
            packageManager.getApplicationLabel(packageManager.getApplicationInfo(sbn.packageName, 0)).toString()
        } catch (_: Exception) {
            sbn.packageName
        }
        Link.notice(this, "note", app, title, text, null, Pure.noticeId(sbn.packageName, sbn.id, "$title|$text"))
    }
}

// включение телефона и обновление приложения - связь с ПК снова в фоне
class BootReceiver : BroadcastReceiver() {
    override fun onReceive(c: Context, i: Intent) {
        if (Link.connected(c)) LinkService.start(c)
    }
}

// «Разбудить ПК» из уведомления
class WakeReceiver : BroadcastReceiver() {
    override fun onReceive(c: Context, i: Intent) {
        val pending = goAsync()
        Thread {
            Wol.wake(c)
            pending.finish()
        }.start()
    }
}
