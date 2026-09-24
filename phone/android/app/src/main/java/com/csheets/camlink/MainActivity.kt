package com.csheets.camlink

import android.app.Activity
import android.app.AlertDialog
import android.content.Context
import android.content.Intent
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.net.ConnectivityManager
import android.net.Network
import android.net.NetworkCapabilities
import android.net.NetworkRequest
import android.net.wifi.WifiManager
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.provider.Settings
import android.text.InputType
import android.util.Base64
import android.util.TypedValue
import android.view.View
import android.widget.Button
import android.widget.EditText
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.Spinner
import android.widget.ArrayAdapter
import android.widget.TextView
import org.json.JSONArray
import org.json.JSONObject
import java.io.ByteArrayOutputStream
import java.io.IOException
import java.net.HttpURLConnection
import java.net.URL
import java.net.URLEncoder
import java.util.concurrent.Executors
import kotlin.concurrent.thread

/*
 * CamLink — посредник «телефон ⇄ устройство CheatSheet2 ⇄ ИИ».
 *
 * Сам чат живёт на УСТРОЙСТВЕ (экран «ИИ»). Телефон только:
 *   1) принимает текст с клавиатуры и фото (устройство / галерея),
 *   2) немедленно шлёт вопрос на устройство (POST /api/question),
 *   3) спрашивает ИИ через мобильный интернет,
 *   4) доставляет ответ (POST /api/answer) — он дописывается в вопрос.
 *
 * Подключение: WifiNetworkSpecifier (Android 10+) — локальная сеть к SoftAP
 * «CSCAM», мобильный интернет остаётся для API ИИ.
 */
class MainActivity : Activity() {

    private companion object {
        const val ESP_IP = "192.168.4.1"
        const val ANSWER_MAX = 3800           // буфер ответа на устройстве — 4096
        const val DEFAULT_PROMPT =
            "Ты помогаешь на контрольной. Распознай вопрос на фото и дай краткий " +
            "правильный ответ: если тест с вариантами — укажи букву и одно предложение " +
            "почему; если развёрнутый вопрос — 2-3 предложения. Отвечай только по делу."
    }

    private lateinit var cm: ConnectivityManager
    private lateinit var tvStatus: TextView
    private lateinit var tvLog: TextView
    private lateinit var etInput: EditText
    private lateinit var btnSend: Button
    private lateinit var btnQuick: Button
    private val mainHandler = Handler(Looper.getMainLooper())
    private val exec = Executors.newSingleThreadExecutor()

    @Volatile private var espNetwork: Network? = null
    private var netCallback: ConnectivityManager.NetworkCallback? = null
    @Volatile private var pendingPhoto: ByteArray? = null
    private var lastSendSeen = 0L        // последний seen send с устройства

    private val prefs by lazy { getSharedPreferences("camlink", Context.MODE_PRIVATE) }

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

