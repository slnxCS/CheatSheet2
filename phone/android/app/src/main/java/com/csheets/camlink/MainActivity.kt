package com.csheets.camlink

import android.app.Activity
import android.app.AlertDialog
import android.content.ContentValues
import android.content.Context
import android.content.Intent
import android.net.Uri
import android.net.wifi.WifiManager
import android.os.Bundle
import android.os.Environment
import android.os.Handler
import android.os.Looper
import android.os.PowerManager
import android.provider.OpenableColumns
import android.provider.Settings
import android.provider.MediaStore
import android.text.InputType
import android.util.TypedValue
import android.view.View
import android.widget.Button
import android.widget.EditText
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.Spinner
import android.widget.ArrayAdapter
import android.widget.ListView
import android.widget.TextView
import org.json.JSONArray
import java.io.IOException
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
 *
 * Вся рабочая логика (сеть, опрос /api/state, вызовы ИИ) живёт в
 * BridgeService — foreground-сервисе, который переживает уход приложения
 * в фон и погашение экрана. Activity здесь только рисует и подписывается
 * на журнал через BridgeBus.
 */
class MainActivity : Activity() {

    private lateinit var tvStatus: TextView
    private lateinit var tvLog: TextView
    private lateinit var etInput: EditText
    private lateinit var btnSend: Button
    private lateinit var btnQuick: Button
    private val mainHandler = Handler(Looper.getMainLooper())
    private val exec = Executors.newSingleThreadExecutor()

    private val prefs by lazy { getSharedPreferences("camlink", Context.MODE_PRIVATE) }

