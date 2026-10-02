package com.csheets.camlink

// ============ CamLink: фоновый мост «устройство ⇄ ИИ» ============
//
// Раньше вся логика жила в MainActivity и умирала вместе с экраном:
// при уходе в фон/погашении экрана Android усыпал сеть и опрос.
// Теперь:
//   * BridgeService — foreground-сервис (уведомление «мост активен»),
//     владеет WifiNetworkSpecifier-запросом, WifiLock + WakeLock —
//     связь и опрос /api/state живут с выключенным экраном;
//   * BridgeBus — журнал + события для UI (Activity только рисует);
//   * espRequest/espNetwork — общий транспорт (его же использует FileActivity).

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.net.ConnectivityManager
import android.net.Network
import android.net.NetworkCapabilities
import android.net.NetworkRequest
import android.net.wifi.WifiManager
import android.net.wifi.WifiNetworkSpecifier
import android.os.Handler
import android.os.IBinder
import android.os.Looper
import android.os.PowerManager
import android.util.Base64
import org.json.JSONArray
import org.json.JSONObject
import java.io.ByteArrayOutputStream
import java.io.IOException
import java.net.ConnectException
import java.net.HttpURLConnection
import java.net.InetSocketAddress
import java.net.NoRouteToHostException
import java.net.Proxy
import java.net.SocketException
import java.net.SocketTimeoutException
import java.net.URL
import java.net.URLEncoder
import java.net.UnknownHostException
import java.util.TimeZone
import java.util.concurrent.Executors

const val ANSWER_MAX = 3800           // буфер ответа на устройстве — 4096
const val DEFAULT_PROMPT =
    "Ты помогаешь на контрольной. Распознай вопрос на фото и дай краткий " +
    "правильный ответ: если тест с вариантами — укажи букву и одно предложение " +
    "почему; если развёрнутый вопрос — 2-3 предложения. Отвечай только по делу."

private const val ESP_IP = "192.168.4.1"

// Handle сети ESP — принадлежит сервису (живёт, пока живёт сервис)
@Volatile private var espNetwork: Network? = null

// Момент последнего УСПЕШНОГО запроса — «линк работал» (для шторм-капа EPERM)
@Volatile private var lastReqOkAt = 0L

// espRequest сбрасывает сюда при смерти линка (invalidateEsp сервиса)
private var espDeadHandler: ((String) -> Unit)? = null

// Фото, прикреплённое в Activity (📎), — для doSend читает сервис
@Volatile var pendingPhoto: ByteArray? = null

// HTTP к устройству: сокет принудительно привязан к сети ESP, иначе трафик
// уйдёт в мобильный интернет и 192.168.4.1 будет недоступен.
fun espRequest(path: String, method: String = "GET",
               headers: Map<String, String>? = null,
               body: ByteArray? = null): Pair<Int, ByteArray> {
    val net = espNetwork ?: throw IOException("нет связи с устройством")
    val url = URL("http://$ESP_IP$path")
    try {
        // Network.openConnection — трафик идёт только по сети ESP
        val conn = net.openConnection(url) as HttpURLConnection
        conn.apply {
            connectTimeout = 5_000
            readTimeout = if (path.startsWith("/api/photo") || path.startsWith("/api/file"))
                30_000 else 8_000
            requestMethod = method
            headers?.forEach { (k, v) -> setRequestProperty(k, v) }
            // Часы устройства: NTP у ESP нет (он не в интернете) —
            // присылаем своё время и свою часовую зону на каждом запросе
            setRequestProperty("X-Time", (System.currentTimeMillis() / 1000).toString())
            setRequestProperty("X-Tz",
                (TimeZone.getDefault().getOffset(System.currentTimeMillis()) / 1000).toString())
            if (body != null) {
                doOutput = true
                outputStream.use { it.write(body) }
            }
        }
        val code = conn.responseCode
        val stream = if (code in 200..299) conn.inputStream else conn.errorStream
        val data = stream?.use { inp ->
            val out = ByteArrayOutputStream()
            val buf = ByteArray(16 * 1024)
            while (true) {
                val n = inp.read(buf)
                if (n < 0) break
                out.write(buf, 0, n)
            }
            out.toByteArray()
        } ?: ByteArray(0)
        conn.disconnect()
        lastReqOkAt = System.currentTimeMillis()
        return code to data
    } catch (e: IOException) {
        val m = e.message.orEmpty()
        if (m.contains("Binding socket") || m.contains("EPERM") ||
            m.contains("ENONET") || m.contains("unreachable")) {
            espDeadHandler?.invoke(e.message ?: "сеть недействительна")
            throw IOException("сеть устройства пропала — переподключаюсь…")
        }
        throw e
    }
}

