#include "services/ai_link.h"
#include "services/lang_service.h"
#include "services/wifi_service.h"
#include "ui/ui_manager.h"
#include "ui/app_host.h"
#include <Arduino.h>
#include <WebServer.h>
#include <FS.h>
#include <cstring>
#include <cstdio>

// Все общие данные (фото в очереди + текст ответа) защищены одним мьютексом:
// пишутся из LVGL-потока (сохранение фото) и из HTTP-задачи (ответ телефона).
#define ANSWER_MAX 4096

static portMUX_TYPE lk_mux = portMUX_INITIALIZER_UNLOCKED;

static WebServer server(80);

static fs::FS*    photo_fs = nullptr;
static char       photo_path[64] = {0};
static char       photo_name[32] = {0};
static uint32_t   photo_id = 0;
static AiLinkState state = AI_LINK_IDLE;

static char       answer_buf[ANSWER_MAX];
static size_t     answer_len = 0;
static uint32_t   answer_seq = 0;

static volatile bool ai_ready = false;        // обработчики зарегистрированы
static volatile bool server_started = false;  // server.begin() вызван

// --- Геттеры (LVGL-поток) ---

AiLinkState ai_link_state() {
    portENTER_CRITICAL(&lk_mux);
    AiLinkState s = state;
    portEXIT_CRITICAL(&lk_mux);
    return s;
}

uint32_t ai_link_photo_id() {
    portENTER_CRITICAL(&lk_mux);
    uint32_t id = photo_id;
    portEXIT_CRITICAL(&lk_mux);
    return id;
}

uint32_t ai_link_answer_seq() {
    return answer_seq;  // uint32 — атомарное чтение на Xtensa
}

size_t ai_link_answer_copy(char* out, size_t max) {
    if (!out || max == 0) return 0;
    portENTER_CRITICAL(&lk_mux);
    size_t n = answer_len < max - 1 ? answer_len : max - 1;
    memcpy(out, answer_buf, n);
    out[n] = '\0';
    portEXIT_CRITICAL(&lk_mux);
    return n;
}

void ai_link_notify_photo(fs::FS* f, const char* path) {
    if (!f || !path) return;
    const char* base = strrchr(path, '/');
    base = base ? base + 1 : path;

    portENTER_CRITICAL(&lk_mux);
    photo_fs = f;
    strncpy(photo_path, path, sizeof(photo_path) - 1);
    photo_path[sizeof(photo_path) - 1] = '\0';
    strncpy(photo_name, base, sizeof(photo_name) - 1);
    photo_name[sizeof(photo_name) - 1] = '\0';
    photo_id++;
    state = AI_LINK_PENDING;
    portEXIT_CRITICAL(&lk_mux);

    Serial.printf("ai_link: photo #%u queued (%s)\n",
                  (unsigned)photo_id, photo_path);
}

// --- HTTP-обработчики (выполняются в HTTP-задаче) ---

static void h_state() {
    char name[32];
    char buf[256];
    portENTER_CRITICAL(&lk_mux);
    strncpy(name, photo_name, sizeof(name) - 1);
    name[sizeof(name) - 1] = '\0';
    AiLinkState st = state;
    uint32_t id = photo_id;
    size_t alen = answer_len;
    portEXIT_CRITICAL(&lk_mux);

    snprintf(buf, sizeof(buf),
             "{\"state\":%d,\"id\":%u,\"name\":\"%s\",\"seq\":%u,\"alen\":%u}",
             (int)st, (unsigned)id, name,
             (unsigned)answer_seq, (unsigned)alen);
    server.send(200, "application/json", buf);
}

static void h_photo() {
    fs::FS* fsp;
    char p[64];
    portENTER_CRITICAL(&lk_mux);
    fsp = photo_fs;
    strncpy(p, photo_path, sizeof(p) - 1);
    p[sizeof(p) - 1] = '\0';
    portEXIT_CRITICAL(&lk_mux);

    if (!fsp || !p[0]) {
        server.send(404, "application/json", "{\"err\":\"no photo\"}");
        return;
    }
    File f = fsp->open(p, FILE_READ);
    if (!f) {
        server.send(404, "application/json", "{\"err\":\"open failed\"}");
        return;
    }
    server.streamFile(f, "image/jpeg");
    f.close();

    portENTER_CRITICAL(&lk_mux);
    if (state == AI_LINK_PENDING) state = AI_LINK_TAKEN;
    portEXIT_CRITICAL(&lk_mux);
    Serial.printf("ai_link: photo #%s sent to phone\n", p);
}

