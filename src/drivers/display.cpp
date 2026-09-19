#include "drivers/display.h"
#include "config/pins.h"
#include <Arduino.h>

#include <TFT_eSPI.h>

static TFT_eSPI tft;
static lv_display_t* lvgl_display = nullptr;

static lv_color_t* buf1 = nullptr;
static lv_color_t* buf2 = nullptr;
static uint8_t* swap_buf = nullptr;

static void flush_cb(lv_display_t* disp, const lv_area_t* area, uint8_t* px_map) {
    uint32_t w = area->x2 - area->x1 + 1;
    uint32_t h = area->y2 - area->y1 + 1;
    uint32_t len = w * h;

    for (uint32_t i = 0; i < len; i++) {
        swap_buf[i * 2]     = px_map[i * 2 + 1];
        swap_buf[i * 2 + 1] = px_map[i * 2];
    }

    tft.startWrite();
    tft.setAddrWindow(area->x1, area->y1, w, h);
    tft.pushColors((uint16_t*)swap_buf, len, false);
    tft.endWrite();

    lv_display_flush_ready(disp);
}

void display_init() {
    Serial.println("[DISPLAY] Starting init...");

    Serial.println("[DISPLAY] tft.begin()...");
    tft.begin();
    Serial.println("[DISPLAY] tft.setRotation()...");
    tft.setRotation(1);
    Serial.println("[DISPLAY] tft.fillScreen()...");
    tft.fillScreen(TFT_BLACK);
    Serial.println("[DISPLAY] tft.initDMA()...");
    tft.initDMA();
    Serial.println("[DISPLAY] TFT OK");

    Serial.println("[DISPLAY] lv_init()...");
    lv_init();
    Serial.println("[DISPLAY] LVGL OK");

    Serial.printf("[DISPLAY] Creating display %dx%d (landscape)...\n", TFT_HEIGHT, TFT_WIDTH);
    lvgl_display = lv_display_create(TFT_HEIGHT, TFT_WIDTH);
    lv_display_set_flush_cb(lvgl_display, flush_cb);

    size_t buf_size = TFT_HEIGHT * 20 * sizeof(lv_color_t);
    Serial.printf("[DISPLAY] Allocating %u bytes x2 for LVGL buffers...\n", (unsigned)buf_size);

    buf1 = (lv_color_t*)ps_malloc(buf_size);
    if (buf1) {
        Serial.println("[DISPLAY] buf1: PSRAM OK");
    } else {
        Serial.println("[DISPLAY] buf1: PSRAM failed, trying heap...");
        buf1 = (lv_color_t*)malloc(buf_size);
        Serial.printf("[DISPLAY] buf1: heap %s\n", buf1 ? "OK" : "FAILED");
    }

    buf2 = (lv_color_t*)ps_malloc(buf_size);
    if (buf2) {
        Serial.println("[DISPLAY] buf2: PSRAM OK");
    } else {
        Serial.println("[DISPLAY] buf2: PSRAM failed, trying heap...");
        buf2 = (lv_color_t*)malloc(buf_size);
        Serial.printf("[DISPLAY] buf2: heap %s\n", buf2 ? "OK" : "FAILED");
    }

    if (buf1 && buf2) {
        Serial.println("[DISPLAY] Using double buffer");
        lv_display_set_buffers(lvgl_display, buf1, buf2, buf_size, LV_DISPLAY_RENDER_MODE_PARTIAL);
    } else if (buf1) {
        Serial.println("[DISPLAY] Using single buffer");
        lv_display_set_buffers(lvgl_display, buf1, nullptr, buf_size, LV_DISPLAY_RENDER_MODE_PARTIAL);
    } else {
        Serial.println("[DISPLAY] WARNING: No buffers! LVGL may crash!");
    }

    lv_display_set_color_format(lvgl_display, LV_COLOR_FORMAT_RGB565);
    Serial.println("[DISPLAY] LVGL display configured");

    swap_buf = (uint8_t*)ps_malloc(buf_size);
    if (!swap_buf) swap_buf = (uint8_t*)malloc(buf_size);
    Serial.printf("[DISPLAY] swap_buf: %s\n", swap_buf ? "OK" : "FAILED");

    display_set_brightness(80);
    Serial.println("[DISPLAY] Init complete!");
}

void display_set_brightness(uint8_t percent) {
    if (percent > 100) percent = 100;
#if PIN_TFT_BL >= 0
    ledcSetup(0, 5000, 8);
    ledcAttachPin(PIN_TFT_BL, 0);
    ledcWrite(0, (uint32_t)percent * 255 / 100);
#endif
}

lv_display_t* display_get_lvgl() {
    return lvgl_display;
}