// ---------- Шина событий «сервис → UI» ----------
// Сервис пишет сюда; Activity при открытии подписывается и получает
// живой лог/статус, а после ухода в фон записи копятся в истории.

object BridgeBus {
    private const val HISTORY_MAX = 500
    private val history = ArrayList<String>()

    @Volatile var onLog: ((String) -> Unit)? = null
    @Volatile var onStatus: ((String) -> Unit)? = null
    @Volatile var onLink: ((Boolean) -> Unit)? = null
    @Volatile var onSendDone: (() -> Unit)? = null
    @Volatile var onAskDone: (() -> Unit)? = null
    @Volatile var lastStatus: String = ""
    @Volatile var linked: Boolean = false

    fun log(line: String) {
        synchronized(history) {
            history.add(line)
            while (history.size > HISTORY_MAX) history.removeAt(0)
        }
        onLog?.invoke(line)
    }

    fun replay(): List<String> = synchronized(history) { history.toList() }

    fun status(s: String) {
        lastStatus = s
        onStatus?.invoke(s)
    }

    fun link(connected: Boolean) {
        linked = connected
        onLink?.invoke(connected)
    }
}

class BridgeService : Service() {

    companion object {
        @Volatile var instance: BridgeService? = null

        // Вопрос с клавиатуры телефона → устройство → ИИ → ответ обратно
        fun submitQuestion(text: String, photo: ByteArray?): Boolean {
            val s = instance ?: return false
            s.exec.execute { s.doSend(text, photo) }
            return true
        }

        // Фото с устройства → ИИ → ответ (кнопка «📷» и «Отправить» с устройства)
        fun photoAsk(): Boolean {
            val s = instance ?: return false
            s.exec.execute { s.doPhotoAsk() }
            return true
        }

        fun reconnect() {
            val s = instance ?: return
            s.mainHandler.post { s.reconnectEsp() }
        }
    }

    private lateinit var cm: ConnectivityManager
    private val mainHandler = Handler(Looper.getMainLooper())
    private val exec = Executors.newSingleThreadExecutor()
    private val prefs by lazy { getSharedPreferences("camlink", Context.MODE_PRIVATE) }

    private var netCallback: ConnectivityManager.NetworkCallback? = null
    private var lastSendSeen = 0L        // последний seen send с устройства
    private var reconnectPending = false  // запланировано авто-переподключение
    private var retriedSilent = false     // уже дали шанс тихому автовосстановлению
    private var bindFails = 0             // окно счётчика EPERM (шторм = ребуты ESP)
    private var bindFailsWin = 0L
    @Volatile private var lastAvailableAt = 0L       // время onAvailable, мс
    @Volatile private var lostSeenSinceAvail = false // был ли onLost после него
    @Volatile private var connectFailed = false      // запрос не дал сети (диалог не подтверждён)

    private lateinit var wifiLock: WifiManager.WifiLock
    private lateinit var wakeLock: PowerManager.WakeLock

    // Устройство может само попросить отправить фото (кнопка «Отправить»
    // в чате «ИИ») — опрашиваем /api/state и подхватываем.
    private val pollRunnable = object : Runnable {
        override fun run() {
            if (espNetwork != null) {
                exec.execute { pollSend() }
            }
            mainHandler.postDelayed(this, 2000)
        }
    }

