#include "services/ai_link.h"
#include "services/lang_service.h"
#include "services/wifi_service.h"
#include "ui/ui_manager.h"
#include "ui/app_host.h"
#include <Arduino.h>
#include <WebServer.h>
#include <FS.h>
#include <LittleFS.h>
#include <SD_MMC.h>
#include <freertos/semphr.h>
#include <cstring>
#include <cstdio>
#include <ctime>

// Данные общие: пишутся из LVGL-потока (событие фото) и HTTP-задачи
// (обмен от телефона), читаются из LVGL (экран «ИИ»).
// lk_mux — критическая секция; hist_file_sem — сериализация записи файла.
#define ANSWER_MAX 4096
#define HIST_FILE  "/ai_chat.log"

static portMUX_TYPE lk_mux = portMUX_INITIALIZER_UNLOCKED;
static SemaphoreHandle_t hist_file_sem = nullptr;

static WebServer server(80);

static fs::FS*    photo_fs = nullptr;
static char       photo_path[64] = {0};
static char       photo_name[32] = {0};
static uint32_t   photo_id = 0;
static AiLinkState state = AI_LINK_IDLE;

static char       answer_buf[ANSWER_MAX];
static size_t     answer_len = 0;
static uint32_t   answer_seq = 0;
static uint32_t   ui_seq = 0;      // любое событие чата → автооткрытия экрана
static uint32_t   send_seq = 0;    // запрос «Отправить» из чата

// Кольцо истории: без memmove, head — слот следующей записи
static AiHistEntry hist[AI_HIST_MAX];
static int         hist_head = 0;
static int         hist_cnt = 0;
static uint32_t    hist_seq = 0;

// Буферы под запись файла (защищены hist_file_sem)
static AiHistEntry pers_tmp;
static char        pers_line[AI_HIST_Q + AI_HIST_A + 64];

static volatile bool ai_ready = false;
static volatile bool server_started = false;

// ---------- Геттеры ----------

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

uint32_t ai_link_history_seq() {
    return hist_seq;
}

bool ai_link_request_send() {
    bool ok = false;
    portENTER_CRITICAL(&lk_mux);
    if (photo_path[0]) { send_seq++; ok = true; }
    portEXIT_CRITICAL(&lk_mux);
    if (ok) Serial.printf("ai_link: send requested (#%u)\n", (unsigned)send_seq);
    return ok;
}

uint32_t ai_link_send_seq() {
    return send_seq;
}

int ai_link_history_count() {
    portENTER_CRITICAL(&lk_mux);
    int n = hist_cnt;
    portEXIT_CRITICAL(&lk_mux);
    return n;
}

bool ai_link_history_get(int idx, AiHistEntry* out) {
    if (!out || idx < 0) return false;
    bool ok = false;
    portENTER_CRITICAL(&lk_mux);
    if (idx < hist_cnt) {
        int phys = (hist_head - hist_cnt + idx + AI_HIST_MAX * 2) % AI_HIST_MAX;
        *out = hist[phys];
        ok = true;
    }
    portEXIT_CRITICAL(&lk_mux);
    return ok;
}

// ---------- История: кодирование строк файла ----------

// '\n' → "\\n", '\\' → "\\\", '\t'/0x1F → ' ', '\r' убрать
static void esc_write(char* dst, size_t max, size_t* pos, const char* src) {
    for (size_t i = 0; src[i] && *pos < max - 1; i++) {
        char c = src[i];
        if (c == '\r') continue;
        if (c == '\n' || c == '\\') {
            if (*pos >= max - 2) break;
            dst[(*pos)++] = '\\';
            dst[(*pos)++] = (c == '\n') ? 'n' : '\\';
        } else if (c == '\t' || c == '\x1f') {
            dst[(*pos)++] = ' ';
        } else {
            dst[(*pos)++] = c;
        }
    }
    dst[*pos] = '\0';
}