    // ---------- Жизненный цикл ----------

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        buildUi()
        connectEsp()
        mainHandler.postDelayed(pollRunnable, 2000)
    }

    override fun onDestroy() {
        mainHandler.removeCallbacks(pollRunnable)
        netCallback?.let { cm.unregisterNetworkCallback(it) }
        netCallback = null
        exec.shutdownNow()
        super.onDestroy()
    }

    // ---------- Интерфейс (программный, без XML) ----------

    private fun dp(v: Int): Int =
        TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, v.toFloat(),
            resources.displayMetrics).toInt()

    private fun buildUi() {
        val root = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dp(12), dp(10), dp(12), dp(10))
        }

        root.addView(TextView(this).apply {
            text = "🔗 CamLink — мост к ИИ"
            textSize = 18f
            setPadding(0, 0, 0, dp(2))
        })

        tvStatus = TextView(this).apply {
            text = "Подключение к CSCAM…"
            textSize = 13f
            setTextColor(0xFF8FA3BF.toInt())
            setPadding(0, 0, 0, dp(6))
        }
        root.addView(tvStatus)

        // Клавиатура телефона → вопрос на устройство → ответ ИИ обратно
        val inputRow = LinearLayout(this).apply { orientation = LinearLayout.HORIZONTAL }
        val btnAttach = Button(this).apply {
            text = "📎"
            setOnClickListener { attachMenu() }
        }
        inputRow.addView(btnAttach, LinearLayout.LayoutParams(
            LinearLayout.LayoutParams.WRAP_CONTENT, LinearLayout.LayoutParams.WRAP_CONTENT, 0f))
        etInput = EditText(this).apply {
            hint = "Вопрос для ИИ…"
            maxLines = 3
            inputType = InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_FLAG_MULTI_LINE
        }
        inputRow.addView(etInput, LinearLayout.LayoutParams(
            0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f))
        btnSend = Button(this).apply {
            text = "→"
            setOnClickListener { send() }
        }
        inputRow.addView(btnSend, LinearLayout.LayoutParams(
            LinearLayout.LayoutParams.WRAP_CONTENT, LinearLayout.LayoutParams.WRAP_CONTENT, 0f))
        root.addView(inputRow, LinearLayout.LayoutParams(
            LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT))

        val actionRow = LinearLayout(this).apply { orientation = LinearLayout.HORIZONTAL }
        btnQuick = Button(this).apply {
            text = "📷 Спросить о фото"
            textSize = 13f
            isEnabled = false
            setOnClickListener { quickAsk() }
        }
        actionRow.addView(btnQuick, LinearLayout.LayoutParams(
            0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f))
        actionRow.addView(Button(this).apply {
            text = "⚙"
            setOnClickListener { openSettings() }
        }, LinearLayout.LayoutParams(LinearLayout.LayoutParams.WRAP_CONTENT,
            LinearLayout.LayoutParams.WRAP_CONTENT, 0f))
        actionRow.addView(Button(this).apply {
            text = "↻"
            setOnClickListener { reconnectEsp() }
        }, LinearLayout.LayoutParams(LinearLayout.LayoutParams.WRAP_CONTENT,
            LinearLayout.LayoutParams.WRAP_CONTENT, 0f))
        root.addView(actionRow, LinearLayout.LayoutParams(
            LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT))

        val scroll = ScrollView(this).apply {
            layoutParams = LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, 0, 1f)
        }
        tvLog = TextView(this).apply {
            textSize = 13f
            typeface = android.graphics.Typeface.MONOSPACE
        }
        scroll.addView(tvLog)
        root.addView(scroll)

        setContentView(root)
    }

    private fun log(msg: String) {
        mainHandler.post {
            tvLog.append(msg + "\n")
            (tvLog.parent as? ScrollView)?.post {
                (tvLog.parent as ScrollView).fullScroll(View.FOCUS_DOWN)
            }
        }
    }

    private fun status(msg: String) {
        mainHandler.post { tvStatus.text = msg }
    }

    // ---------- Подключение к SoftAP устройства ----------

    @Suppress("DEPRECATION")
    private fun connectEsp() {
        if (netCallback != null) return
        cm = getSystemService(Context.CONNECTIVITY_SERVICE) as ConnectivityManager

        val wifi = applicationContext.getSystemService(Context.WIFI_SERVICE) as WifiManager
        if (!wifi.isWifiEnabled) {
            status("WiFi выключен — включаю настройки")
            startActivity(Intent(Settings.ACTION_WIFI_SETTINGS))
            log("! Включите WiFi и вернитесь в приложение (кнопка ↻)")
        }

        val ssid = prefs.getString("ssid", "CSCAM")!!
        val pass = prefs.getString("pass", "cscam1234")!!
        log("Запрос подключения к «$ssid» (системный диалог — «Подключиться»)…")

        val specifier = try {
            android.net.wifi.WifiNetworkSpecifier.Builder()
                .setSsid(ssid)
                .setWpa2Passphrase(pass)
                .build()
        } catch (e: IllegalArgumentException) {
            status("Проверьте пароль в Настройках")
            log("Ошибка сети: ${e.message}")
            return
        }

        val request = NetworkRequest.Builder()
            .addTransportType(NetworkCapabilities.TRANSPORT_WIFI)
            .setNetworkSpecifier(specifier)
            .build()

        val cb = object : ConnectivityManager.NetworkCallback() {
            override fun onAvailable(network: Network) {
                espNetwork = network
                log("✔ Подключено к устройству ($ssid)")
                status("Подключено — чат на устройстве")
                mainHandler.post { btnQuick.isEnabled = true }
            }

            override fun onLost(network: Network) {
                if (espNetwork == network) espNetwork = null
                log("✖ Связь с устройством потеряна (нажмите ↻)")
                status("Нет связи с устройством")
                mainHandler.post { btnQuick.isEnabled = false }
            }

            override fun onUnavailable() {
                status("Не удалось подключиться — нажмите ↻")
                log("✖ Диалог подключения не подтверждён")
            }
        }
        netCallback = cb
        try {
            cm.requestNetwork(request, cb, 30_000)
        } catch (e: SecurityException) {
            // старые сборки падали тут — нет CHANGE_NETWORK_STATE
            status("Нет разрешения сети: ${e.message}")
            log("requestNetwork: ${e.message}")
            netCallback = null
            return
        }
        status("Подключение к устройству…")
    }

    private fun reconnectEsp() {
        netCallback?.let {
            cm.unregisterNetworkCallback(it)
            netCallback = null
        }
        espNetwork = null
        connectEsp()
    }

    // ---------- HTTP к устройству (сокет привязан к сети ESP) ----------

    private fun espRequest(path: String, method: String = "GET",
                           headers: Map<String, String>? = null,
                           body: ByteArray? = null): Pair<Int, ByteArray> {
        val net = espNetwork ?: throw IOException("нет связи с устройством")
        val url = URL("http://$ESP_IP$path")
        // Network.openConnection — трафик идёт только по сети ESP
        val conn = net.openConnection(url) as HttpURLConnection
        conn.apply {
            connectTimeout = 5_000
            readTimeout = if (path == "/api/photo") 20_000 else 8_000
            requestMethod = method
            headers?.forEach { (k, v) -> setRequestProperty(k, v) }
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
        return code to data
    }

    // ---------- Отправка ----------

    // Текст (± фото) → вопрос сразу на устройство → ИИ → ответ на устройство
    private fun send() {
        val text = etInput.text.toString().trim()
        val photo = pendingPhoto
        if (text.isEmpty() && photo == null) return
        etInput.setText("")
        if (text.isNotEmpty()) log("→ $text")

        btnSend.isEnabled = false
        val headers = mutableMapOf("X-Id" to "chat")
        if (text.isNotEmpty())
            headers["X-Question"] = URLEncoder.encode(text, "UTF-8")

        // 1) вопрос мгновенно в чат устройства (пузырь «…»)
        if (text.isNotEmpty()) {
            exec.execute {
                try {
                    val (code, _) = espRequest("/api/question", "POST",
                        mapOf("X-Question" to headers["X-Question"]!!), ByteArray(0))
                    if (code != 200) log("устройство: вопрос не принят (HTTP $code)")
                } catch (e: Exception) {
                    log("вопрос не доставлен: ${e.message}")
                }
            }
        }

        // 2) ИИ через мобильный интернет, 3) ответ обратно
        exec.execute {
            try {
                status("ИИ думает…")
                val answer = callAi(photo, text).trim()
                log("← $answer")
                pendingPhoto = null

                // ответ (с дублем вопроса — если шаг 1 не прошл, он станет новым обменом)
                val (code, _) = espRequest("/api/answer", "POST",
                    headers + ("Content-Type" to "text/plain; charset=utf-8"),
                    answer.take(ANSWER_MAX).toByteArray(Charsets.UTF_8))
                if (code == 200) status("Готово — ответ на экране устройства")
                else { log("устройство: ответ не принят (HTTP $code)"); status("ИИ ответил, устройство недоступно") }
            } catch (e: Exception) {
                log("ОШИБКА: ${e.message}")
                status("Ошибка: ${e.message}")
                // сорванный вопрос не должен остаться «…» навсегда
                try {
                    if (text.isNotEmpty())
                        espRequest("/api/answer", "POST",
                            headers + ("Content-Type" to "text/plain; charset=utf-8"),
                            "⚠ ${e.message}".toByteArray(Charsets.UTF_8))
                } catch (_: Exception) {}
            } finally {
                mainHandler.post { btnSend.isEnabled = true }
            }
        }
    }

    // «Отправить» на устройстве → телефон сам: фото → вопрос → ИИ → ответ
    private fun pollSend() {
        try {
            val (c, d) = espRequest("/api/state")
            if (c != 200) return
            val send = JSONObject(String(d, Charsets.UTF_8)).optLong("send", 0)
            if (send > lastSendSeen) {
                if (exec.isShutdown) return
                lastSendSeen = send
                log("Устройство просит отправить фото (#$send)")
                runPhotoAsk()
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
            status("Фото нет — снимите его на устройстве")
            return
        }

        status("Качаю фото с устройства…")
        val (pcode, photo) = espRequest("/api/photo")
        if (pcode != 200 || photo.isEmpty())
            throw IOException("фото недоступно (HTTP $pcode)")
        log("Фото: ${photo.size / 1024} КБ")

        val question = detectPendingQuestion()
        if (question.isNotEmpty()) log("→ $question")

        status("ИИ думает…")
        val answer = callAi(photo, question).trim()
        log("← $answer")

        val headers = mutableMapOf(
            "X-Id" to "auto",
            "Content-Type" to "text/plain; charset=utf-8")
        if (question.isNotEmpty())
            headers["X-Question"] = URLEncoder.encode(question, "UTF-8")
        val (code, _) = espRequest("/api/answer", "POST", headers,
            answer.take(ANSWER_MAX).toByteArray(Charsets.UTF_8))
        if (code == 200) status("Готово — ответ на экране устройства")
        else status("ответ не принят (HTTP $code)")
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

    private fun quickAsk() {
        btnQuick.isEnabled = false
        exec.execute {
            try {
                runPhotoAsk()
            } catch (e: Exception) {
                log("ОШИБКА: ${e.message}")
                status("Ошибка: ${e.message}")
            } finally {
                mainHandler.post { btnQuick.isEnabled = espNetwork != null }
            }
        }
    }

    // ---------- Прикрепление фото ----------

    private fun attachMenu() {
        AlertDialog.Builder(this)
            .setTitle("Прикрепить фото")
            .setItems(arrayOf("Фото с устройства", "Фото из галереи")) { _, which ->
                if (which == 0) attachFromDevice() else pickFromGallery()
            }
            .setNegativeButton("Отмена", null)
            .show()
    }

    private fun attachFromDevice() {
        exec.execute {
            try {
                val (pcode, photo) = espRequest("/api/photo")
                if (pcode != 200 || photo.isEmpty())
                    throw IOException("нет фото на устройстве (HTTP $pcode)")
                pendingPhoto = photo
                log("📎 Фото с устройства прикреплено (${photo.size / 1024} КБ)")
                status("Фото прикреплено — введите вопрос")
            } catch (e: Exception) {
                status(e.message ?: "Ошибка")
            }
        }
    }

    private fun pickFromGallery() {
        val intent = Intent(Intent.ACTION_GET_CONTENT)
            .addCategory(Intent.CATEGORY_OPENABLE)
            .setType("image/*")
        startActivityForResult(intent, 100)
    }

    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        super.onActivityResult(requestCode, resultCode, data)
        if (requestCode != 100 || resultCode != RESULT_OK) return
        val uri = data?.data ?: return
        thread {
            try {
                val bytes = contentResolver.openInputStream(uri)?.use { it.readBytes() }
                    ?: throw IOException("не удалось прочитать фото")
                pendingPhoto = bytes
                log("📎 Фото из галереи прикреплено (${bytes.size / 1024} КБ)")
                status("Фото прикреплено — введите вопрос")
            } catch (e: Exception) {
                status(e.message ?: "Ошибка")
            }
        }.start()
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

        if (photo == null) {   // текстовый вопрос
            return if (provider == "openai") openAiText(key, userText)
                   else geminiText(key, userText)
        }

        // Фото: промпт-инструкция (+ вопрос пользователя, если был набран)
        val prompt = prefs.getString("prompt", DEFAULT_PROMPT)!!
        val visionPrompt =
            if (userText.isEmpty()) prompt else "$prompt\n\nВопрос: $userText"
        val resized = resize(photo, 1568, 85)
        val b64 = Base64.encodeToString(resized, Base64.NO_WRAP)
        return if (provider == "openai") callOpenAi(key, visionPrompt, b64)
               else callGemini(key, visionPrompt, b64)
    }

    private fun postJson(urlStr: String, json: JSONObject,
                         headers: Map<String, String> = emptyMap()): JSONObject {
        val conn = URL(urlStr).openConnection() as HttpURLConnection
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
    }

    private fun openAiText(key: String, text: String): String {
        val body = JSONObject()
            .put("model", "gpt-4o-mini")
            .put("max_tokens", 1200)
            .put("messages", JSONArray().put(
                JSONObject().put("role", "user").put("content", text)))
        val resp = postJson("https://api.openai.com/v1/chat/completions", body,
            mapOf("Authorization" to "Bearer $key"))
        return resp.getJSONArray("choices")
            .getJSONObject(0).getJSONObject("message").getString("content")
    }

    private fun geminiText(key: String, text: String): String {
        val body = JSONObject().put("contents", JSONArray().put(
            JSONObject().put("role", "user")
                .put("parts", JSONArray().put(JSONObject().put("text", text)))))
        val resp = postJson(
            "https://generativelanguage.googleapis.com/v1beta/models/" +
            "gemini-2.0-flash:generateContent?key=$key", body)
        val cand = resp.getJSONArray("candidates").getJSONObject(0)
        return cand.getJSONObject("content").getJSONArray("parts")
            .getJSONObject(0).getString("text")
    }

    private fun callOpenAi(key: String, prompt: String, b64: String): String {
        val content = JSONArray()
            .put(JSONObject().put("type", "text").put("text", prompt))
            .put(JSONObject().put("type", "image_url")
                .put("image_url", JSONObject().put("url", "data:image/jpeg;base64,$b64")))
        val body = JSONObject()
            .put("model", "gpt-4o-mini")
            .put("max_tokens", 1200)
            .put("messages", JSONArray().put(
                JSONObject().put("role", "user").put("content", content)))
        val resp = postJson("https://api.openai.com/v1/chat/completions", body,
            mapOf("Authorization" to "Bearer $key"))
        return resp.getJSONArray("choices")
            .getJSONObject(0).getJSONObject("message").getString("content")
    }

    private fun callGemini(key: String, prompt: String, b64: String): String {
        val parts = JSONArray()
            .put(JSONObject().put("text", prompt))
            .put(JSONObject().put("inline_data",
                JSONObject().put("mime_type", "image/jpeg").put("data", b64)))
        val body = JSONObject().put("contents", JSONArray().put(
            JSONObject().put("role", "user").put("parts", parts)))
        val resp = postJson(
            "https://generativelanguage.googleapis.com/v1beta/models/" +
            "gemini-2.0-flash:generateContent?key=$key", body)
        val cand = resp.getJSONArray("candidates").getJSONObject(0)
        return cand.getJSONObject("content").getJSONArray("parts")
            .getJSONObject(0).getString("text")
    }

    // ---------- Настройки ----------

    private fun openSettings() {
        val layout = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dp(24), dp(8), dp(24), 0)
        }

        layout.addView(TextView(this).apply { text = "Провайдер ИИ" })
        val spinner = Spinner(this).apply {
            adapter = ArrayAdapter(context,
                android.R.layout.simple_spinner_dropdown_item,
                listOf("Google Gemini Flash", "OpenAI GPT-4o-mini"))
            setSelection(if (prefs.getString("provider", "gemini") == "openai") 1 else 0)
        }
        layout.addView(spinner)

        val keyEdit = EditText(this).apply {
            hint = "API-ключ"
            inputType = InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_VARIATION_PASSWORD
            setText(prefs.getString("key", ""))
        }
        layout.addView(keyEdit)

        layout.addView(TextView(this).apply {
            text = "Промпт (для фото)"
            setPadding(0, dp(12), 0, 0)
        })
        val promptEdit = EditText(this).apply {
            hint = "Что спрашивать у ИИ по фото"
            inputType = InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_FLAG_MULTI_LINE
            minLines = 3
            setText(prefs.getString("prompt", DEFAULT_PROMPT))
        }
        layout.addView(promptEdit)

        layout.addView(TextView(this).apply {
            text = "Сеть устройства (SSID / пароль)"; setPadding(0, dp(12), 0, 0)
        })
        val ssidEdit = EditText(this).apply {
            hint = "SSID"
            setText(prefs.getString("ssid", "CSCAM"))
        }
        layout.addView(ssidEdit)
        val passEdit = EditText(this).apply {
            hint = "Пароль"
            inputType = InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_VARIATION_PASSWORD
            setText(prefs.getString("pass", "cscam1234"))
        }
        layout.addView(passEdit)

        val scroll = ScrollView(this).apply { addView(layout) }

        AlertDialog.Builder(this)
            .setTitle("Настройки CamLink")
            .setView(scroll)
            .setPositiveButton("Сохранить") { _, _ ->
                prefs.edit()
                    .putString("provider", if (spinner.selectedItemPosition == 1) "openai" else "gemini")
                    .putString("key", keyEdit.text.toString().trim())
                    .putString("prompt", promptEdit.text.toString().trim())
                    .putString("ssid", ssidEdit.text.toString().trim())
                    .putString("pass", passEdit.text.toString().trim())
                    .apply()
                log("Настройки сохранены")
            }
            .setNegativeButton("Отмена", null)
            .show()
    }
}