    override fun onCreate() {
        super.onCreate()
        instance = this
        cm = getSystemService(Context.CONNECTIVITY_SERVICE) as ConnectivityManager
        startAsForeground()

        // Радио и CPU не засыпают с выключенным экраном — иначе
        // система урежет Wi-Fi и оборвёт опрос/сокеты в фоне.
        val wm = applicationContext.getSystemService(Context.WIFI_SERVICE) as WifiManager
        wifiLock = wm.createWifiLock(WifiManager.WIFI_MODE_FULL_LOW_LATENCY, "camlink:wifi")
        wifiLock.acquire()
        val pm = getSystemService(Context.POWER_SERVICE) as PowerManager
        wakeLock = pm.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "camlink:bridge")
        wakeLock.acquire()

        espDeadHandler = { invalidateEsp(it) }
        connectEsp()
        mainHandler.postDelayed(pollRunnable, 2000)
        BridgeBus.log("Мост запущен в фоне (уведомление «CamLink»)")
    }

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        // На каждый startForegroundService — подтверждаем foreground-состояние
        // (иначе система убьёт сервис за 5 с без вызова startForeground)
        startAsForeground()
        return START_STICKY
    }

    // Вызывает Activity при открытии: если прошлая попытка подключения
    // не удалась (диалог не показывался в фоне) — повторяем при ней.
    fun ensureConnected() {
        if (connectFailed) {
            BridgeBus.log("↻ Повтор подключения…")
            reconnectEsp()
        }
    }

    override fun onDestroy() {
        mainHandler.removeCallbacks(pollRunnable)
        netCallback?.let { cm.unregisterNetworkCallback(it) }
        netCallback = null
        espDeadHandler = null
        if (wifiLock.isHeld) wifiLock.release()
        if (wakeLock.isHeld) wakeLock.release()
        instance = null
        exec.shutdownNow()
        super.onDestroy()
    }

    private fun startAsForeground() {
        val nm = getSystemService(NotificationManager::class.java)
        val channel = NotificationChannel("bridge", "Фоновый мост",
            NotificationManager.IMPORTANCE_LOW)
        channel.setShowBadge(false)
        nm.createNotificationChannel(channel)
        val notification = Notification.Builder(this, "bridge")
            .setSmallIcon(android.R.drawable.ic_dialog_info)
            .setContentTitle("CamLink — мост активен")
            .setContentText("Связь с устройством и ИИ работают в фоне")
            .setOngoing(true)
            .build()
        startForeground(1, notification,
            ServiceInfo.FOREGROUND_SERVICE_TYPE_CONNECTED_DEVICE)
    }

    // ---------- Подключение к SoftAP устройства ----------

    @Suppress("DEPRECATION")
    private fun connectEsp() {
        if (netCallback != null) return
        connectFailed = false

        val wifi = applicationContext.getSystemService(Context.WIFI_SERVICE) as WifiManager
        if (!wifi.isWifiEnabled)
            BridgeBus.log("! WiFi выключен — подключение к устройству невозможно")

        val ssid = prefs.getString("ssid", "CSCAM")!!
        val pass = prefs.getString("pass", "cscam1234")!!
        BridgeBus.log("Запрос подключения к «$ssid» (системный диалог — «Подключиться»)…")
        if (vpnActive()) {
            BridgeBus.log("! Активен VPN — он может запрещать привязку сокета к устройству (EPERM)")
            if (prefs.getString("ai_proxy", "")!!.isBlank())
                BridgeBus.log("  ИИ при VPN может блокироваться — укажите порт Happ в ⚙ «Прокси для ИИ»")
        }

        val specifier = try {
            WifiNetworkSpecifier.Builder()
                .setSsid(ssid)
                .setWpa2Passphrase(pass)
                .build()
        } catch (e: IllegalArgumentException) {
            BridgeBus.status("Проверьте пароль в Настройках")
            BridgeBus.log("Ошибка сети: ${e.message}")
            return
        }

        val request = NetworkRequest.Builder()
            .addTransportType(NetworkCapabilities.TRANSPORT_WIFI)
            .setNetworkSpecifier(specifier)
            .build()

        val cb = object : ConnectivityManager.NetworkCallback() {
            override fun onAvailable(network: Network) {
                espNetwork = network
                lastAvailableAt = System.currentTimeMillis()
                lostSeenSinceAvail = false
                retriedSilent = false
                connectFailed = false
                BridgeBus.log("✔ Подключено к устройству ($ssid) [сеть $network]")
                BridgeBus.status("Подключено — чат на устройстве")
                BridgeBus.link(true)
            }

            override fun onLost(network: Network) {
                lostSeenSinceAvail = true
                val since = if (lastAvailableAt > 0)
                    "${System.currentTimeMillis() - lastAvailableAt} мс после подключения"
                else "вне подключения"
                if (espNetwork == network) espNetwork = null
                BridgeBus.log("✖ onLost [сеть $network] — $since")
                BridgeBus.status("Нет связи — жду переподключения…")
                BridgeBus.link(false)
                scheduleAutoReconnect()
            }

            override fun onUnavailable() {
                connectFailed = true
                BridgeBus.status("Не удалось подключиться — нажмите ↻")
                BridgeBus.log("✖ Диалог подключения не подтверждён")
            }
        }
        netCallback = cb
        try {
            // Без таймаута: запрос держится, пока живёт сервис
            cm.requestNetwork(request, cb)
        } catch (e: SecurityException) {
            BridgeBus.status("Нет разрешения сети: ${e.message}")
            BridgeBus.log("requestNetwork: ${e.message}")
            netCallback = null
            return
        }
        BridgeBus.status("Подключение к устройству…")
        // Подсказка, если системный диалог подключения не подтвердили
        mainHandler.postDelayed({
            if (espNetwork == null && netCallback == cb) {
                connectFailed = true
                BridgeBus.status("Не подключено — нажмите ↻")
                BridgeBus.log("! Системный диалог подключения не подтверждён")
            }
        }, 45_000)
    }

    private fun reconnectEsp() {
        netCallback?.let {
            cm.unregisterNetworkCallback(it)
            netCallback = null
        }
        espNetwork = null
        connectEsp()
    }

    // Сеть выпала (перезагрузка устройства, уход с WiFi). Сначала даём
    // системе 15 с пройти мимо тихо (запрос держится — сессию может
    // восстановить без диалога), и только потом перевыполняем запрос
    // (новый системный диалог). Один флаг — без шквала повторов.
    private fun scheduleAutoReconnect() {
        mainHandler.post {
            if (reconnectPending) return@post
            reconnectPending = true
            mainHandler.postDelayed({
                reconnectPending = false
                if (espNetwork != null) return@postDelayed
                if (!retriedSilent) {
                    retriedSilent = true
                    BridgeBus.log("↻ жду автовосстановления сессии…")
                    scheduleAutoReconnect()
                } else {
                    BridgeBus.log("↻ Авто-переподключение…")
                    reconnectEsp()
                }
            }, 15_000)
        }
    }

    // Сеть умерла между выбором и привязкой сокета
    // («Binding socket to network N failed: EPERM/ENONET») — сбросить
    // старую ссылку и дать цепочке восстановиться
    private fun invalidateEsp(reason: String) {
        mainHandler.post {
            val since = if (lastAvailableAt > 0)
                "onAvailable был ${System.currentTimeMillis() - lastAvailableAt} мс назад"
            else "onAvailable не было"
            val lost = if (lostSeenSinceAvail) "onLost: был" else "onLost: НЕ был"
            BridgeBus.log("! Связь прервана: $reason")
            BridgeBus.log("  └ $since; $lost")
            espNetwork = null
            BridgeBus.link(false)

            // VPN — известная системная причина EPERM: запрещает привязку
            // сокета к локальной сети. Переподключение тут не поможет —
            // не крутим шторм.
            if (vpnActive()) {
                BridgeBus.log("! На телефоне активен VPN — он блокирует связь с устройством (EPERM).")
                BridgeBus.log("  Отключите VPN или добавьте CamLink в его исключения/обход.")
                BridgeBus.status("EPERM — мешает VPN (см. лог)")
                return@post
            }

            // Шторм-кап: линк НЕ работал 2 минуты и 4 ошибки подряд —
            // авто-переподключение не лечит причину, остановиться.
            val now = System.currentTimeMillis()
            if (now - lastReqOkAt < 60_000) bindFails = 0      // линк работал недавно = реальные обрывы
            if (now - bindFailsWin > 120_000) { bindFailsWin = now; bindFails = 0 }
            bindFails++
            if (bindFails >= 4) {
                BridgeBus.log("! Авто-переподключение остановлено ($bindFails подряд за 2 мин).")
                BridgeBus.log("  Причина на стороне телефона/линка — переподключение не лечит.")
                BridgeBus.log("  Нажмите ↻ после устранения причины.")
                BridgeBus.status("Переподключение остановлено — нажмите ↻")
                return@post
            }
            if (bindFails == 3)
                BridgeBus.log("! Частые обрывы — если устройство не перезагружается, " +
                    "проверьте питание: 5.0V под нагрузкой, ≥2A, " +
                    "конденсатор 470–1000µF.")
            BridgeBus.status("Переподключение к устройству…")
            scheduleAutoReconnect()
        }
    }

    private fun vpnActive(): Boolean {
        val caps = cm.activeNetwork?.let { cm.getNetworkCapabilities(it) }
        return caps?.hasTransport(NetworkCapabilities.TRANSPORT_VPN) == true
    }

    // ---------- Отправка (вызывается из Activity через companion) ----------

    // Текст (± фото) → вопрос сразу на устройство → ИИ → ответ на устройство
    private fun doSend(text: String, photo: ByteArray?) {
        val headers = mutableMapOf("X-Id" to "chat")
        if (text.isNotEmpty()) {
            headers["X-Question"] = URLEncoder.encode(text, "UTF-8")
            BridgeBus.log("вопрос: ${text.length} симв → устройство")
        }
        try {
            // 1) вопрос мгновенно в чат устройства (пузырь «…»)
            if (text.isNotEmpty()) {
                try {
                    val (code, _) = espRequest("/api/question", "POST",
                        mapOf("X-Question" to headers["X-Question"]!!), ByteArray(0))
                    if (code != 200) BridgeBus.log("устройство: вопрос не принят (HTTP $code)")
                } catch (e: Exception) {
                    BridgeBus.log("вопрос не доставлен: ${e.message}")
                }
            }

            // 2) ИИ через мобильный интернет, 3) ответ обратно
            BridgeBus.status("ИИ думает…")
            val answer = callAi(photo, text).trim()
            BridgeBus.log("ответ ИИ: ${answer.length} симв")
            pendingPhoto = null

            val (code, _) = espRequest("/api/answer", "POST",
                headers + ("Content-Type" to "text/plain; charset=utf-8"),
                answer.take(ANSWER_MAX).toByteArray(Charsets.UTF_8))
            if (code == 200) BridgeBus.status("Готово — ответ на экране устройства")
            else {
                BridgeBus.log("устройство: ответ не принят (HTTP $code)")
                BridgeBus.status("ИИ ответил, устройство недоступно")
            }
        } catch (e: Exception) {
            BridgeBus.log("ОШИБКА: ${e.message}")
            BridgeBus.status("Ошибка: ${e.message}")
            // сорванный вопрос не должен остаться «…» навсегда
            try {
                if (text.isNotEmpty())
                    espRequest("/api/answer", "POST",
                        headers + ("Content-Type" to "text/plain; charset=utf-8"),
                        "⚠ ${e.message}".toByteArray(Charsets.UTF_8))
            } catch (_: Exception) {}
        } finally {
            BridgeBus.onSendDone?.invoke()
        }
    }

    private fun doPhotoAsk() {
        try {
            runPhotoAsk()
        } catch (e: Exception) {
            BridgeBus.log("ОШИБКА: ${e.message}")
            BridgeBus.status("Ошибка: ${e.message}")
        } finally {
            BridgeBus.onAskDone?.invoke()
        }
    }

    // Устройство просит отправить фото (кнопка «Отправить» в чате «ИИ»)
    private fun pollSend() {
        try {
            val (c, d) = espRequest("/api/state")
            if (c != 200) return
            val send = JSONObject(String(d, Charsets.UTF_8)).optLong("send", 0)
            if (send > lastSendSeen) {
                if (exec.isShutdown) return
                lastSendSeen = send
                BridgeBus.log("Устройство просит отправить фото (#$send)")
                doPhotoAsk()
            }
        } catch (_: Exception) {
            // связи нет — просто ждём следующего опроса
        }
    }

    // Общий сценарий «фото с устройства → ИИ → ответ»: и для кнопки «📷»,
    // и для запроса «Отправить» с устройства. Вопрос берётся из последней
    // «висящей» записи чата (пользователь мог набрать его на телефоне).
    private fun runPhotoAsk() {
        val (sc, sd) = espRequest("/api/state")
        if (sc != 200) throw IOException("устройство недоступно (HTTP $sc)")
        val st = JSONObject(String(sd, Charsets.UTF_8))
        if (st.getInt("id") == 0) {
            BridgeBus.status("Фото нет — снимите его на устройстве")
            return
        }

        BridgeBus.status("Качаю фото с устройства…")
        val (pcode, photo) = espRequest("/api/photo")
        if (pcode != 200 || photo.isEmpty())
            throw IOException("фото недоступно (HTTP $pcode)")
        BridgeBus.log("Фото: ${photo.size / 1024} КБ")

        val question = detectPendingQuestion()
        if (question.isNotEmpty()) BridgeBus.log("вопрос с устройства: ${question.length} симв")

        BridgeBus.status("ИИ думает…")
        val answer = callAi(photo, question).trim()
        BridgeBus.log("ответ ИИ: ${answer.length} симв")

        val headers = mutableMapOf(
            "X-Id" to "auto",
            "Content-Type" to "text/plain; charset=utf-8")
        if (question.isNotEmpty())
            headers["X-Question"] = URLEncoder.encode(question, "UTF-8")
        val (code, _) = espRequest("/api/answer", "POST", headers,
            answer.take(ANSWER_MAX).toByteArray(Charsets.UTF_8))
        if (code == 200) BridgeBus.status("Готово — ответ на экране устройства")
        else BridgeBus.status("ответ не принят (HTTP $code)")
    }

    // Последняя «висящая» вопрос-запись (t=2), набранная на телефоне
    private fun detectPendingQuestion(): String {
        return try {
            val (c, d) = espRequest("/api/history")
            if (c != 200) return ""
            val arr = JSONArray(String(d, Charsets.UTF_8))
            if (arr.length() == 0) return ""
            val last = arr.getJSONObject(arr.length() - 1)
            if (last.getInt("t") == 2) last.optString("q", "") else ""
        } catch (_: Exception) {
            ""
        }
    }

    // ---------- Изображение ----------

    private fun resize(jpeg: ByteArray, maxSide: Int, quality: Int): ByteArray {
        val bmp = BitmapFactory.decodeByteArray(jpeg, 0, jpeg.size) ?: return jpeg
        val scale = maxSide.toFloat() / maxOf(bmp.width, bmp.height)
        val out = if (scale < 1f) {
            Bitmap.createScaledBitmap(bmp,
                (bmp.width * scale).toInt().coerceAtLeast(1),
                (bmp.height * scale).toInt().coerceAtLeast(1), true)
        } else bmp
        val bos = ByteArrayOutputStream()
        out.compress(Bitmap.CompressFormat.JPEG, quality, bos)
        if (out !== bmp) out.recycle()
        bmp.recycle()
        return bos.toByteArray()
    }

    // ---------- Вызов API ИИ (через мобильный интернет) ----------

    private fun callAi(photo: ByteArray?, userText: String): String {
        val key = prefs.getString("key", "")!!
        if (key.isBlank()) throw IOException("нет API-ключа (⚙ Настройки)")
        val provider = prefs.getString("provider", "gemini")!!
        val model = prefs.getString("model", "")!!.ifBlank {
            when (provider) {
                "openai"     -> "gpt-4o-mini"
                "openrouter" -> "openrouter/free"   // роутер: сам выберет free-модель с vision
                else         -> "gemini-3.6-flash"        // бесплатный тариф AI Studio
            }
        }

        val openAiUrl =
            if (provider == "openrouter") "https://openrouter.ai/api/v1/chat/completions"
            else "https://api.openai.com/v1/chat/completions"

        if (photo == null) {   // текстовый вопрос
            val call: () -> String = {
                if (provider == "gemini") geminiText(key, userText, model)
                else openAiText(key, userText, openAiUrl, model)
            }
            return askWithRetry(call)
        }

        // Фото: промпт-инструкция (+ вопрос пользователя, если был набран)
        val prompt = prefs.getString("prompt", DEFAULT_PROMPT)!!
        val visionPrompt =
            if (userText.isEmpty()) prompt else "$prompt\n\nВопрос: $userText"
        val resized = resize(photo, 1568, 85)
        val b64 = Base64.encodeToString(resized, Base64.NO_WRAP)
        val call: () -> String = {
            if (provider == "gemini") callGemini(key, visionPrompt, b64, model)
            else callOpenAi(key, visionPrompt, b64, openAiUrl, model)
        }
        return askWithRetry(call)
    }

    // Повторные попытки: роутер может дать мусорную модель (пустой ответ
    // или строку "null") — переспрашиваем до 3 раз, роутер каждый раз
    // выбирает новую модель.
    private fun askWithRetry(call: () -> String): String {
        for (attempt in 1..3) {
            val ans = try {
                call()
            } catch (e: Exception) {
                if (attempt == 3) throw e
                BridgeBus.log("! ИИ: попытка $attempt не удалась (${e.message?.take(80)}) — повтор…")
                Thread.sleep(1500)
                continue
            }
            if (ans.isNotEmpty() && !ans.equals("null", ignoreCase = true)) return ans
            BridgeBus.log("ИИ: пустой ответ (попытка $attempt/3) — повтор…")
            Thread.sleep(1000)
        }
        throw IOException("ИИ вернул пустой ответ (3 попытки)")
    }

    // Разбор ответа OpenRouter/OpenAI: content — строка, либо массив
    // частей (так vision-модели иногда отдают), либо null.
    private fun extractContent(resp: JSONObject): String {
        val msg = resp.optJSONArray("choices")?.optJSONObject(0)
            ?.optJSONObject("message") ?: return ""
        val c = msg.opt("content") ?: return ""
        return when (c) {
            is String -> c.trim()
            is JSONArray -> (0 until c.length()).joinToString("\n") { i ->
                val p = c.optJSONObject(i) ?: return@joinToString ""
                if (p.optString("type") == "text") p.optString("text") else ""
            }.trim()
            else -> ""   // JSON null и прочее = пусто → повтор
        }
    }

    private fun extractGemini(resp: JSONObject): String {
        val parts = resp.optJSONArray("candidates")?.optJSONObject(0)
            ?.optJSONObject("content")?.optJSONArray("parts") ?: return ""
        return (0 until parts.length()).joinToString("\n") {
            parts.optJSONObject(it)?.optString("text") ?: ""
        }.trim()
    }

    // Прокси для запросов к ИИ (например, локальный порт Happ:
    // в Happ включить «Разрешить LAN-подключения» → вписать порт сюда).
    // Формат: host:port (HTTP) | http://host:port | socks5://host:port.
    // Локальные запросы к устройству (espRequest) прокси НЕ используют.
    private fun aiProxy(): Proxy? {
        val raw = prefs.getString("ai_proxy", "")!!.trim()
        if (raw.isEmpty()) return null
        var s = raw
        val type = when {
            s.startsWith("socks5://") -> { s = s.removePrefix("socks5://"); Proxy.Type.SOCKS }
            s.startsWith("socks://")  -> { s = s.removePrefix("socks://");  Proxy.Type.SOCKS }
            s.startsWith("http://")  -> { s = s.removePrefix("http://");  Proxy.Type.HTTP }
            s.startsWith("https://") -> { s = s.removePrefix("https://"); Proxy.Type.HTTP }
            else -> Proxy.Type.HTTP
        }
        val i = s.lastIndexOf(':')
        if (i <= 0 || i == s.length - 1)
            throw IOException("прокси для ИИ: нужен формат host:port")
        val port = s.substring(i + 1).toIntOrNull()
            ?: throw IOException("прокси для ИИ: порт должен быть числом")
        return Proxy(type, InetSocketAddress(s.substring(0, i), port))
    }

    private fun postJson(urlStr: String, json: JSONObject,
                         headers: Map<String, String> = emptyMap()): JSONObject {
        val proxy = aiProxy()
        val conn = (if (proxy != null) URL(urlStr).openConnection(proxy)
                    else URL(urlStr).openConnection()) as HttpURLConnection
        if (proxy != null)
            BridgeBus.log("ИИ: ${URL(urlStr).host} → прокси ${proxy.type()} ${proxy.address()}")
        try {
            conn.apply {
                requestMethod = "POST"
                connectTimeout = 10_000
                readTimeout = 60_000
                setRequestProperty("Content-Type", "application/json; charset=utf-8")
                headers.forEach { (k, v) -> setRequestProperty(k, v) }
                doOutput = true
                outputStream.use { it.write(json.toString().toByteArray(Charsets.UTF_8)) }
            }
            val code = conn.responseCode
            val stream = if (code in 200..299) conn.inputStream else conn.errorStream
            val text = stream?.bufferedReader(Charsets.UTF_8)?.use { it.readText() } ?: ""
            conn.disconnect()
            if (code !in 200..299) {
                val msg = try {
                    JSONObject(text).optJSONObject("error")?.optString("message") ?: text.take(200)
                } catch (_: Exception) { text.take(200) }
                throw IOException("API HTTP $code: $msg")
            }
            return JSONObject(text)
        } catch (e: IOException) {
            // Прямое подключение режется (белый список/VPN) — подсказываем про прокси
            val noNet = e is ConnectException || e is UnknownHostException ||
                e is SocketTimeoutException || e is NoRouteToHostException ||
                e is SocketException
            if (proxy == null && noNet)
                throw IOException("${e.message} — ИИ идёт напрямую; укажите " +
                    "«Прокси для ИИ» в ⚙ (порт Happ)")
            throw e
        }
    }

    private fun logUsedModel(resp: JSONObject) {
        val m = resp.optString("model", "")
        if (m.isNotEmpty()) BridgeBus.log("ИИ: модель $m")
    }

    private fun openAiText(key: String, text: String,
                           url: String, model: String): String {
        val body = JSONObject()
            .put("model", model)
            .put("max_tokens", 1200)
            .put("messages", JSONArray().put(
                JSONObject().put("role", "user").put("content", text)))
        val resp = postJson(url, body,
            mapOf("Authorization" to "Bearer $key"))
        logUsedModel(resp)
        return extractContent(resp)
    }

    private fun geminiText(key: String, text: String, model: String): String {
        val body = JSONObject().put("contents", JSONArray().put(
            JSONObject().put("role", "user")
                .put("parts", JSONArray().put(JSONObject().put("text", text)))))
        val resp = postJson(
            "https://generativelanguage.googleapis.com/v1beta/models/" +
            "$model:generateContent?key=$key", body)
        return extractGemini(resp)
    }

    private fun callOpenAi(key: String, prompt: String, b64: String,
                           url: String, model: String): String {
        val content = JSONArray()
            .put(JSONObject().put("type", "text").put("text", prompt))
            .put(JSONObject().put("type", "image_url")
                .put("image_url", JSONObject().put("url", "data:image/jpeg;base64,$b64")))
        val body = JSONObject()
            .put("model", model)
            .put("max_tokens", 1200)
            .put("messages", JSONArray().put(
                JSONObject().put("role", "user").put("content", content)))
        val resp = postJson(url, body,
            mapOf("Authorization" to "Bearer $key"))
        logUsedModel(resp)
        return extractContent(resp)
    }

    private fun callGemini(key: String, prompt: String, b64: String,
                           model: String): String {
        val parts = JSONArray()
            .put(JSONObject().put("text", prompt))
            .put(JSONObject().put("inline_data",
                JSONObject().put("mime_type", "image/jpeg").put("data", b64)))
        val body = JSONObject().put("contents", JSONArray().put(
            JSONObject().put("role", "user").put("parts", parts)))
        val resp = postJson(
            "https://generativelanguage.googleapis.com/v1beta/models/" +
            "$model:generateContent?key=$key", body)
        return extractGemini(resp)
    }
}
