#include "fonts/fonts.h"
#include "apps/builtin/camera_app.h"
#include "drivers/camera_driver.h"
#include "drivers/jpeg_decoder.h"
#include "drivers/input.h"
#include "services/lang_service.h"
#include "services/ai_link.h"
#include "services/storage_service.h"
#include "ui/theme.h"
#include <Arduino.h>
#include <JPEGENC.h>
#include <time.h>
#include <FS.h>
#include <LittleFS.h>
#include <SD_MMC.h>

#define SCREEN_W 320
#define SCREEN_H 240
#define IMG_W SCREEN_W
#define IMG_H SCREEN_H        // превью во весь экран; шапка/подсказки — поверх
#define AF_W 400              // буфер превью и метрики AF = 1/4 UXGA
#define AF_H 300

// --- State ---
static lv_obj_t* parent_ref = nullptr;
static lv_obj_t* lbl_status = nullptr;
static lv_obj_t* canvas = nullptr;
static lv_timer_t* refresh_timer = nullptr;

static uint8_t* canvas_buf = nullptr;
static uint8_t* temp_buf = nullptr;
static uint32_t canvas_stride = 0;

// --- Плашка «Снято» ---
static lv_obj_t* toast = nullptr;
static lv_obj_t* toast_lbl = nullptr;

