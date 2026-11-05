package ru.moondesk.near

import android.Manifest
import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.ClipData
import android.content.ClipboardManager
import android.content.ContentUris
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.content.BroadcastReceiver
import android.content.pm.ServiceInfo
import android.database.ContentObserver
import android.net.ConnectivityManager
import android.net.Network
import android.net.LinkProperties
import android.net.NetworkCapabilities
import android.net.NetworkRequest
import android.net.Uri
import android.os.Build
import android.os.Handler
import android.os.IBinder
import android.os.Looper
import android.os.SystemClock
import android.os.BatteryManager
import android.provider.MediaStore
import java.io.IOException
import java.net.URLEncoder

// Связь с ПК в фоне (служба с уведомлением - иначе Android её усыпит): буфер ПК - в буфер телефона (долгий опрос
// /poll, как у страницы), новые фото и видео камеры - в «Изображения\Телефон\дата» (PUT /up кусками с докачкой),
// MAC для Wake-on-LAN - пока ПК на связи.
class LinkService : Service() {
    companion object {
        private const val CHANNEL = "link"

        fun start(ctx: Context) {
            ctx.startForegroundService(Intent(ctx, LinkService::class.java))
        }

        fun stop(ctx: Context) {
            ctx.stopService(Intent(ctx, LinkService::class.java))
        }

        @Volatile
        var online = false

        @Volatile
        var lastClip: String? = null  // что уже в обоих буферах - не гонять туда-обратно
    }

    @Volatile
    private var running = false
    private val wake = Object()
    private var networkEpoch = 0L  // под wake: событие между запросом и wait не должно потеряться
    private val main = Handler(Looper.getMainLooper())
    private var photoThread: Thread? = null
    private val photoKick = Object()

    @Volatile
    private var photoWanted = true

    private val mediaObserver = object : ContentObserver(main) {
        override fun onChange(selfChange: Boolean) = kickPhotos()
    }
    private val localLinks = mutableMapOf<Network, String>()
    @Volatile private var pcBulkPause = true
    private val powerEvents = object : BroadcastReceiver() {
        override fun onReceive(context: Context?, intent: Intent?) = kickPhotos()
    }
    private val net = object : ConnectivityManager.NetworkCallback() {
        // Здесь только снимок адресов и сигнал потоку; запросов и поиска ПК в callback нет.
        override fun onLinkPropertiesChanged(network: Network, properties: LinkProperties) {
            val addresses = properties.linkAddresses.map { it.toString() }.sorted().joinToString(",")
            if (localLinks.put(network, addresses) != addresses) nudge()
        }

        override fun onLost(network: Network) {
            localLinks.remove(network)
            nudge()
        }
    }

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onCreate() {
        super.onCreate()
        val nm = getSystemService(NotificationManager::class.java)
        nm.createNotificationChannel(NotificationChannel(CHANNEL, "Связь с ПК", NotificationManager.IMPORTANCE_MIN))
        startForeground(1, note(), ServiceInfo.FOREGROUND_SERVICE_TYPE_CONNECTED_DEVICE)
        running = true
        val powerFilter = IntentFilter(Intent.ACTION_POWER_CONNECTED).apply { addAction(Intent.ACTION_POWER_DISCONNECTED) }
        if (Build.VERSION.SDK_INT >= 33) registerReceiver(powerEvents, powerFilter, Context.RECEIVER_NOT_EXPORTED)
        else registerReceiver(powerEvents, powerFilter)
        Thread({ loop() }, "poll").start()
        photoThread = Thread({ photoLoop() }, "photos").also { it.start() }
        contentResolver.registerContentObserver(MediaStore.Images.Media.EXTERNAL_CONTENT_URI, true, mediaObserver)
        contentResolver.registerContentObserver(MediaStore.Video.Media.EXTERNAL_CONTENT_URI, true, mediaObserver)
        // Wi-Fi без интернета тоже подходит для ПК; default callback может видеть только мобильную сеть/VPN.
        // clearCapabilities() - только с Android 11, а minSdk 29: на Android 10 служба падала бы при старте.
        // Снять требование интернета хватает: NOT_VPN и так в запросе по умолчанию.
        getSystemService(ConnectivityManager::class.java).registerNetworkCallback(
            NetworkRequest.Builder()
                .removeCapability(NetworkCapabilities.NET_CAPABILITY_INTERNET)
                .addCapability(NetworkCapabilities.NET_CAPABILITY_NOT_VPN)
                .addTransportType(NetworkCapabilities.TRANSPORT_WIFI)
                .addTransportType(NetworkCapabilities.TRANSPORT_ETHERNET).build(), net,
        )
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        nudge()
        kickPhotos()
        refresh()
        return START_STICKY
    }