static void h_answer() {
    String id_h = server.header("X-Id");
    String body = server.arg("plain");
    if (body.length() == 0) {
        server.send(400, "text/plain", "empty body");
        return;
    }
    if ((size_t)body.length() >= ANSWER_MAX)
        body = body.substring(0, ANSWER_MAX - 1);

    portENTER_CRITICAL(&lk_mux);
    memcpy(answer_buf, body.c_str(), body.length());
    answer_len = body.length();
    answer_seq++;
    state = AI_LINK_ANSWERED;
    portEXIT_CRITICAL(&lk_mux);

    Serial.printf("ai_link: answer #%u received (%u bytes, for X-Id=%s)\n",
                  (unsigned)answer_seq, (unsigned)body.length(),
                  id_h.c_str());
    server.send(200, "text/plain", "ok");
}

// Страница для отладки из браузера телефона — путь без приложения.
static void h_root() {
    char name[32];
    char ans[ANSWER_MAX];
    AiLinkState st;
    uint32_t id, seq;
    size_t alen;

    portENTER_CRITICAL(&lk_mux);
    strncpy(name, photo_name, sizeof(name) - 1);
    name[sizeof(name) - 1] = '\0';
    st = state;
    id = photo_id;
    alen = answer_len;
    memcpy(ans, answer_buf, answer_len);
    ans[answer_len] = '\0';
    portEXIT_CRITICAL(&lk_mux);
    seq = answer_seq;

    String html;
    html.reserve(1024 + alen);
    html += F("<html><head><meta charset='utf-8'>"
              "<meta name='viewport' content='width=device-width,initial-scale=1'>"
              "<title>CheatSheet2</title>"
              "<style>body{background:#0A1628;color:#eee;font-family:sans-serif;"
              "padding:16px}a{color:#4FA3FF}pre{white-space:pre-wrap;"
              "background:#132238;padding:12px;border-radius:8px}</style></head><body>");
    html += F("<h2>CheatSheet2</h2><p>state=");
    html += String((int)st);
    html += F(" photo_id=");
    html += String(id);
    if (name[0]) {
        html += F(" (");
        html += name;
        html += F(")");
    }
    html += F("</p>");
    if (st == AI_LINK_PENDING || st == AI_LINK_TAKEN)
        html += F("<p><a href='/api/photo'>&#8595; скачать фото</a></p>");
    if (alen) {
        html += F("<p>Ответ ИИ (#");
        html += String(seq);
        html += F("):</p><pre>");
        // экранирование HTML
        for (size_t i = 0; i < alen; i++) {
            char c = ans[i];
            if (c == '&') html += F("&amp;");
            else if (c == '<') html += F("&lt;");
            else if (c == '>') html += F("&gt;");
            else html += c;
        }
        html += F("</pre>");
    }
    html += F("</body></html>");
    server.send(200, "text/html", html);
}

// --- LVGL-таймер: автооткрытие экрана «ИИ» при новом ответе ---
// Ответ приходит из HTTP-задачи; переключать экраны можно только
// из потока LVGL, поэтому — через таймер.

static void poll_cb(lv_timer_t*) {
    uint32_t seq = answer_seq;
    if (seq == 0) return;                       // ответов ещё не было
    static uint32_t seen_seq = 0;
    if (seq == seen_seq) return;                // уже показали

    if (ui_manager_current() == SCREEN_SPLASH) return;  // ждём загрузки

    int ai_idx = app_host_ai_index();
    if (ai_idx < 0) return;

    if (app_host_in_app() && app_host_current() == ai_idx) {
        seen_seq = seq;   // экран уже открыт — обновит его собственный таймер
        return;
    }
    seen_seq = seq;
    app_host_open_index(ai_idx);
}

static void http_task_fn(void*) {
    for (;;) {
        // handleClient() безопасен только после begin(): до этого
        // lwIP может быть вообще не инициализирован (WiFi выключен)
        if (server_started) server.handleClient();
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

// Вызывается из ai_link_init и из wifi_service_start (когда AP поднят).
// server.begin() создаёт lwIP-сокет — без инициализированного стека
// это assert «Invalid mbox» и boot-loop с чёрным экраном.
void ai_link_ensure_server() {
    if (server_started || !ai_ready) return;
    if (!wifi_service_running()) {
        Serial.println("ai_link: server deferred (WiFi off)");
        return;
    }
    server.begin();
    server_started = true;
    Serial.println("ai_link: HTTP server on :80");
}

void ai_link_init() {
    const char* hdr_keys[] = {"X-Id"};
    server.collectHeaders(hdr_keys, 1);
    server.on("/", HTTP_GET, h_root);
    server.on("/api/state", HTTP_GET, h_state);
    server.on("/api/photo", HTTP_GET, h_photo);
    server.on("/api/answer", HTTP_POST, h_answer);
    ai_ready = true;

    xTaskCreatePinnedToCore(http_task_fn, "ai_http", 8192, nullptr, 1, nullptr, 1);

    lv_timer_create(poll_cb, 300, nullptr);
    ai_link_ensure_server();   // если WiFi уже поднят — начинаем сразу
}