static void toast_set(const char* text, lv_color_t bg) {
    if (!parent_ref) return;

    if (!toast) {
        toast = lv_obj_create(parent_ref);
        lv_obj_set_size(toast, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_style_radius(toast, 12, 0);
        lv_obj_set_style_border_width(toast, 0, 0);
        lv_obj_set_style_pad_all(toast, 14, 0);
        lv_obj_set_style_bg_opa(toast, LV_OPA_90, 0);
        lv_obj_set_scrollbar_mode(toast, LV_SCROLLBAR_MODE_OFF);
        lv_obj_remove_flag(toast, LV_OBJ_FLAG_SCROLLABLE);

        toast_lbl = lv_label_create(toast);
        lv_obj_set_style_text_color(toast_lbl, lv_color_white(), 0);
        lv_obj_set_style_text_font(toast_lbl, &lv_font_cyr_24, 0);
        lv_obj_set_style_text_align(toast_lbl, LV_TEXT_ALIGN_CENTER, 0);
    }

    lv_obj_set_style_bg_color(toast, bg, 0);
    lv_label_set_text(toast_lbl, text);
    lv_obj_center(toast);
}

static void toast_hide() {
    if (toast) {
        lv_obj_delete(toast);
        toast = nullptr;
        toast_lbl = nullptr;
    }
}

static bool preview_active = false;
static bool saving = false;

static int cam_brightness = 0;   // -2..2
static int cam_focus = 512;      // 0..1023
static bool preview_fill = true; // true = во всю ширину (кроп сверху/снизу),
                                 // false = весь кадр (чёрные поля по бокам)

// --- Декод превью в отдельной задаче (core 0) ---
// Декод 1/4 UXGA занимает ~100+ мс. Раньше он шёл в потоке LVGL — каждый тик
// таймера «замораживал» интерфейс (кнопки/плашки отставали). Теперь LVGL только
// выдаёт кадр воркеру и показывает готовый буфер (смена указателя, без копий),
// а JPEGDEC + scale идут на втором ядре.
static TaskHandle_t pv_task = nullptr;
static volatile bool pv_stop = false;
static uint8_t* work_buf = nullptr;          // второй canvas-буфер: сюда пишет воркер
static volatile bool job_ready = false;      // кадр выдан воркеру (fb у воркера)
static volatile bool job_done = false;       // work_buf готов к показу
static uint8_t* job_fb = nullptr;
static size_t job_len = 0;
static portMUX_TYPE pv_mux = portMUX_INITIALIZER_UNLOCKED;

// Транспонирование (оси X/Y swapped — сенсор физически повёрнут) +
// вписывание (fit) или заполнение экрана (fill) с сохранением пропорций.
// fill: картинка идёт во всю ширину, лишнее сверху/снизу обрезается по центру.
static void scale_image(const uint16_t* src, int src_w, int src_h,
                        uint16_t* dst, int dst_w, int dst_h, uint32_t dst_stride) {
    if (src_w <= 0 || src_h <= 0 || dst_w <= 0 || dst_h <= 0) return;

    // Очистить весь canvas — фон под полями (в fill не виден)
    memset(dst, 0, (size_t)dst_stride * dst_h);

    // Транспонированный размер: ширина = высота src, высота = ширина src
    int t_w = src_h;
    int t_h = src_w;

    // fit  = минимум из масштабов (вписать целиком, поля по бокам)
    // fill = максимум (во весь экран, кроп по центру)
    int32_t scale_w = ((int32_t)dst_w << 16) / t_w;
    int32_t scale_h = ((int32_t)dst_h << 16) / t_h;
    int32_t scale = preview_fill
        ? (scale_w > scale_h ? scale_w : scale_h)
        : (scale_w < scale_h ? scale_w : scale_h);

    int out_w = (int)(((int64_t)t_w * scale) >> 16);
    int out_h = (int)(((int64_t)t_h * scale) >> 16);
    if (out_w < 1) out_w = 1;
    if (out_h < 1) out_h = 1;

    // Центрирование; при fill координаты отрицательные — кадрируем
    int x0 = (dst_w - out_w) / 2;
    int y0 = (dst_h - out_h) / 2;
    int dy0 = y0 < 0 ? -y0 : 0;
    int dy1 = out_h;
    if (y0 + out_h > dst_h) dy1 = dst_h - y0;
    int dx0 = x0 < 0 ? -x0 : 0;
    int dx1 = out_w;
    if (x0 + out_w > dst_w) dx1 = dst_w - x0;
    if (dy0 >= dy1 || dx0 >= dx1) return;

    // Строка dst ↔ столбец src, столбец dst ↔ строка src (транспонирование)
    int32_t sx_step = ((int32_t)src_w << 16) / out_h;   // dy → src_x
    int32_t sy_step = ((int32_t)src_h << 16) / out_w;   // dx → src_y

    for (int dy = dy0; dy < dy1; dy++) {
        uint16_t* dst_row = (uint16_t*)((uint8_t*)dst + (size_t)(y0 + dy) * dst_stride) + x0;
        int32_t sx_fixed = dy * sx_step;

        for (int dx = dx0; dx < dx1; dx++) {
            int32_t src_x = sx_fixed >> 16;
            int32_t src_y = ((int32_t)dx * sy_step) >> 16;

            if (src_x >= src_w) src_x = src_w - 1;
            if (src_y >= src_h) src_y = src_h - 1;

            dst_row[dx] = src[src_y * src_w + src_x];
        }
    }
}

// --- Preview timer: только выдача кадров и показ готового (декод — в pv_task_fn) ---
static void refresh_cb(lv_timer_t* timer) {
    if (!parent_ref || !lbl_status || !preview_active || !canvas || !canvas_buf) return;

    // 1) Готовый кадр воркера: показать его и решить, выдавать ли новый.
    //    Всё в одном критическом участке — иначе гонка «ready сброшен,
    //    а done ещё не виден» привела бы к выдаче нового кадра поверх
    //    не показанного (рваная картинка).
    bool done = false;
    bool issue = false;
    portENTER_CRITICAL(&pv_mux);
    done = job_done;
    if (done) job_done = false;
    issue = !job_ready && !job_done;
    portEXIT_CRITICAL(&pv_mux);

    if (done) {
        // Поменять показываемый и рабочий буферы местами (без копирования)
        uint8_t* tmp = canvas_buf;
        canvas_buf = work_buf;
        work_buf = tmp;
        lv_canvas_set_buffer(canvas, canvas_buf, IMG_W, IMG_H, LV_COLOR_FORMAT_RGB565);
        lv_obj_invalidate(canvas);
    }

    if (!issue) return;

    // 2) Снять свежий кадр и отдать воркеру
    uint8_t* jpeg_buf = nullptr;
    size_t jpeg_len = 0;
    if (!camera_capture(&jpeg_buf, &jpeg_len)) {
        lv_label_set_text_fmt(lbl_status, "%s %s",
                              LV_SYMBOL_WARNING, lang_str_camera_no_camera());
        return;
    }

    portENTER_CRITICAL(&pv_mux);
    job_fb = jpeg_buf;
    job_len = jpeg_len;
    job_ready = true;
    portEXIT_CRITICAL(&pv_mux);
    if (pv_task) xTaskNotifyGive(pv_task);
}

// Дождаться, пока воркер закончит текущий кадр: он держит fb камеры
// и использует temp_buf. Нужно перед автофокусом/снимком.
static void wait_preview_idle() {
    uint32_t t0 = millis();
    while (job_ready && millis() - t0 < 1500) {
        delay(10);
    }
}

// Воркер превью: core 0. Декодирует кадр (1/4 UXGA) и пишет в work_buf.
static void pv_task_fn(void* arg) {
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(30));
        if (pv_stop) break;

        uint8_t* fb = nullptr;
        size_t len = 0;
        bool have = false;

        portENTER_CRITICAL(&pv_mux);
        if (job_ready) { fb = job_fb; len = job_len; have = true; }
        portEXIT_CRITICAL(&pv_mux);
        if (!have) continue;

        int dw = 0, dh = 0;
        bool ok = jpeg_decode_to_rgb565(fb, len, temp_buf,
                                        AF_W, AF_H, AF_W * 2, 4, &dw, &dh);
        camera_release();   // fb можно вернуть сразу после декода
        if (ok && dw > 0 && dh > 0) {
            scale_image((uint16_t*)temp_buf, dw, dh,
                        (uint16_t*)work_buf, IMG_W, IMG_H, canvas_stride);
        }

        portENTER_CRITICAL(&pv_mux);
        job_ready = false;  // temp_buf и fb снова свободны
        job_done  = true;
        portEXIT_CRITICAL(&pv_mux);
    }

    // Остановка: если кадр был взят, но не обработан — вернуть fb
    bool leak = false;
    portENTER_CRITICAL(&pv_mux);
    leak = job_ready;
    if (leak) job_ready = false;
    portEXIT_CRITICAL(&pv_mux);
    if (leak) camera_release();

    pv_task = nullptr;
    vTaskDelete(nullptr);
}

