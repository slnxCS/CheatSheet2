#include "fonts/fonts.h"
#include "apps/builtin/camera_app.h"
#include "drivers/camera_driver.h"
#include "drivers/jpeg_decoder.h"
#include "drivers/input.h"
#include "services/lang_service.h"
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
#define HEADER_H 36
#define IMG_W SCREEN_W
#define IMG_H (SCREEN_H - HEADER_H)  // 204

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

// Транспонирование (оси X/Y swapped — сенсор физически повёрнут) +
// вписывание с сохранением пропорций. Предпросмотр теперь совпадает
// с сохранённым фото 1 к 1 (чёрные поля по краям).
static void scale_image(const uint16_t* src, int src_w, int src_h,
                        uint16_t* dst, int dst_w, int dst_h, uint32_t dst_stride) {
    if (src_w <= 0 || src_h <= 0 || dst_w <= 0 || dst_h <= 0) return;

    // Очистить весь canvas — чёрные поля
    memset(dst, 0, (size_t)dst_stride * dst_h);

    // Транспонированный размер: ширина = высота src, высота = ширина src
    int t_w = src_h;
    int t_h = src_w;

    // Вписать в dst с сохранением пропорций (fixed point 16.16)
    int32_t scale = ((int32_t)dst_w << 16) / t_w;
    int32_t scale_h = ((int32_t)dst_h << 16) / t_h;
    if (scale_h < scale) scale = scale_h;

    int out_w = (int)(((int64_t)t_w * scale) >> 16);
    int out_h = (int)(((int64_t)t_h * scale) >> 16);
    if (out_w < 1) out_w = 1;
    if (out_h < 1) out_h = 1;

    int x0 = (dst_w - out_w) / 2;
    int y0 = (dst_h - out_h) / 2;

    // Строка dst ↔ столбец src, столбец dst ↔ строка src (транспонирование)
    int32_t sx_step = ((int32_t)src_w << 16) / out_h;   // dy → src_x
    int32_t sy_step = ((int32_t)src_h << 16) / out_w;   // dx → src_y

    for (int dy = 0; dy < out_h; dy++) {
        uint16_t* dst_row = (uint16_t*)((uint8_t*)dst + (size_t)(y0 + dy) * dst_stride) + x0;
        int32_t sx_fixed = dy * sx_step;

        for (int dx = 0; dx < out_w; dx++) {
            int32_t src_x = sx_fixed >> 16;
            int32_t src_y = ((int32_t)dx * sy_step) >> 16;

            if (src_x >= src_w) src_x = src_w - 1;
            if (src_y >= src_h) src_y = src_h - 1;

            dst_row[dx] = src[src_y * src_w + src_x];
        }
    }
}

// --- Preview timer callback ---
static void refresh_cb(lv_timer_t* timer) {
    if (!parent_ref || !lbl_status || !preview_active || !canvas || !canvas_buf) return;

    uint8_t* jpeg_buf = nullptr;
    size_t jpeg_len = 0;

    if (!camera_capture(&jpeg_buf, &jpeg_len)) {
        lv_label_set_text_fmt(lbl_status, "%s %s",
                              LV_SYMBOL_WARNING, lang_str_camera_no_camera());
        return;
    }

    // Preview: EIGHTH scale of 1600x1200 = 200x150 (быстро)
    int dec_w = 0, dec_h = 0;
    if (jpeg_decode_to_rgb565(jpeg_buf, jpeg_len,
                              temp_buf, 200, 150, 200 * 2, 8,
                              &dec_w, &dec_h)) {
        if (dec_w > 0 && dec_h > 0) {
            scale_image((uint16_t*)temp_buf, dec_w, dec_h,
                        (uint16_t*)canvas_buf, IMG_W, IMG_H, canvas_stride);
            lv_obj_invalidate(canvas);
        }
    }

    camera_release();
}

// --- JPEGENC: запись напрямую в открытый File ---
static File* enc_file = nullptr;