static void esc_read(const char* src, char* dst, size_t max) {
    size_t o = 0;
    for (size_t i = 0; src[i] && o < max - 1; i++) {
        char c = src[i];
        if (c == '\\' && src[i + 1]) {
            i++;
            c = (src[i] == 'n') ? '\n' : src[i];  // \n и \\, прочее — как есть
        }
        dst[o++] = c;
    }
    dst[o] = '\0';
}

// Обрезать ровно по границе UTF-8 (не рвать символ посередине)
static void utf8_cut(char* s, size_t max) {
    if (strlen(s) < max) return;
    s[max - 1] = '\0';
    size_t n = strlen(s);
    while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80) n--;
    s[n] = '\0';
}

static void now_hm(char* out, size_t max) {
    time_t t = time(nullptr);
    struct tm* lt = localtime(&t);
    snprintf(out, max, "%02d:%02d", lt->tm_hour, lt->tm_min);
}

// Файл: строки вида "T\x1fHH:MM\x1fq\x1fa\n" (экранировано)
static void persist_append(int phys) {
    if (!hist_file_sem) return;
    xSemaphoreTake(hist_file_sem, portMAX_DELAY);
    portENTER_CRITICAL(&lk_mux);
    pers_tmp = hist[phys];
    portEXIT_CRITICAL(&lk_mux);

    size_t pos = 0;
    pers_line[pos++] = (char)('0' + pers_tmp.type);
    pers_line[pos++] = '\x1f';
    esc_write(pers_line, sizeof(pers_line), &pos, pers_tmp.time);
    pers_line[pos++] = '\x1f';
    esc_write(pers_line, sizeof(pers_line), &pos, pers_tmp.q);
    pers_line[pos++] = '\x1f';
    esc_write(pers_line, sizeof(pers_line), &pos, pers_tmp.a);
    pers_line[pos++] = '\n';
    pers_line[pos] = '\0';

    File f = LittleFS.open(HIST_FILE, FILE_APPEND);
    if (f) {
        f.print(pers_line);
        f.close();
    }
    xSemaphoreGive(hist_file_sem);
}

// Добавить событие в чат (type: 0=фото, 1=вопрос+ответ)
static void hist_add(uint8_t type, const char* q, const char* a) {
    portENTER_CRITICAL(&lk_mux);
    AiHistEntry& e = hist[hist_head];
    memset(&e, 0, sizeof(e));
    e.type = type;
    now_hm(e.time, sizeof(e.time));
    strncpy(e.q, q ? q : "", AI_HIST_Q - 1);
    strncpy(e.a, a ? a : "", AI_HIST_A - 1);
    utf8_cut(e.q, AI_HIST_Q);
    utf8_cut(e.a, AI_HIST_A);
    hist_head = (hist_head + 1) % AI_HIST_MAX;
    if (hist_cnt < AI_HIST_MAX) hist_cnt++;
    hist_seq++;
    if (type == 1) {           // полный обмен — автооткрытие + «ответ получен»
        answer_seq++;
        state = AI_LINK_ANSWERED;
        ui_seq++;
    } else if (type == 2) {     // вопрос с телефона — тоже показать чат
        ui_seq++;
    }
    int phys = (hist_head - 1 + AI_HIST_MAX) % AI_HIST_MAX;
    portEXIT_CRITICAL(&lk_mux);

    persist_append(phys);
}

// Переписать файл истории актуальными строками (компактизация / дозапись)
static void persist_rewrite() {
    if (!hist_file_sem) return;
    xSemaphoreTake(hist_file_sem, portMAX_DELAY);
    File w = LittleFS.open(HIST_FILE, FILE_WRITE);
    if (w) {
        for (int i = 0; i < hist_cnt; i++) {
            int phys = (hist_head - hist_cnt + i + AI_HIST_MAX * 2) % AI_HIST_MAX;
            AiHistEntry e;
            portENTER_CRITICAL(&lk_mux);
            e = hist[phys];
            portEXIT_CRITICAL(&lk_mux);
            size_t pos = 0;
            pers_line[pos++] = (char)('0' + e.type);
            pers_line[pos++] = '\x1f';
            esc_write(pers_line, sizeof(pers_line), &pos, e.time);
            pers_line[pos++] = '\x1f';
            esc_write(pers_line, sizeof(pers_line), &pos, e.q);
            pers_line[pos++] = '\x1f';
            esc_write(pers_line, sizeof(pers_line), &pos, e.a);
            pers_line[pos++] = '\n';
            pers_line[pos] = '\0';
            w.print(pers_line);
        }
        w.close();
    }
    xSemaphoreGive(hist_file_sem);
}