// --- Контрастный автофокус (CDAF) ---
// Перебираем позиции фокус-мотора, меряем резкость (сумма модулей
// горизонтального/вертикального градиента яркости) на кадре 1/4 UXGA.
// Кадр обязателен свежий — снятый уже после установки DAC.
static uint64_t af_since_ms = 0;

static inline int af_luma(uint16_t p) {
    // приближение яркости из RGB565: 3*R + 6*Г + 1*B
    return (int)(((p >> 11) & 31) * 3 + ((p >> 5) & 63) * 6 + (p & 31));
}

static uint32_t measure_sharpness() {
    uint8_t* jbuf = nullptr;
    size_t jlen = 0;
    if (!camera_capture_after(&jbuf, &jlen, af_since_ms)) return 0;

    int dw = 0, dh = 0;
    bool ok = jpeg_decode_to_rgb565(jbuf, jlen, temp_buf,
                                    AF_W, AF_H, AF_W * 2, 4, &dw, &dh);
    camera_release();
    if (!ok || dw < 8 || dh < 8) return 0;

    uint32_t sum = 0;
    for (int y = 1; y < dh - 1; y++) {
        const uint16_t* row  = (const uint16_t*)(temp_buf + (size_t)y * AF_W * 2);
        const uint16_t* prow = (const uint16_t*)(temp_buf + (size_t)(y - 1) * AF_W * 2);
        for (int x = 1; x < dw - 1; x++) {
            int d = af_luma(row[x]) * 2 - af_luma(row[x - 1]) - af_luma(prow[x]);
            if (d < 0) d = -d;
            sum += (uint32_t)d;
        }
    }
    return sum;
}

