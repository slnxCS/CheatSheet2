#include "fonts/fonts.h"
#include "ui/screens/splash_screen.h"
#include "ui/theme.h"
#include <Arduino.h>

static lv_obj_t* splash_obj = nullptr;
static lv_obj_t* label_title = nullptr;
static lv_obj_t* label_sub = nullptr;
static lv_obj_t* spinner = nullptr;
static uint32_t splash_start = 0;

void splash_screen_create() {
    splash_obj = lv_obj_create(lv_screen_active());
    lv_obj_set_size(splash_obj, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(splash_obj, theme_color_bg(), 0);
    lv_obj_set_style_bg_opa(splash_obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(splash_obj, 0, 0);
    lv_obj_set_style_pad_all(splash_obj, 0, 0);
    lv_obj_center(splash_obj);

    label_title = lv_label_create(splash_obj);
    lv_label_set_text(label_title, "CheatSheet");
    lv_obj_set_style_text_font(label_title, &lv_font_cyr_24, 0);
    lv_obj_set_style_text_color(label_title, lv_color_white(), 0);
    lv_obj_align(label_title, LV_ALIGN_CENTER, 0, -30);

    label_sub = lv_label_create(splash_obj);
    lv_label_set_text(label_sub, "ESP32-S3 CAM");
    lv_obj_set_style_text_font(label_sub, &lv_font_cyr_12, 0);
    lv_obj_set_style_text_color(label_sub, lv_color_white(), 0);
    lv_obj_align(label_sub, LV_ALIGN_CENTER, 0, 5);

    spinner = lv_spinner_create(splash_obj);
    lv_spinner_set_anim_params(spinner, 1000, 200);
    lv_obj_set_size(spinner, 40, 40);
    lv_obj_align(spinner, LV_ALIGN_CENTER, 0, 40);
    lv_obj_set_style_arc_color(spinner, lv_color_white(), LV_PART_INDICATOR);

    splash_start = millis();
}

bool splash_screen_update() {
    return (millis() - splash_start) > 1500;
}

void splash_screen_destroy() {
    if (splash_obj) {
        lv_obj_delete(splash_obj);
        splash_obj = nullptr;
    }
}