    // ---------- Жизненный цикл ----------

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        buildUi()
        attachBus()
        maybePromptWifi()
        maybeAskBattery()
        startForegroundService(Intent(this, BridgeService::class.java))
        // Если прошлый старт сервиса не смог показать системный диалог
        // подключения (приложение было в фоне) — повторяем, пока мы на виду.
        BridgeService.instance?.ensureConnected()
    }

    override fun onDestroy() {
        BridgeBus.onLog = null
        BridgeBus.onStatus = null
        BridgeBus.onLink = null
        BridgeBus.onSendDone = null
        BridgeBus.onAskDone = null
        exec.shutdownNow()
        super.onDestroy()
    }

    // Подписка на журнал моста: реплей истории + живые обновления
    private fun attachBus() {
        BridgeBus.onLog = { line -> mainHandler.post { appendLog(line) } }
        BridgeBus.onStatus = { s -> mainHandler.post { tvStatus.text = s } }
        BridgeBus.onLink = { linked -> mainHandler.post { btnQuick.isEnabled = linked } }
        BridgeBus.onSendDone = { mainHandler.post { btnSend.isEnabled = true } }
        BridgeBus.onAskDone = { mainHandler.post { btnQuick.isEnabled = BridgeBus.linked } }
        BridgeBus.replay().forEach { appendLog(it) }
        if (BridgeBus.lastStatus.isNotEmpty()) tvStatus.text = BridgeBus.lastStatus
        btnQuick.isEnabled = BridgeBus.linked
    }

    private fun appendLog(msg: String) {
        tvLog.append(msg + "\n")
        (tvLog.parent as? ScrollView)?.post {
            (tvLog.parent as ScrollView).fullScroll(View.FOCUS_DOWN)
        }
    }

    @Suppress("DEPRECATION")
    private fun maybePromptWifi() {
        val wifi = applicationContext.getSystemService(Context.WIFI_SERVICE) as WifiManager
        if (!wifi.isWifiEnabled) {
            status("WiFi выключен — включаю настройки")
            try { startActivity(Intent(Settings.ACTION_WIFI_SETTINGS)) } catch (_: Exception) {}
            log("! Включите WiFi и вернитесь в приложение (кнопка ↻)")
        }
    }

    // Android режет сеть фоновым приложениям (Doze) — просим исключение,
    // иначе с выключенным экраном связь с устройством будет обрываться.
    private fun maybeAskBattery() {
        if (prefs.getBoolean("battAsked", false)) return
        val pm = getSystemService(Context.POWER_SERVICE) as PowerManager
        if (pm.isIgnoringBatteryOptimizations(packageName)) return
        prefs.edit().putBoolean("battAsked", true).apply()
        log("! Разрешите CamLink работать без ограничений батареи —")
        log("  иначе Android усыпит связь в фоне (экран погас)")
        try {
            startActivity(Intent(Settings.ACTION_REQUEST_IGNORE_BATTERY_OPTIMIZATIONS,
                Uri.parse("package:$packageName")))
        } catch (e: Exception) {
            log("  не удалось: ${e.message} — вручную: Настройки → Батарея → Исключения")
        }
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
            text = "📁"
            setOnClickListener {
                startActivity(Intent(this@MainActivity, FileActivity::class.java))
            }
        }, LinearLayout.LayoutParams(LinearLayout.LayoutParams.WRAP_CONTENT,
            LinearLayout.LayoutParams.WRAP_CONTENT, 0f))
        actionRow.addView(Button(this).apply {
            text = "↻"
            setOnClickListener { BridgeService.reconnect() }
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

    private fun log(msg: String) = BridgeBus.log(msg)

    private fun status(msg: String) = BridgeBus.status(msg)

    // ---------- Отправка (работа выполняется в BridgeService) ----------

    // Текст (± фото) → вопрос сразу на устройство → ИИ → ответ на устройство
    private fun send() {
        val text = etInput.text.toString().trim()
        val photo = pendingPhoto
        if (text.isEmpty() && photo == null) return
        etInput.setText("")
        btnSend.isEnabled = false
        if (!BridgeService.submitQuestion(text, photo)) {
            log("! Мост ещё не запущен — попробуйте через секунду")
            btnSend.isEnabled = true
        }
    }

    private fun quickAsk() {
        btnQuick.isEnabled = false
        if (!BridgeService.photoAsk()) {
            log("! Мост ещё не запущен — попробуйте через секунду")
            btnQuick.isEnabled = BridgeBus.linked
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
                listOf("Google Gemini Flash (бесплатный)",
                       "OpenAI GPT-4o-mini",
                       "OpenRouter (бесплатные модели)"))
            setSelection(when (prefs.getString("provider", "gemini")) {
                "openai" -> 1
                "openrouter" -> 2
                else -> 0
            })
        }
        layout.addView(spinner)

        val keyEdit = EditText(this).apply {
            hint = "API-ключ"
            inputType = InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_VARIATION_PASSWORD
            setText(prefs.getString("key", ""))
        }
        layout.addView(keyEdit)

        layout.addView(TextView(this).apply {
            text = "Модель"; setPadding(0, dp(12), 0, 0)
        })
        val modelEdit = EditText(this).apply {
            hint = "пусто = по умолчанию (gemini-3.6-flash / " +
                   "gpt-4o-mini / openrouter/free)"
            setText(prefs.getString("model", ""))
        }
        layout.addView(modelEdit)

        layout.addView(TextView(this).apply {
            text = "Прокси для ИИ (порт Happ при активном VPN)"
            setPadding(0, dp(12), 0, 0)
        })
        val proxyEdit = EditText(this).apply {
            hint = "127.0.0.1:10809 или socks5://127.0.0.1:10808 — пусто = напрямую"
            setText(prefs.getString("ai_proxy", ""))
        }
        layout.addView(proxyEdit)

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
                    .putString("provider", when (spinner.selectedItemPosition) {
                        1 -> "openai"
                        2 -> "openrouter"
                        else -> "gemini"
                    })
                    .putString("key", keyEdit.text.toString().trim())
                    .putString("model", modelEdit.text.toString().trim())
                    .putString("ai_proxy", proxyEdit.text.toString().trim())
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

// ---------- Файлы устройства (📁): флэш LittleFS и SD-карта ----------
// API: GET /api/fs — список, GET /api/file — скачать,
// POST /api/file — загрузить (потоково), DELETE — удалить.
// Виртуальные пути устройства: "/flash/..." → LittleFS, "/sd/..." → SD.
private data class FsEntry(val name: String, val dir: Boolean, val size: Long)

class FileActivity : Activity() {
    private lateinit var tvPath: TextView
    private lateinit var tvStatus: TextView
    private lateinit var list: ListView
    private val exec = Executors.newSingleThreadExecutor()
    private var cwd = "/"
    private var entries: List<FsEntry> = emptyList()
    private var selected = -1

    private fun dp(v: Int): Int =
        TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, v.toFloat(),
            resources.displayMetrics).toInt()

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        buildUi()
        load()
    }

    override fun onDestroy() {
        exec.shutdownNow()
        super.onDestroy()
    }

    private fun buildUi() {
        val root = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dp(12), dp(8), dp(12), dp(8))
        }

        val head = LinearLayout(this).apply { orientation = LinearLayout.HORIZONTAL }
        tvPath = TextView(this).apply {
            textSize = 15f
            gravity = android.view.Gravity.CENTER_VERTICAL
        }
        head.addView(tvPath, LinearLayout.LayoutParams(0,
            LinearLayout.LayoutParams.WRAP_CONTENT, 1f))
        head.addView(Button(this).apply {
            text = "↻"
            setOnClickListener { load() }
        }, LinearLayout.LayoutParams(LinearLayout.LayoutParams.WRAP_CONTENT,
            LinearLayout.LayoutParams.WRAP_CONTENT, 0f))
        root.addView(head, LinearLayout.LayoutParams(
            LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT))

        list = ListView(this).apply { choiceMode = ListView.CHOICE_MODE_SINGLE }
        root.addView(list, LinearLayout.LayoutParams(
            LinearLayout.LayoutParams.MATCH_PARENT, 0, 1f))

        val row1 = LinearLayout(this).apply { orientation = LinearLayout.HORIZONTAL }
        row1.addView(Button(this).apply {
            text = "↑ Наверх"
            textSize = 12f
            setOnClickListener { goUp() }
        }, LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f))
        row1.addView(Button(this).apply {
            text = "⬆ На ESP"
            textSize = 12f
            setOnClickListener { pickUpload() }
        }, LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f))
        root.addView(row1, LinearLayout.LayoutParams(
            LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT))

        val row2 = LinearLayout(this).apply { orientation = LinearLayout.HORIZONTAL }
        row2.addView(Button(this).apply {
            text = "⬇ Скачать"
            textSize = 12f
            setOnClickListener { downloadSelected() }
        }, LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f))
        row2.addView(Button(this).apply {
            text = "🗑 Удалить"
            textSize = 12f
            setOnClickListener { deleteSelected() }
        }, LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f))
        root.addView(row2, LinearLayout.LayoutParams(
            LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT))

        tvStatus = TextView(this).apply {
            textSize = 13f
            setTextColor(0xFF8FA3BF.toInt())
            setPadding(0, dp(6), 0, 0)
        }
        root.addView(tvStatus)

        // По нажатию: папка — войти, файл — выбрать/снять выбор
        list.setOnItemClickListener { _, _, pos, _ ->
            val e = entries.getOrNull(pos) ?: return@setOnItemClickListener
            if (e.dir) {
                cwd = (if (cwd.endsWith("/")) cwd else "$cwd/") + e.name
                selected = -1
                load()
            } else {
                selected = if (selected == pos) -1 else pos
                render()
            }
        }

        setContentView(root)
    }

    private fun status(msg: String) = runOnUiThread { tvStatus.text = msg }

    private fun full(name: String) =
        if (cwd.endsWith("/")) cwd + name else "$cwd/$name"

    private fun humanSize(n: Long): String = when {
        n >= 1_048_576L -> "%.1f МБ".format(n / 1_048_576.0)
        n >= 1024L      -> "%d КБ".format(n / 1024)
        else            -> "$n Б"
    }

    private fun render() {
        runOnUiThread {
            val rows = entries.mapIndexed { i, e ->
                val icon = if (e.dir) "📁" else "📄"
                val size = if (e.dir) "" else "   ${humanSize(e.size)}"
                (if (i == selected) "✔ " else " ") + "$icon ${e.name}$size"
            }
            list.adapter = ArrayAdapter(this,
                android.R.layout.simple_list_item_1, rows)
            tvPath.text = cwd
        }
    }

    private fun load() {
        val path = cwd
        status("Читаю $path…")
        exec.execute {
            try {
                val (code, data) = espRequest("/api/fs?path=" +
                    URLEncoder.encode(path, "UTF-8"))
                if (code != 200) throw IOException("HTTP $code")
                val arr = JSONArray(String(data, Charsets.UTF_8))
                val items = ArrayList<FsEntry>(arr.length())
                for (i in 0 until arr.length()) {
                    val o = arr.getJSONObject(i)
                    items.add(FsEntry(o.getString("n"),
                        o.optInt("d") == 1, o.optLong("s")))
                }
                entries = items.sortedWith(
                    compareBy({ !it.dir }, { it.name.lowercase() }))
                selected = -1
                render()
                status("Элементов: ${entries.size}")
            } catch (e: Exception) {
                status("Ошибка: ${e.message}")
            }
        }
    }

    private fun goUp() {
        if (cwd == "/") return
        cwd = if (cwd == "/flash" || cwd == "/sd") "/"
              else cwd.substringBeforeLast('/', "/")
        selected = -1
        load()
    }

    // ---- загрузка файла с телефона на устройство ----

    private fun pickUpload() {
        if (cwd == "/") { status("Войдите в папку: flash или sd"); return }
        val intent = Intent(Intent.ACTION_OPEN_DOCUMENT)
            .addCategory(Intent.CATEGORY_OPENABLE)
            .setType("*/*")
        startActivityForResult(intent, 200)
    }

    private fun displayName(uri: Uri): String {
        contentResolver.query(uri, arrayOf(OpenableColumns.DISPLAY_NAME),
            null, null, null)?.use { c ->
            if (c.moveToFirst()) {
                val i = c.getColumnIndex(OpenableColumns.DISPLAY_NAME)
                if (i >= 0) return c.getString(i) ?: "file"
            }
        }
        return uri.lastPathSegment ?: "file"
    }

    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        super.onActivityResult(requestCode, resultCode, data)
        if (requestCode != 200 || resultCode != RESULT_OK) return
        val uri = data?.data ?: return
        val name = displayName(uri)
        exec.execute {
            try {
                val bytes = contentResolver.openInputStream(uri)?.use { it.readBytes() }
                    ?: throw IOException("не удалось прочитать файл")
                status("Загрузка $name (${bytes.size / 1024} КБ)…")
                val (code, _) = espRequest("/api/file?path=" +
                    URLEncoder.encode(full(name), "UTF-8"), "POST", null, bytes)
                if (code != 200) throw IOException("HTTP $code")
                status("Загружено: $name")
                load()
            } catch (e: Exception) {
                status("Ошибка: ${e.message}")
            }
        }
    }

    // ---- скачивание файла с устройства в «Загрузки» телефона ----

    private fun downloadSelected() {
        val e = entries.getOrNull(selected)
        if (e == null) { status("Выберите файл (нажмите на строку)"); return }
        if (e.dir) { status("Это папка — войдите в неё"); return }
        exec.execute {
            try {
                status("Скачиваю ${e.name}…")
                val (code, data) = espRequest("/api/file?path=" +
                    URLEncoder.encode(full(e.name), "UTF-8"))
                if (code != 200 || data.isEmpty()) throw IOException("HTTP $code")
                val values = ContentValues().apply {
                    put(MediaStore.MediaColumns.DISPLAY_NAME, e.name)
                    put(MediaStore.MediaColumns.MIME_TYPE, "application/octet-stream")
                    put(MediaStore.MediaColumns.RELATIVE_PATH,
                        Environment.DIRECTORY_DOWNLOADS)
                }
                val uri = contentResolver.insert(
                    MediaStore.Downloads.EXTERNAL_CONTENT_URI, values)
                    ?: throw IOException("MediaStore недоступен")
                contentResolver.openOutputStream(uri)?.use { it.write(data) }
                    ?: throw IOException("нет потока записи")
                status("Сохранено в Загрузки: ${e.name}")
            } catch (ex: Exception) {
                status("Ошибка: ${ex.message}")
            }
        }
    }

    // ---- удаление ----

    private fun deleteSelected() {
        val e = entries.getOrNull(selected)
        if (e == null) { status("Выберите элемент"); return }
        val target = full(e.name)
        AlertDialog.Builder(this)
            .setTitle("Удалить?")
            .setMessage(target + if (e.dir) "\n(папка должна быть пустой)" else "")
            .setPositiveButton("Удалить") { _, _ ->
                exec.execute {
                    try {
                        status("Удаляю ${e.name}…")
                        val (code, _) = espRequest("/api/file?path=" +
                            URLEncoder.encode(target, "UTF-8"), "DELETE")
                        if (code != 200) throw IOException("HTTP $code")
                        status("Удалено: ${e.name}")
                        load()
                    } catch (ex: Exception) {
                        status("Ошибка: ${ex.message}")
                    }
                }
            }
            .setNegativeButton("Отмена", null)
            .show()
    }
}