// Ответ пришёл на последний «висящий» вопрос (type=2) — дописать его туда.
// false = висящего вопроса нет, вызывающий должен дописать новый обмен.
static bool hist_fill_last_answer(const char* a) {
    bool filled = false;
    portENTER_CRITICAL(&lk_mux);
    if (hist_cnt > 0) {
        int phys = (hist_head - 1 + AI_HIST_MAX) % AI_HIST_MAX;
        if (hist[phys].type == 2) {
            strncpy(hist[phys].a, a ? a : "", AI_HIST_A - 1);
            utf8_cut(hist[phys].a, AI_HIST_A);
            hist[phys].type = 1;
            hist_seq++;
            ui_seq++;
            answer_seq++;
            state = AI_LINK_ANSWERED;
            filled = true;
        }
    }
    portEXIT_CRITICAL(&lk_mux);

    if (filled) persist_rewrite();   // строка в файле уже лежит — переписать
    return filled;
}

// Загрузка истории из файла + компактизация (файл ≤ AI_HIST_MAX строк)
static void hist_load() {
    if (!LittleFS.exists(HIST_FILE)) return;
    File f = LittleFS.open(HIST_FILE, FILE_READ);
    if (!f) return;
    size_t sz = f.size();
    if (sz > 65536) {          // защита от мусорного файла
        f.close();
        LittleFS.remove(HIST_FILE);
        return;
    }
    char* buf = (char*)malloc(sz + 1);
    if (!buf) { f.close(); return; }
    f.read((uint8_t*)buf, sz);
    buf[sz] = '\0';
    f.close();

    char* line = strtok(buf, "\n");
    while (line) {
        // Формат: "T\x1fHH:MM\x1fq\x1fa" — разделители после типа,
        // времени и вопроса; ответ идёт до конца строки.
        if (strlen(line) > 4 && line[1] == '\x1f') {
            uint8_t type = (uint8_t)(line[0] - '0');
            char* f1 = strchr(line + 2, '\x1f');               // конец time
            char* f2 = f1 ? strchr(f1 + 1, '\x1f') : nullptr;  // конец q
            if (type <= 2 && f1 && f2) {
                *f1 = '\0';
                *f2 = '\0';
                AiHistEntry& e = hist[hist_head];
                memset(&e, 0, sizeof(e));
                e.type = type;
                strncpy(e.time, line + 2, sizeof(e.time) - 1);
                esc_read(f1 + 1, e.q, AI_HIST_Q);
                esc_read(f2 + 1, e.a, AI_HIST_A);
                hist_head = (hist_head + 1) % AI_HIST_MAX;
                if (hist_cnt < AI_HIST_MAX) hist_cnt++;
            }
        }
        line = strtok(nullptr, "\n");
    }
    free(buf);

    persist_rewrite();            // компактизация: только актуальные строки
    if (hist_cnt) hist_seq = 1;   // есть прошлая переписка (не открывать экран)
    Serial.printf("ai_link: history loaded (%d entries)\n", hist_cnt);
}

// ---------- Очередь фото ----------

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

    hist_add(0, "", "");   // событие в чат: «фото сохранено»
    Serial.printf("ai_link: photo #%u queued (%s)\n",
                  (unsigned)photo_id, photo_path);
}

// ---------- Разбор ----------