    override fun onDestroy() {
        running = false
        online = false
        nudge()
        kickPhotos()
        contentResolver.unregisterContentObserver(mediaObserver)
        unregisterReceiver(powerEvents)
        try {
            getSystemService(ConnectivityManager::class.java).unregisterNetworkCallback(net)
        } catch (_: Exception) {
        }
        super.onDestroy()
    }

    private fun note(): Notification {
        val prefs = Prefs(this)
        val pc = prefs.pc.ifEmpty { "ПК" }
        val open = PendingIntent.getActivity(
            this, 0, Intent(this, MainActivity::class.java), PendingIntent.FLAG_IMMUTABLE,
        )
        val clip = PendingIntent.getActivity(
            this, 1, ClipActivity.own(this).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK),
            PendingIntent.FLAG_IMMUTABLE,
        )
        val b = Notification.Builder(this, CHANNEL)
            .setSmallIcon(R.drawable.ic_notify)
            .setContentTitle(if (online) "$pc на связи" else "$pc не на связи")
            .setContentIntent(open)
            .setOngoing(true)
            .addAction(Notification.Action.Builder(null, "Буфер на ПК", clip).build())
        if (!online && prefs.wol.isNotEmpty()) {
            val w = PendingIntent.getBroadcast(
                this, 2, Intent(this, WakeReceiver::class.java), PendingIntent.FLAG_IMMUTABLE,
            )
            b.addAction(Notification.Action.Builder(null, "Разбудить ПК", w).build())
        }
        return b.build()
    }

    private fun refresh() = main.post {
        if (running) getSystemService(NotificationManager::class.java).notify(1, note())
    }

    private fun nudge() = synchronized(wake) {
        ++networkEpoch
        wake.notifyAll()
    }

    private fun pause(ms: Long, epoch: Long) = synchronized(wake) {
        val until = SystemClock.elapsedRealtime() + ms
        while (running && networkEpoch == epoch) {
            val left = until - SystemClock.elapsedRealtime()
            if (left <= 0) break
            wake.wait(left)
        }
    }

    private fun setOnline(v: Boolean) {
        if (online == v) return
        online = v
        refresh()
        if (v) kickPhotos()
    }

    // долгий опрос: ПК отвечает сразу при перемене или через 20 с
    private fun loop() {
        var seq = -1L
        var first = true
        var infoAt = 0L
        val retry = Pure.Reconnect()
        var previousEpoch = -1L
        while (running) {
            val epoch = synchronized(wake) { networkEpoch }
            if (epoch != previousEpoch) {
                retry.networkChanged()
                previousEpoch = epoch
                seq = -1
                first = true
                infoAt = 0L
            }
            if (!Link.connected(this)) {
                pause(30000, epoch)
                continue
            }
            try {
                val r = Link.call(this, "GET", "poll?seq=$seq", timeout = 30000)
                if (r.code != 200) throw IOException("код ${r.code}")
                val o = org.json.JSONObject(r.text)
                seq = o.optLong("seq", -1)
                if (!running || synchronized(wake) { networkEpoch != epoch }) continue
                retry.connected()
                val pauseWas = pcBulkPause
                pcBulkPause = o.optBoolean("bulkPause", false)
                if (pauseWas && !pcBulkPause) kickPhotos()
                setOnline(true)
                val clip = o.optString("clip", "")
                if (first) {
                    lastClip = clip  // старое содержимое буфера ПК при подключении не подсовываем
                    first = false
                } else if (clip.isNotEmpty() && clip != lastClip && Prefs(this).clip) {
                    lastClip = clip
                    main.post {
                        getSystemService(ClipboardManager::class.java)
                            .setPrimaryClip(ClipData.newPlainText("С компьютера", clip))
                    }
                }
                if (infoAt == 0L || SystemClock.elapsedRealtime() - infoAt > 10 * 60000) {  // MAC для Wake-on-LAN
                    infoAt = SystemClock.elapsedRealtime()
                    val info = Link.call(this, "GET", "pcinfo")
                    if (info.code == 200) Link.savePcInfo(this, info.text)
                }
            } catch (_: Exception) {
                if (!running) break
                setOnline(false)
                first = true
                seq = -1
                if (synchronized(wake) { networkEpoch != epoch }) continue
                val delay = retry.failed()
                if (Link.hasLan(this) && retry.searchDue(SystemClock.elapsedRealtime())) {
                    val found = try {
                        Link.relocate(this) { running && synchronized(wake) { networkEpoch == epoch } }
                    } catch (_: Exception) { false }
                    if (found) {
                        retry.connected()
                        continue
                    }
                }
                pause(delay, epoch)
            }
        }
    }

    private fun kickPhotos() {
        photoWanted = true
        synchronized(photoKick) { photoKick.notifyAll() }
    }

    private fun photoLoop() {
        while (running) {
            synchronized(photoKick) { if (!photoWanted && running) photoKick.wait(5 * 60000) }
            photoWanted = false
            Thread.sleep(3000)  // камера дописывает файл и выставляет его в галерею
            if (online && Prefs(this).photos && mediaAllowed() && autoTransferAllowed()) {
                try {
                    syncKind("img", MediaStore.Images.Media.EXTERNAL_CONTENT_URI)
                    syncKind("vid", MediaStore.Video.Media.EXTERNAL_CONTENT_URI)
                } catch (_: Exception) {  // ПК пропал посреди файла - докачаем в следующий раз
                }
            }
        }
    }

    private fun mediaAllowed() = if (Build.VERSION.SDK_INT >= 33) granted(Manifest.permission.READ_MEDIA_IMAGES)
    else granted(Manifest.permission.READ_EXTERNAL_STORAGE)

    private fun autoTransferAllowed(): Boolean {
        val prefs = Prefs(this)
        val power = registerReceiver(null, IntentFilter(Intent.ACTION_BATTERY_CHANGED))
        val charging = (power?.getIntExtra(BatteryManager.EXTRA_PLUGGED, 0) ?: 0) != 0
        val metered = getSystemService(ConnectivityManager::class.java).isActiveNetworkMetered
        return Pure.autoTransferAllowed(prefs.gentleTransfer, charging, metered, pcBulkPause)
    }

    private fun syncKind(kind: String, uri: Uri) {
        val prefs = Prefs(this)
        val last = prefs.lastMedia(kind)
        if (last < 0) {  // включили только что: отправляем то, что снято с этой минуты, а не всю галерею
            prefs.setLastMedia(kind, Photos.maxId(this, uri))
            return
        }
        val cols = arrayOf(
            MediaStore.MediaColumns._ID, MediaStore.MediaColumns.DISPLAY_NAME, MediaStore.MediaColumns.SIZE,
            MediaStore.MediaColumns.DATE_TAKEN, MediaStore.MediaColumns.RELATIVE_PATH,
        )
        contentResolver.query(
            uri, cols, "${MediaStore.MediaColumns._ID} > ? AND ${MediaStore.MediaColumns.IS_PENDING} = 0",
            arrayOf(last.toString()), "${MediaStore.MediaColumns._ID} ASC",
        )?.use { c ->
            while (c.moveToNext() && running && online) {
                val id = c.getLong(0)
                val size = c.getLong(2)
                if (Pure.cameraPath(c.getString(4)) && size > 0)
                    upload(ContentUris.withAppendedId(uri, id), id, c.getString(1) ?: "$id", size, c.getLong(3))
                prefs.setLastMedia(kind, id)
            }
        }
    }

    // кусками по 4 МБ; ПК помнит, сколько принял (GET /up?id=), - после обрыва продолжаем с того места
    private fun upload(uri: Uri, mediaId: Long, name: String, size: Long, taken: Long) {
        val id = Pure.uploadId(mediaId, size)
        val q = "id=$id&total=$size&kind=photo&mt=$taken&name=" + URLEncoder.encode(name, "UTF-8")
        var off = Pure.haveOf(Link.call(this, "GET", "up?id=$id").text).coerceAtLeast(0)
        val buf = ByteArray(4 shl 20)
        var tries = 0
        while (off < size) {
            if (!running) throw IOException("стоп")
            contentResolver.openInputStream(uri)?.use { s ->
                var skip = off
                while (skip > 0) {
                    val k = s.skip(skip)
                    if (k <= 0) throw IOException("файл короче")
                    skip -= k
                }
                while (off < size) {
                    if (!autoTransferAllowed()) throw IOException("бережный режим: продолжим позже")
                    var len = 0
                    while (len < buf.size) {
                        val k = s.read(buf, len, buf.size - len)
                        if (k < 0) break
                        len += k
                    }
                    if (len == 0) throw IOException("файл короче")
                    val r = Link.put(this, "up?$q&off=$off", buf, len)
                    val have = Pure.haveOf(r.text)
                    if (r.code == 409 && have >= 0) {  // ПК ждёт с другого места - открыть файл заново
                        if (++tries > 3) throw IOException("не сходится")
                        off = have
                        return@use
                    }
                    if (r.code != 200 || have < 0) throw IOException("код ${r.code}")
                    if (have != off + len && have < size) {  // ПК забыл файл (перезапуск) - с его места заново
                        if (++tries > 3) throw IOException("не сходится")
                        off = have
                        return@use
                    }
                    off = have
                }
            } ?: throw IOException("файл не открылся")
        }
    }
}

object Photos {
    fun maxId(ctx: Context, uri: Uri): Long =
        ctx.contentResolver.query(
            uri, arrayOf(MediaStore.MediaColumns._ID), null, null, "${MediaStore.MediaColumns._ID} DESC",
        )?.use { if (it.moveToFirst()) it.getLong(0) else 0L } ?: 0L
}