// Полный проход: грубый перебор всего хода мотора + точная подстройка.
// ~2-2.5 с. Возвращает позицию лучшей резкости.
// fast=true — быстрый проход только вокруг прошлой позиции (~0.7 с);
// если пик оказался на краю окна (фокус уехал) — полный проход.
static int run_autofocus(int start_pos, bool fast = false) {
    if (!camera_is_ready() || !temp_buf) return start_pos;

    if (fast) {
        static const int NEAR[5] = {0, -32, 32, -64, 64};
        int best = start_pos;
        uint32_t bestm = 0;
        bool at_edge = false;
        for (int i = 0; i < 5; i++) {
            int p = start_pos + NEAR[i];
            if (p < 0 || p > 1023) continue;
            camera_set_focus(p);
            delay(5);
            af_since_ms = millis();
            uint32_t m = measure_sharpness();
            if (m > bestm) { bestm = m; best = p; at_edge = (NEAR[i] != 0 && abs(NEAR[i]) == 64); }
        }
        if (bestm == 0) {                       // кадров нет — фокус не трогаем
            camera_set_focus(start_pos);
            return start_pos;
        }
        if (!at_edge) {                         // пик внутри окна — этого достаточно
            camera_set_focus(best);
            Serial.printf("AF fast: pos=%d sharp=%u\n", best, bestm);
            return best;
        }
        Serial.println("AF fast: edge hit, full sweep");
    }

    static const int COARSE[8] = {32, 160, 288, 416, 544, 672, 800, 928};
    int best = start_pos;
    uint32_t bestm = 0;

    for (int i = 0; i < 8; i++) {
        camera_set_focus(COARSE[i]);
        delay(5);                     // мотору на сдвиг
        af_since_ms = millis();       // кадр должен начаться позже
        uint32_t m = measure_sharpness();
        if (m > bestm) { bestm = m; best = COARSE[i]; }
    }

    // Ни одного валидного кадра — текущий фокус не трогаем
    if (bestm == 0) {
        Serial.println("AF: no frames, keep current focus");
        camera_set_focus(start_pos);
        return start_pos;
    }

    const int FINE[4] = {best - 64, best - 32, best + 32, best + 64};
    for (int i = 0; i < 4; i++) {
        int p = FINE[i];
        if (p < 0 || p > 1023) continue;
        camera_set_focus(p);
        delay(5);
        af_since_ms = millis();
        uint32_t m = measure_sharpness();
        if (m > bestm) { bestm = m; best = p; }
    }

    camera_set_focus(best);
    Serial.printf("AF done: pos=%d sharp=%u\n", best, bestm);
    return best;
}

// --- JPEGENC: запись напрямую в открытый File ---
static File* enc_file = nullptr;
static bool enc_write_err = false;  // true, если диск переполнился в середине записи

static void* enc_open_cb(const char* /*name*/) {
    return enc_file;  // File уже открыт до вызова JPEGENC::open()
}
static int32_t enc_write_cb(JPEGE_FILE* f, uint8_t* buf, int32_t len) {
    File* fp = (File*)f->fHandle;
    if (!fp) { enc_write_err = true; return 0; }
    int32_t written = (int32_t)fp->write(buf, (size_t)len);
    if (written != len) enc_write_err = true;  // короткая запись = нет места
    return written;
}
static int32_t enc_read_cb(JPEGE_FILE*, uint8_t*, int32_t) {
    return 0;  // при кодировании не читаем
}
static int32_t enc_seek_cb(JPEGE_FILE* f, int32_t pos) {
    File* fp = (File*)f->fHandle;
    return fp && fp->seek((uint32_t)pos) ? pos : -1;
}