static void* enc_open_cb(const char* /*name*/) {
    return enc_file;  // File уже открыт до вызова JPEGENC::open()
}
static int32_t enc_write_cb(JPEGE_FILE* f, uint8_t* buf, int32_t len) {
    File* fp = (File*)f->fHandle;
    return fp ? (int32_t)fp->write(buf, (size_t)len) : 0;
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

    // Перекодированный файл может оказаться больше исходного
    if (free_bytes < jpeg_len * 2 + 65536) {
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
    if (jpg.open(path, enc_open_cb, nullptr, enc_read_cb,
                 enc_write_cb, enc_seek_cb) == JPEGE_SUCCESS) {
        if (jpg.encodeBegin(&enc, out_w, out_h, JPEGE_PIXEL_RGB565,
                            JPEGE_SUBSAMPLE_420, JPEGE_Q_HIGH) == JPEGE_SUCCESS) {
            jpg.addFrame(&enc, (uint8_t*)tbuf, out_w * 2);
            int32_t total = jpg.close();
            ok = total > 0;
            if (ok && out_size) *out_size = (size_t)total;
        }
    }
    enc_file = nullptr;
    f.close();
    free(tbuf);

    Serial.printf("Saved %s (%s, %dx%d)\n", path, ok ? "ok" : "fail", out_w, out_h);
    return ok;
}

void camera_app_open(lv_obj_t* parent) {
    parent_ref = parent;
    preview_active = false;
    saving = false;
    cam_brightness = 0;
    cam_focus = 512;

    lv_obj_set_style_bg_color(parent, lv_color_hex(0x0A0A1A), 0);

    // Header
    lv_obj_t* header = lv_obj_create(parent);
    lv_obj_set_size(header, LV_PCT(100), 32);
    lv_obj_set_style_bg_color(header, theme_color_panel(), 0);
    lv_obj_set_style_bg_opa(header, LV_OPA_80, 0);
    lv_obj_set_style_border_width(header, 0, 0);
    lv_obj_set_style_radius(header, 0, 0);
    lv_obj_align(header, LV_ALIGN_TOP_MID, 0, 0);

    lv_obj_t* lbl_title = lv_label_create(header);
    lv_label_set_text_fmt(lbl_title, "%s %s",
                          LV_SYMBOL_IMAGE, lang_str_camera_title());
    lv_obj_set_style_text_color(lbl_title, theme_color_text(), 0);
    lv_obj_set_style_text_font(lbl_title, &lv_font_cyr_14, 0);
    lv_obj_align(lbl_title, LV_ALIGN_CENTER, 0, 0);

    // Status
    lbl_status = lv_label_create(parent);
    lv_label_set_text_fmt(lbl_status, "%s %s",
                          LV_SYMBOL_REFRESH, lang_str_camera_init());
    lv_obj_set_style_text_color(lbl_status, theme_color_text_muted(), 0);
    lv_obj_set_style_text_font(lbl_status, &lv_font_cyr_14, 0);
    lv_obj_align(lbl_status, LV_ALIGN_TOP_MID, 0, 38);

    // Init camera
    if (!camera_init()) {
        lv_label_set_text_fmt(lbl_status, "%s %s",
                              LV_SYMBOL_WARNING, lang_str_camera_failed());
        return;
    }

    // Pre-allocate temp decode buffer for EIGHTH of UXGA (200x150)
    temp_buf = (uint8_t*)ps_malloc(200 * 150 * 2);
    if (!temp_buf) {
        lv_label_set_text_fmt(lbl_status, "%s %s",
                              LV_SYMBOL_WARNING, "No PSRAM");
        return;
    }

    // Allocate canvas buffer with LVGL stride
    canvas_stride = lv_draw_buf_width_to_stride(IMG_W, LV_COLOR_FORMAT_RGB565);
    size_t canvas_size = canvas_stride * IMG_H;
    canvas_buf = (uint8_t*)ps_malloc(canvas_size);
    if (!canvas_buf) canvas_buf = (uint8_t*)malloc(canvas_size);
    if (!canvas_buf) {
        lv_label_set_text_fmt(lbl_status, "%s %s",
                              LV_SYMBOL_WARNING, "No memory");
        return;
    }
    memset(canvas_buf, 0, canvas_size);

    canvas = lv_canvas_create(parent);
    lv_canvas_set_buffer(canvas, canvas_buf, IMG_W, IMG_H, LV_COLOR_FORMAT_RGB565);
    lv_obj_align(canvas, LV_ALIGN_TOP_MID, 0, HEADER_H + 2);

    preview_active = true;
    lv_label_set_text_fmt(lbl_status, "%s %s  |  B:%d F:%d",
                          LV_SYMBOL_IMAGE, lang_str_camera_ready(),
                          cam_brightness, cam_focus);

    // 100ms timer = ~10 FPS preview
    refresh_timer = lv_timer_create(refresh_cb, 100, nullptr);
}

void camera_app_close() {
    if (refresh_timer) {
        lv_timer_del(refresh_timer);
        refresh_timer = nullptr;
    }
    camera_deinit();

    if (canvas_buf) { free(canvas_buf); canvas_buf = nullptr; }
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
    if (event != BTN_EVENT_CLICKED) return;

    if (button_id == BTN_ID_OK) {
        if (saving) return;

        preview_active = false;
        saving = true;

        // Плашка появляется сразу при нажатии — видно, что фото снято
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

        // Показать захват на экране (EIGHTH scale, вписан 1:1)
        int dec_w = 0, dec_h = 0;
        if (jpeg_decode_to_rgb565(jpeg_buf, jpeg_len,
                                  temp_buf, 200, 150, 200 * 2, 8,
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