static void url_decode(const char* in, char* out, size_t max) {
    size_t o = 0;
    auto hex = [](char h) -> int {
        if (h >= '0' && h <= '9') return h - '0';
        if (h >= 'a' && h <= 'f') return h - 'a' + 10;
        if (h >= 'A' && h <= 'F') return h - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; in[i] && o < max - 1; i++) {
        char c = in[i];
        if (c == '+') {
            c = ' ';
        } else if (c == '%' && in[i + 1] && in[i + 2]) {
            int hi = hex(in[i + 1]), lo = hex(in[i + 2]);
            if (hi >= 0 && lo >= 0) {
                c = (char)(hi * 16 + lo);
                i += 2;
            }
        }
        out[o++] = c;
    }
    out[o] = '\0';
}

static void json_escape_write(String& out, const char* s) {
    for (size_t i = 0; s[i]; i++) {
        char c = s[i];
        if (c == '"')       out += F("\\\"");
        else if (c == '\\') out += F("\\\\");
        else if (c == '\n') out += F("\\n");
        else if (c == '\r') out += F("\\r");
        else if (c == '\t') out += F("\\t");
        else if ((unsigned char)c < 0x20) { /* пропускаем управляющие */ }
        else out += c;
    }
}

// ---------- HTTP-обработчики (HTTP-задача) ----------