// Сохранить фото в той же ориентации, что и предпросмотр:
// JPEG → декодирование в RGB565 (полное разрешение) → транспонирование
// → повторное кодирование в JPEG.
static bool save_photo(const uint8_t* jpeg_data, size_t jpeg_len, size_t* out_size) {
    FS* fs;
    size_t free_bytes;

    switch (storage_get())
    {
        case 0:
            fs = &LittleFS;
            free_bytes = LittleFS.totalBytes() - LittleFS.usedBytes();
        break;

        case 1:
            fs = &SD_MMC;
            free_bytes = SD_MMC.totalBytes() - SD_MMC.usedBytes();
        break;
    }

    if (!fs->exists("/images")) {
        fs->mkdir("/images");
    }

    // Q_BEST + 4:4:4 дают файл крупнее исходного — закладываем запас 256 КБ
    if (free_bytes < jpeg_len * 2 + 262144) {
        Serial.printf("not enough space (%u free, need %u)\n",
                      (unsigned)free_bytes, (unsigned)(jpeg_len * 2 + 65536));
        return false;
    }

    // Узнать размеры исходника
    int src_w = 0, src_h = 0;
    if (!jpeg_open(jpeg_data, jpeg_len, &src_w, &src_h)) return false;

    // Транспонированный кадр: ширина = высота сенсора (1200),
    // высота = ширина сенсора (1600). ~3.84 MB в PSRAM.
    int out_w = src_h;
    int out_h = src_w;
    size_t buf_size = (size_t)out_w * out_h * 2;

    uint16_t* tbuf = (uint16_t*)ps_malloc(buf_size);
    if (!tbuf) {
        Serial.println("save: no PSRAM for rotation buffer");
        return false;
    }
    memset(tbuf, 0, buf_size);

    if (!jpeg_decode_to_rgb565(jpeg_data, jpeg_len, (uint8_t*)tbuf,
                               out_w, out_h, out_w * 2, 0,
                               nullptr, nullptr, true)) {
        Serial.println("save: decode failed");
        free(tbuf);
        return false;
    }

    time_t now = time(nullptr);
    struct tm* t = localtime(&now);
    char path[64];
    snprintf(path, sizeof(path), "/images/%04d%02d%02d_%02d%02d%02d.jpg",
             t->tm_year + 1900, t->tm_mon + 1, t->tm_mday,
             t->tm_hour, t->tm_min, t->tm_sec);

    File f = fs->open(path, FILE_WRITE);
    if (!f) { free(tbuf); return false; }

    static JPEGENC jpg;   // ~4KB — держим в статике, не на стеке
    JPEGENCODE enc;
    bool ok = false;

    enc_file = &f;
    enc_write_err = false;
    if (jpg.open(path, enc_open_cb, nullptr, enc_read_cb,
                 enc_write_cb, enc_seek_cb) == JPEGE_SUCCESS) {
        // Q_BEST + 4:4:4 — максимум деталей: края знаков резкие,
        // цветные подчёркивания/шкалы не размазываются (читает ИИ/OCR)
        if (jpg.encodeBegin(&enc, out_w, out_h, JPEGE_PIXEL_RGB565,
                            JPEGE_SUBSAMPLE_444, JPEGE_Q_BEST) == JPEGE_SUCCESS) {
            jpg.addFrame(&enc, (uint8_t*)tbuf, out_w * 2);
            int32_t total = jpg.close();
            ok = total > 0 && !enc_write_err;
            if (ok && out_size) *out_size = (size_t)total;
        }
    }
    enc_file = nullptr;
    f.close();
    if (!ok) {
        fs->remove(path);  // не оставляем битый/недописанный файл
        free(tbuf);
        Serial.println("save: encode/write failed, partial file removed");
        return false;
    }
    free(tbuf);

    Serial.printf("Saved %s (%d bytes, %dx%d)\n", path, (int)(out_size ? *out_size : 0), out_w, out_h);

    // Поставить фото в очередь на передачу телефону (HTTP-мост к ИИ)
    ai_link_notify_photo(fs, path);
    return true;
}

// --- Снимок из другого приложения (кнопка «Сфоткать» в чате «ИИ») ---
// Приложение камеры закрыто → камера не инициализирована: поднимаем её на
// время снимка, гоняем тот же AF и сохраняем тем же save_photo.
//
// Всё выполняется в ОТДЕЛЬНОЙ задаче с большим стеком на ядре 1: раньше
// цепочка шла в loopTask (8 КБ, ядро 0) и перезагружала консоль, плюс
// LVGL на несколько секунд замирал. LVGL только опрашивает cap_stage.

static volatile int      cap_stage = -1;   // -1 нет; 0..2 идёт; 3 успех; 4 ошибка
static volatile unsigned cap_kb = 0;
static volatile bool     cap_task_alive = false;

static void cap_task_fn(void*) {
    bool ok = false;
    bool inited_here = false;
    bool tmp_here = false;
    unsigned kb = 0;

    Serial.println("cap: start");
    cap_stage = 0;   // инициализация

    do {
        if (!camera_is_ready()) {
            if (!camera_init()) break;
            inited_here = true;
        }
        if (!temp_buf) {
            temp_buf = (uint8_t*)ps_malloc(AF_W * AF_H * 2);
            if (!temp_buf) break;
            tmp_here = true;
        }

        cap_stage = 1;   // автофокус (быстрый: вокруг прошлой позиции)
        cam_focus = run_autofocus(cam_focus, /*fast=*/true);

        cap_stage = 2;   // снимок + сохранение
        uint8_t* jb = nullptr;
        size_t jl = 0;
        if (!camera_capture(&jb, &jl)) break;

        size_t saved = 0;
        ok = save_photo(jb, jl, &saved);   // внутри ставит фото в очередь ИИ
        camera_release();
        kb = (unsigned)(saved / 1024);
    } while (0);

    if (tmp_here && temp_buf) { free(temp_buf); temp_buf = nullptr; }
    if (inited_here) camera_deinit();

    cap_kb = kb;
    cap_stage = ok ? 3 : 4;
    cap_task_alive = false;
    Serial.printf("cap: done ok=%d (%u KB)\n", ok ? 1 : 0, kb);
    vTaskDelete(nullptr);
}

