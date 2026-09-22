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
import android.view.Gravity
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
import java.util.concurrent.Executors
import kotlin.concurrent.thread

/*
 * CamLink — мост «устройство CheatSheet2 → телефон → ИИ → ответ обратно на устройство».
 *
 * Подключение: WifiNetworkSpecifier (Android 10+) подключается к SoftAP «CSCAM»
 * как к ЛОКАЛЬНОЙ сети — телефон остаётся на мобильном интернете для запроса к ИИ.
 * Переброс фото/ответа: HTTP на 192.168.4.1 (сокет привязан к сети ESP).
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
    private lateinit var btnAsk: Button
    private val mainHandler = Handler(Looper.getMainLooper())
    private val exec = Executors.newSingleThreadExecutor()

    @Volatile private var espNetwork: Network? = null
    private var netCallback: ConnectivityManager.NetworkCallback? = null

    private val prefs by lazy { getSharedPreferences("camlink", Context.MODE_PRIVATE) }

    // ---------- Жизненный цикл ----------

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        buildUi()
        connectEsp()
    }

    override fun onDestroy() {
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
            setPadding(dp(16), dp(16), dp(16), dp(16))
        }

        root.addView(TextView(this).apply {
            text = "🔗 CamLink — мост к ИИ"
            textSize = 22f
            setPadding(0, 0, 0, dp(4))
        })

        tvStatus = TextView(this).apply {
            text = "Подключение к CSCAM…"
            textSize = 14f
            setPadding(0, 0, 0, dp(12))
        }
        root.addView(tvStatus)

        btnAsk = Button(this).apply {
            text = "Спросить ИИ о последнем фото"
            setOnClickListener { askAi() }
        }
        root.addView(btnAsk, LinearLayout.LayoutParams(
            LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT))

        val row = LinearLayout(this).apply { orientation = LinearLayout.HORIZONTAL }
        row.addView(Button(this).apply {
            text = "⚙ Настройки"
            setOnClickListener { openSettings() }
        })
        row.addView(Button(this).apply {
            text = "↻ Связь"
            setOnClickListener {
                reconnectEsp()
            }
        })
        root.addView(row, LinearLayout.LayoutParams(
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
            log("! Включите WiFi и вернитесь в приложение (кнопка «Связь»)")
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
                status("Подключено к устройству — можно спрашивать ИИ")
            }

            override fun onLost(network: Network) {
                if (espNetwork == network) espNetwork = null
                log("✖ Связь с устройством потеряна (нажмите «Связь»)")
                status("Нет связи с устройством")
            }

            override fun onUnavailable() {
                status("Не удалось подключиться — нажмите «Связь»")
                log("✖ Диалог подключения не подтверждён")
            }
        }
        netCallback = cb
        cm.requestNetwork(request, cb, 30_000)
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
        val conn = url.openConnection() as HttpURLConnection
        conn.apply {
            connectTimeout = 5_000
            readTimeout = if (path == "/api/photo") 20_000 else 8_000
            requestMethod = method
            socketFactory = net.socketFactory      // ← трафик только в сеть ESP
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

    private fun espGetJson(path: String): JSONObject {
        val (code, data) = espRequest(path)
        if (code != 200) throw IOException("HTTP $code с устройства")
        return JSONObject(String(data, Charsets.UTF_8))
    }

    // ---------- Основной сценарий ----------

    private fun askAi() {
        if (espNetwork == null) {
            status("Нет связи с устройством — нажмите «Связь»")
            return
        }
        btnAsk.isEnabled = false
        exec.execute {
            try {
                status("Проверяю фото на устройстве…")
                val st = espGetJson("/api/state")
                val state = st.getInt("state")
                val id = st.getInt("id")
                when (state) {
                    0 -> {
                        status("Фото нет — снимите его на устройстве")
                        log("Очередь пуста (state=0)")
                        return@execute
                    }
                    3 -> log("Есть прошлый ответ (#${st.getInt("seq")}), беру свежее фото")
                }
                if (id == 0) return@execute

                status("Качаю фото с устройства…")
                val (pcode, photo) = espRequest("/api/photo")
                if (pcode != 200 || photo.isEmpty()) throw IOException("фото недоступно (HTTP $pcode)")
                log("Фото получено: ${photo.size / 1024} КБ")

                status("Готовлю изображение…")
                val resized = resize(photo, 1568, 85)
                log("Сжато для ИИ: ${resized.size / 1024} КБ")

                status("ИИ думает…")
                val answer = callAi(resized).trim()
                log("ИИ ответил (${answer.length} симв.)")

                status("Отправляю ответ на устройство…")
                val answerBytes = answer.take(ANSWER_MAX).toByteArray(Charsets.UTF_8)
                val (acode, adata) = espRequest(
                    "/api/answer", "POST",
                    mapOf("X-Id" to id.toString(), "Content-Type" to "text/plain; charset=utf-8"),
                    answerBytes
                )
                if (acode != 200) throw IOException("ответ не принят (HTTP $acode)")

                status("✅ Готово! Ответ показан на экране устройства")
                log("Ответ доставлен (для фото #$id)")
            } catch (e: Exception) {
                status("Ошибка: ${e.message}")
                log("ОШИБКА: ${e.message}")
            } finally {
                mainHandler.post { btnAsk.isEnabled = true }
            }
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

    private fun callAi(jpeg: ByteArray): String {
        val key = prefs.getString("key", "")!!
        if (key.isBlank()) throw IOException("нет API-ключа (Настройки)")
        val provider = prefs.getString("provider", "gemini")!!
        val prompt = prefs.getString("prompt", DEFAULT_PROMPT)!!
        val b64 = Base64.encodeToString(jpeg, Base64.NO_WRAP)
        return if (provider == "openai") callOpenAi(key, prompt, b64)
               else callGemini(key, prompt, b64)
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
            text = "Промпт"; setPadding(0, dp(12), 0, 0)
        })
        val promptEdit = EditText(this).apply {
            hint = "Что спрашивать у ИИ"
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