static void h_state() {
    char name[32];
    char buf[288];
    portENTER_CRITICAL(&lk_mux);
    strncpy(name, photo_name, sizeof(name) - 1);
    name[sizeof(name) - 1] = '\0';
    AiLinkState st = state;
    uint32_t id = photo_id;
    size_t alen = answer_len;
    portEXIT_CRITICAL(&lk_mux);

    snprintf(buf, sizeof(buf),
             "{\"state\":%d,\"id\":%u,\"name\":\"%s\",\"seq\":%u,\"alen\":%u,\"hseq\":%u,\"hcnt\":%d,\"send\":%u}",
             (int)st, (unsigned)id, name,
             (unsigned)answer_seq, (unsigned)alen,
             (unsigned)hist_seq, hist_cnt, (unsigned)send_seq);
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

// Общий обработчик ответа: вопрос (опц., URL-encoded в заголовке) + текст
static void handle_chat_reply() {
    String id_h = server.header("X-Id");
    String q_h = server.header("X-Question");
    String body = server.arg("plain");
    if (body.length() == 0) {
        server.send(400, "text/plain", "empty body");
        return;
    }
    if ((size_t)body.length() >= ANSWER_MAX)
        body = body.substring(0, ANSWER_MAX - 1);

    char qdec[AI_HIST_Q];
    url_decode(q_h.c_str(), qdec, sizeof(qdec));

    portENTER_CRITICAL(&lk_mux);
    memcpy(answer_buf, body.c_str(), body.length());
    answer_len = body.length();
    portEXIT_CRITICAL(&lk_mux);

    // Сначала — дозапись в последний «висящий» вопрос, иначе новый обмен
    if (!hist_fill_last_answer(body.c_str()))
        hist_add(1, qdec, body.c_str());

    Serial.printf("ai_link: chat reply #%u (X-Id=%s, q=%u chars, a=%u chars)\n",
                  (unsigned)answer_seq, id_h.c_str(),
                  (unsigned)strlen(qdec), (unsigned)body.length());
    server.send(200, "text/plain", "ok");
}

// POST /api/chat — вопрос+ответ (новый контракт приложения)
static void h_chat() { handle_chat_reply(); }

// POST /api/answer — только ответ (обратная совместимость)
static void h_answer() { handle_chat_reply(); }

// POST /api/question — вопрос с телефона без ответа (type=2, рисуется «…»,
// пока ИИ не ответит через /api/chat или /api/answer)
static void h_question() {
    String q_h = server.header("X-Question");
    char qdec[AI_HIST_Q];
    url_decode(q_h.c_str(), qdec, sizeof(qdec));
    if (!qdec[0]) {
        server.send(400, "text/plain", "no question");
        return;
    }
    hist_add(2, qdec, "");
    Serial.printf("ai_link: question from phone (%u chars)\n",
                  (unsigned)strlen(qdec));
    server.send(200, "text/plain", "ok");
}

// GET /api/history — вся переписка для телефона
static void h_history() {
    String out;
    out.reserve(2048 + hist_cnt * (AI_HIST_Q + AI_HIST_A));
    out += '[';
    for (int i = 0; i < hist_cnt; i++) {
        AiHistEntry e;
        if (!ai_link_history_get(i, &e)) continue;
        if (i) out += ',';
        out += F("{\"t\":");
        out += (int)e.type;
        out += F(",\"time\":\"");
        out += e.time;
        out += F("\",\"q\":\"");
        json_escape_write(out, e.q);
        out += F("\",\"a\":\"");
        json_escape_write(out, e.a);
        out += F("\"}");
    }
    out += ']';
    server.send(200, "application/json", out);
}

// ---------- Файловый API (телефон: список/скачать/загрузить/удалить) ----------
// Виртуальные пути: "/flash/..." → LittleFS, "/sd/..." → SD (/sdcard).
// "/" — корень с двумя папками (sd — только если карта вставлена).

static fs::FS* fs_resolve(const String& vpath, String& real) {
    real = "";
    if (vpath == "/flash" || vpath.startsWith("/flash/")) {
        real = "/";
        real += vpath.substring(6);          // "/flash/images" → "/images"
        if (vpath.indexOf("..") >= 0) return nullptr;
        return &LittleFS;
    }
    if (vpath == "/sd" || vpath.startsWith("/sd/")) {
        real = "/sdcard";
        if (vpath.length() > 3) real += vpath.substring(3);  // "/sd/DCIM" → "/sdcard/DCIM"
        if (vpath.indexOf("..") >= 0) return nullptr;
        return &SD_MMC;
    }
    return nullptr;
}

// GET /api/fs?path=/flash/images — список каталога
static void h_fs() {
    String vp = server.arg("path");
    if (!vp.length()) vp = "/";

    if (vp == "/") {
        String out = F("[{\"n\":\"flash\",\"d\":1}");
        if (SD_MMC.cardType() != CARD_NONE) out += F(",{\"n\":\"sd\",\"d\":1}");
        out += ']';
        server.send(200, "application/json", out);
        return;
    }

    String real;
    fs::FS* fsp = fs_resolve(vp, real);
    if (!fsp) { server.send(400, "application/json", "{\"err\":\"bad path\"}"); return; }

    File dir = fsp->open(real, FILE_READ);
    if (!dir || !dir.isDirectory()) {
        if (dir) dir.close();
        server.send(404, "application/json", "{\"err\":\"not found\"}");
        return;
    }

    String out;
    out.reserve(2048);
    out += '[';
    bool first = true;
    File e;
    while ((e = dir.openNextFile())) {
        String n = e.name();
        int sl = n.lastIndexOf('/');
        if (sl >= 0) n = n.substring(sl + 1);
        if (n.length()) {
            if (!first) out += ',';
            first = false;
            out += F("{\"n\":\"");
            json_escape_write(out, n.c_str());
            out += F("\",\"s\":");
            out += (unsigned long)e.size();
            out += F(",\"d\":");
            out += e.isDirectory() ? 1 : 0;
            out += '}';
        }
        e.close();
    }
    out += ']';
    dir.close();
    server.send(200, "application/json", out);
}

// GET /api/file?path=/flash/img.jpg — скачать файл
static void h_file_get() {
    String vp = server.arg("path");
    String real;
    fs::FS* fsp = fs_resolve(vp, real);
    if (!fsp) { server.send(400, "text/plain", "bad path"); return; }
    File f = fsp->open(real, FILE_READ);
    if (!f || f.isDirectory()) {
        if (f) f.close();
        server.send(404, "text/plain", "not found");
        return;
    }
    server.streamFile(f, "application/octet-stream");
    f.close();
}

// POST /api/file?path=/flash/new.bin — загрузить (потоково, по 2-4 КБ)
static fs::FS* up_fs = nullptr;
static String   up_real;
static File     up_file;
static bool     up_err = false;

static void h_file_upload() {
    HTTPUpload& u = server.upload();
    if (u.status == UPLOAD_FILE_START) {
        up_err = false;
        up_file = File();
        up_real = "";
        String vp = server.arg("path");
        up_fs = vp.length() ? fs_resolve(vp, up_real) : nullptr;
        if (!up_fs || up_real == "/" || up_real == "/sdcard") { up_err = true; return; }
        if (up_fs->exists(up_real)) up_fs->remove(up_real);
        up_file = up_fs->open(up_real, FILE_WRITE);
        if (!up_file || up_file.isDirectory()) { up_file = File(); up_err = true; }
    } else if (u.status == UPLOAD_FILE_WRITE) {
        if (up_err || !up_file) return;
        if (up_file.write(u.buf, u.currentSize) != u.currentSize) up_err = true;
    } else if (u.status == UPLOAD_FILE_END) {
        if (up_file) up_file.close();
    }
}

static void h_file_post() {
    if (up_err && up_fs && up_real.length() && up_fs->exists(up_real))
        up_fs->remove(up_real);          // недописанный файл не оставляем
    up_fs = nullptr;
    if (up_err) { server.send(500, "application/json", "{\"err\":\"write failed\"}"); return; }
    server.send(200, "application/json", "{\"ok\":1}");
}

// DELETE /api/file?path=/flash/img.jpg — удалить файл или пустую папку
static void h_file_del() {
    String vp = server.arg("path");
    String real;
    fs::FS* fsp = fs_resolve(vp, real);
    if (!fsp || real == "/" || real == "/sdcard") {
        server.send(400, "application/json", "{\"err\":\"bad path\"}");
        return;
    }
    if (!fsp->exists(real)) {
        server.send(404, "application/json", "{\"err\":\"not found\"}");
        return;
    }
    if (!fsp->remove(real)) {
        server.send(500, "application/json", "{\"err\":\"remove failed\"}");
        return;
    }
    server.send(200, "application/json", "{\"ok\":1}");
}

// Страница для отладки из браузера телефона
static void h_root() {
    char name[32];
    char ans[ANSWER_MAX];
    AiLinkState st;
    uint32_t id;
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
    html += F("<p><a href='/api/history'>переписка (JSON)</a></p>");
    if (alen) {
        html += F("<p>Последний ответ:</p><pre>");
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

// ---------- LVGL-таймер: автооткрытия экрана «ИИ» ----------

static void poll_cb(lv_timer_t*) {
    uint32_t seq = ui_seq;   // вопрос или ответ — открыть чат в любом случае
    if (seq == 0) return;
    static uint32_t seen_seq = 0;
    if (seq == seen_seq) return;

    if (ui_manager_current() == SCREEN_SPLASH) return;

    int ai_idx = app_host_ai_index();
    if (ai_idx < 0) return;

    if (app_host_in_app() && app_host_current() == ai_idx) {
        seen_seq = seq;
        return;
    }
    seen_seq = seq;
    app_host_open_index(ai_idx);
}

// ---------- Сервер ----------

static void http_task_fn(void*) {
    for (;;) {
        if (server_started) server.handleClient();
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

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
    hist_file_sem = xSemaphoreCreateMutex();

    hist_load();   // до обработчиков: файл читаем один раз при загрузке

    const char* hdr_keys[] = {"X-Id", "X-Question"};
    server.collectHeaders(hdr_keys, 2);
    server.on("/", HTTP_GET, h_root);
    server.on("/api/state", HTTP_GET, h_state);
    server.on("/api/photo", HTTP_GET, h_photo);
    server.on("/api/question", HTTP_POST, h_question);
    server.on("/api/chat", HTTP_POST, h_chat);
    server.on("/api/answer", HTTP_POST, h_answer);
    server.on("/api/history", HTTP_GET, h_history);
    server.on("/api/fs", HTTP_GET, h_fs);
    server.on("/api/file", HTTP_GET, h_file_get);
    server.on("/api/file", HTTP_POST, h_file_post, h_file_upload);
    server.on("/api/file", HTTP_DELETE, h_file_del);
    ai_ready = true;

    xTaskCreatePinnedToCore(http_task_fn, "ai_http", 8192, nullptr, 1, nullptr, 1);

    lv_timer_create(poll_cb, 300, nullptr);
    ai_link_ensure_server();
}