bool camera_app_capture_start() {
    if (cap_task_alive || parent_ref) return false;   // уже идёт / камера открыта
    cap_task_alive = true;
    cap_kb = 0;
    cap_stage = 0;
    // 16 КБ стека — запас на AF + декод UXGA + JPEGENC с большим разбором
    if (xTaskCreatePinnedToCore(cap_task_fn, "cam_cap", 16384, nullptr,
                                2, nullptr, 1) != pdPASS) {
        cap_task_alive = false;
        cap_stage = -1;
        Serial.println("cap: task create failed");
        return false;
    }
    return true;
}

int camera_app_capture_poll(unsigned* kb) {
    if (kb) *kb = cap_kb;
    return cap_stage;
}

void camera_app_open(lv_obj_t* parent) {
    // Фоновый снимок из чата «ИИ» ещё идёт — двойной camera_init = паника.
    // Редкий случай: подождать окончания (снимок ≤ ~15 с).
    uint32_t t0 = millis();
    while (cap_task_alive && millis() - t0 < 15000) delay(10);
    if (cap_task_alive) {
        Serial.println("cam open: capture still running, abort");
        return;
    }

    parent_ref = parent;
    preview_active = false;
    saving = false;
    cam_brightness = 0;
    cam_focus = 512;
    preview_fill = true;

    lv_obj_set_style_bg_color(parent, lv_color_hex(0x000000), 0);

    // Canvas — во весь экран (видоискатель без шапки).
    // Создаётся ДО статуса/плашек — они лежат поверх (z-order LVGL).
    // Два буфера: canvas_buf (показывается) и work_buf (пишет воркер) —
    // они меняются местами без копирования.
    canvas_stride = lv_draw_buf_width_to_stride(IMG_W, LV_COLOR_FORMAT_RGB565);
    size_t canvas_size = canvas_stride * IMG_H;
    canvas_buf = (uint8_t*)ps_malloc(canvas_size);
    if (!canvas_buf) canvas_buf = (uint8_t*)malloc(canvas_size);
    work_buf = (uint8_t*)ps_malloc(canvas_size);
    if (!work_buf) work_buf = (uint8_t*)malloc(canvas_size);
    if (!canvas_buf || !work_buf) {
        // Статуса ещё нет — просто выходим
        if (canvas_buf) { free(canvas_buf); canvas_buf = nullptr; }
        if (work_buf) { free(work_buf); work_buf = nullptr; }
        return;
    }
    memset(canvas_buf, 0, canvas_size);
    memset(work_buf, 0, canvas_size);

    canvas = lv_canvas_create(parent);
    lv_canvas_set_buffer(canvas, canvas_buf, IMG_W, IMG_H, LV_COLOR_FORMAT_RGB565);
    lv_obj_align(canvas, LV_ALIGN_TOP_MID, 0, 0);

    // Status — тёмная «таблетка» поверх картинки (читается на любом кадре)
    lbl_status = lv_label_create(parent);
    lv_label_set_text_fmt(lbl_status, "%s %s",
                          LV_SYMBOL_REFRESH, lang_str_camera_init());
    lv_obj_set_style_text_color(lbl_status, lv_color_white(), 0);
    lv_obj_set_style_text_font(lbl_status, &lv_font_cyr_14, 0);
    lv_obj_set_style_bg_color(lbl_status, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(lbl_status, LV_OPA_60, 0);
    lv_obj_set_style_radius(lbl_status, 8, 0);
    lv_obj_set_style_pad_all(lbl_status, 4, 0);
    lv_obj_align(lbl_status, LV_ALIGN_TOP_MID, 0, 8);

    // Init camera
    if (!camera_init()) {
        lv_label_set_text_fmt(lbl_status, "%s %s",
                              LV_SYMBOL_WARNING, lang_str_camera_failed());
        return;
    }

    // Буфер превью и метрики AF: 1/4 UXGA = 400x300 (чётко для текста)
    temp_buf = (uint8_t*)ps_malloc(AF_W * AF_H * 2);
    if (!temp_buf) {
        lv_label_set_text_fmt(lbl_status, "%s %s",
                              LV_SYMBOL_WARNING, "No PSRAM");
        return;
    }

    preview_active = true;

    // Автофокус при открытии: ~2-2.5 с, статус виден сразу
    lv_label_set_text_fmt(lbl_status, "%s %s",
                          LV_SYMBOL_REFRESH, lang_str_camera_focusing());
    lv_refr_now(lv_display_get_default());
    cam_focus = run_autofocus(cam_focus);

    lv_label_set_text_fmt(lbl_status, "%s %s  |  B:%d F:%d",
                          LV_SYMBOL_IMAGE, lang_str_camera_ready(),
                          cam_brightness, cam_focus);

    // Воркер декода превью на втором ядре — LVGL не блокируется
    pv_stop = false;
    xTaskCreatePinnedToCore(pv_task_fn, "cam_prev", 8192, nullptr, 1, &pv_task, 0);

    // 100ms timer — предпросмотр; декод идёт в воркере, UI не тормозит
    refresh_timer = lv_timer_create(refresh_cb, 100, nullptr);
}

void camera_app_close() {
    if (refresh_timer) {
        lv_timer_del(refresh_timer);
        refresh_timer = nullptr;
    }

    // Остановить воркер превью и дождаться его выхода (fb и temp_buf
    // принадлежат ему, деинициализация камеры — только после остановки)
    pv_stop = true;
    if (pv_task) {
        xTaskNotifyGive(pv_task);
        uint32_t t0 = millis();
        while (pv_task && millis() - t0 < 1500) {
            delay(10);
        }
    }
    pv_stop = false;

    camera_deinit();

    if (canvas_buf) { free(canvas_buf); canvas_buf = nullptr; }
    if (work_buf) { free(work_buf); work_buf = nullptr; }
    if (temp_buf) { free(temp_buf); temp_buf = nullptr; }

    parent_ref = nullptr;
    lbl_status = nullptr;
    canvas = nullptr;
    toast = nullptr;
    toast_lbl = nullptr;
    preview_active = false;
    saving = false;
}

// Вернуть предпросмотр и убрать плашку через delay_ms
static void schedule_resume(uint32_t delay_ms) {
    lv_timer_t* t = lv_timer_create([](lv_timer_t* timer) {
        lv_timer_del(timer);
        toast_hide();
        preview_active = true;
        saving = false;
        if (lbl_status) {
            lv_label_set_text_fmt(lbl_status, "%s %s  |  B:%d F:%d",
                                  LV_SYMBOL_IMAGE, lang_str_camera_ready(),
                                  cam_brightness, cam_focus);
        }
    }, delay_ms, nullptr);
    (void)t;
}

void camera_app_button(int button_id, int event) {
    // Долгое нажатие любой кнопки навигации (OK-long перехватывает main —
    // выход из приложения): переключение режима превью
    // «во всю ширину» (кроп сверху/снизу) ⇄ «весь кадр» (поля по бокам).
    if (event == BTN_EVENT_LONG_PRESSED) {
        if (button_id == BTN_ID_OK) return;
        if (saving) return;
        preview_fill = !preview_fill;
        toast_set(preview_fill ? lang_str_camera_mode_fill()
                               : lang_str_camera_mode_fit(),
                  lv_color_hex(0x3A3A3A));
        schedule_resume(1500);
        return;
    }

    if (event != BTN_EVENT_CLICKED) return;

    if (button_id == BTN_ID_OK) {
        if (saving) return;

        preview_active = false;
        saving = true;

        // Дождаться воркера: он держит fb камеры и использует temp_buf
        wait_preview_idle();

        // 1) Сначала автофокус — чтобы текст в файле был читаем.
        //    Плашка появляется мгновенно: видно, что нажатие сработало.
        toast_set(lang_str_camera_focusing(), lv_color_hex(0x2C6FBF));
        lv_label_set_text_fmt(lbl_status, "%s %s",
                              LV_SYMBOL_REFRESH, lang_str_camera_focusing());
        lv_refr_now(lv_display_get_default());
        cam_focus = run_autofocus(cam_focus);

        // 2) Плашка «Снято» — фото будет сохранено
        toast_set(lang_str_camera_captured(), lv_color_hex(0x1B7F3B));
        lv_label_set_text_fmt(lbl_status, "%s %s",
                              LV_SYMBOL_REFRESH, lang_str_camera_captured());
        lv_refr_now(lv_display_get_default());

        uint8_t* jpeg_buf = nullptr;
        size_t jpeg_len = 0;

        if (!camera_capture(&jpeg_buf, &jpeg_len)) {
            lv_label_set_text_fmt(lbl_status, "%s %s",
                                  LV_SYMBOL_WARNING, lang_str_camera_no_camera());
            toast_set(lang_str_camera_no_camera(), lv_color_hex(0xB33A3A));
            schedule_resume(1500);
            return;
        }

        // Показать захват на экране (1/4 scale, как предпросмотр)
        int dec_w = 0, dec_h = 0;
        if (jpeg_decode_to_rgb565(jpeg_buf, jpeg_len,
                                  temp_buf, AF_W, AF_H, AF_W * 2, 4,
                                  &dec_w, &dec_h)) {
            if (dec_w > 0 && dec_h > 0) {
                scale_image((uint16_t*)temp_buf, dec_w, dec_h,
                            (uint16_t*)canvas_buf, IMG_W, IMG_H, canvas_stride);
                lv_obj_invalidate(canvas);
            }
        }

        // Сохранить: декодировать + транспонировать (как предпросмотр)
        // + закодировать в JPEG. Полное разрешение, несколько секунд.
        lv_label_set_text_fmt(lbl_status, "%s ...", LV_SYMBOL_REFRESH);

        size_t saved_size = 0;
        if (save_photo(jpeg_buf, jpeg_len, &saved_size)) {
            char t[64];
            snprintf(t, sizeof(t), "%s (%u KB)",
                     lang_str_camera_captured(), (unsigned)(saved_size / 1024));
            toast_set(t, lv_color_hex(0x1B7F3B));
            lv_label_set_text_fmt(lbl_status, "%s OK! (%u KB)",
                                  LV_SYMBOL_OK, (unsigned)(saved_size / 1024));
        } else {
            toast_set(lang_str_camera_error(), lv_color_hex(0xB33A3A));
            lv_label_set_text_fmt(lbl_status, "%s %s",
                                  LV_SYMBOL_WARNING, lang_str_camera_error());
        }

        camera_release();
        schedule_resume(2000);

    } else if (button_id == BTN_ID_UP) {
        // Яркость +
        if (cam_brightness < 2) {
            cam_brightness++;
            camera_set_brightness(cam_brightness);
            if (lbl_status)
                lv_label_set_text_fmt(lbl_status, "B:%d F:%d",
                                      cam_brightness, cam_focus);
        }
    } else if (button_id == BTN_ID_DOWN) {
        // Яркость -
        if (cam_brightness > -2) {
            cam_brightness--;
            camera_set_brightness(cam_brightness);
            if (lbl_status)
                lv_label_set_text_fmt(lbl_status, "B:%d F:%d",
                                      cam_brightness, cam_focus);
        }
    } else if (button_id == BTN_ID_RIGHT) {
        // Фокус — ближе (increase value)
        if (cam_focus < 1023) {
            cam_focus += 64;
            if (cam_focus > 1023) cam_focus = 1023;
            camera_set_focus(cam_focus);
            lv_label_set_text_fmt(lbl_status, "B:%d F:%d",
                                  cam_brightness, cam_focus);
        }
    } else if (button_id == BTN_ID_LEFT) {
        // Фокус — дальше (decrease value)
        if (cam_focus > 0) {
            cam_focus -= 64;
            if (cam_focus < 0) cam_focus = 0;
            camera_set_focus(cam_focus);
            lv_label_set_text_fmt(lbl_status, "B:%d F:%d",
                                  cam_brightness, cam_focus);
        }
    }
}
