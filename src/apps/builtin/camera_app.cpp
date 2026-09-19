#include "fonts/fonts.h"
#include "apps/builtin/camera_app.h"
#include "drivers/camera_driver.h"
#include "drivers/jpeg_decoder.h"
#include "drivers/input.h"
#include "services/lang_service.h"
#include "services/storage_service.h"
#include "ui/theme.h"
#include <Arduino.h>
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

static bool preview_active = false;
static bool saving = false;

// Rotate -90° + mirror + scale: src(src_w×src_h) → dst(dst_w×dst_h)
static void rotate_scale(const uint16_t* src, int src_w, int src_h,
                          uint16_t* dst, int dst_w, int dst_h, uint32_t dst_stride) {
    if (src_w <= 0 || src_h <= 0 || dst_w <= 0 || dst_h <= 0) return;

    int32_t sy_step = ((int32_t)src_h << 16) / dst_w;
    int32_t sx_step = ((int32_t)src_w << 16) / dst_h;

    for (int dy = 0; dy < dst_h; dy++) {
        uint16_t* dst_row = (uint16_t*)((uint8_t*)dst + dy * dst_stride);
        int32_t sx_fixed = dy * sx_step;

        for (int dx = 0; dx < dst_w; dx++) {
            int32_t sy_fixed = dx * sy_step;

            int32_t src_x = src_w - 1 - (sx_fixed >> 16);
            int32_t src_y = src_h - 1 - (sy_fixed >> 16);

            if (src_x < 0) src_x = 0;
            if (src_x >= src_w) src_x = src_w - 1;
            if (src_y < 0) src_y = 0;
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

    // Fast preview: QUARTER scale
    int dec_w = 0, dec_h = 0;
    if (jpeg_decode_to_rgb565(jpeg_buf, jpeg_len,
                              temp_buf, 200, 150, 200 * 2, 4,
                              &dec_w, &dec_h)) {
        if (dec_w > 0 && dec_h > 0) {
            rotate_scale((uint16_t*)temp_buf, dec_w, dec_h,
                          (uint16_t*)canvas_buf, IMG_W, IMG_H, canvas_stride);
            lv_obj_invalidate(canvas);
        }
    }

    camera_release();
}

static bool save_photo(const uint8_t* jpeg_data, size_t jpeg_len) {
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

    if (free_bytes < jpeg_len + 4096) {
        Serial.printf("not enough space (%u free, need %u)\n",
                      (unsigned)free_bytes, (unsigned)jpeg_len);
        return false;
    }

    time_t now = time(nullptr);
    struct tm* t = localtime(&now);
    char path[64];
    snprintf(path, sizeof(path), "/images/%04d%02d%02d_%02d%02d%02d.jpg",
             t->tm_year + 1900, t->tm_mon + 1, t->tm_mday,
             t->tm_hour, t->tm_min, t->tm_sec);

    File f = fs->open(path, FILE_WRITE);
    if (!f) return false;
    size_t written = f.write(jpeg_data, jpeg_len);
    f.close();

    Serial.printf("Saved %s (%u bytes)\n", path, (unsigned)written);

    return written == jpeg_len;
}

void camera_app_open(lv_obj_t* parent) {
    parent_ref = parent;
    preview_active = false;
    saving = false;

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

    // Pre-allocate temp decode buffer for SVGA full-res (800×600)
    temp_buf = (uint8_t*)ps_malloc(800 * 600 * 2);
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
    lv_label_set_text_fmt(lbl_status, "%s %s",
                          LV_SYMBOL_IMAGE, lang_str_camera_ready());

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
    preview_active = false;
    saving = false;
}

void camera_app_button(int button_id, int event) {
    if (event != BTN_EVENT_CLICKED) return;

    if (button_id == BTN_ID_OK) {
        if (saving) return;

        preview_active = false;
        saving = true;
        lv_label_set_text_fmt(lbl_status, "%s %s",
                              LV_SYMBOL_REFRESH, lang_str_camera_captured());

        uint8_t* jpeg_buf = nullptr;
        size_t jpeg_len = 0;

        if (!camera_capture(&jpeg_buf, &jpeg_len)) {
            lv_label_set_text_fmt(lbl_status, "%s %s",
                                  LV_SYMBOL_WARNING, lang_str_camera_no_camera());
            saving = false;
            preview_active = true;
            return;
        }

        // Show captured image at HALF resolution (400×300 — enough for text)
        int dec_w = 0, dec_h = 0;
        if (jpeg_decode_to_rgb565(jpeg_buf, jpeg_len,
                                  temp_buf, 400, 300, 400 * 2, 2,
                                  &dec_w, &dec_h)) {
            if (dec_w > 0 && dec_h > 0) {
                rotate_scale((uint16_t*)temp_buf, dec_w, dec_h,
                              (uint16_t*)canvas_buf, IMG_W, IMG_H, canvas_stride);
                lv_obj_invalidate(canvas);
            }
        }

        // Save JPEG to flash
        if (save_photo(jpeg_buf, jpeg_len)) {
            lv_label_set_text_fmt(lbl_status, "%s OK!", LV_SYMBOL_OK);
        } else {
            lv_label_set_text_fmt(lbl_status, "%s %s",
                                  LV_SYMBOL_WARNING, "Save failed");
        }

        camera_release();

        // Resume preview after 2 seconds
        lv_timer_t* resume_timer = lv_timer_create([](lv_timer_t* t) {
            lv_timer_del(t);
            preview_active = true;
            saving = false;
            if (lbl_status) {
                lv_label_set_text_fmt(lbl_status, "%s %s",
                                      LV_SYMBOL_IMAGE, lang_str_camera_ready());
            }
        }, 2000, nullptr);
    }
}
