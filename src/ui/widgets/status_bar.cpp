#include "fonts/fonts.h"
#include "ui/widgets/status_bar.h"
#include "ui/theme.h"
#include <WiFi.h>

static lv_obj_t* bar_obj = nullptr;
static lv_obj_t* lbl_wifi = nullptr;
static lv_obj_t* lbl_time = nullptr;
static bool wifi_connected = false;

void status_bar_create(lv_obj_t* parent) {
    bar_obj = lv_obj_create(parent);
    lv_obj_set_size(bar_obj, LV_PCT(100), 28);
    lv_obj_set_style_bg_color(bar_obj, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(bar_obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bar_obj, 0, 0);
    lv_obj_set_style_radius(bar_obj, 0, 0);
    lv_obj_set_style_pad_hor(bar_obj, 8, 0);
    lv_obj_set_flex_flow(bar_obj, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar_obj, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lbl_wifi = lv_label_create(bar_obj);
    lv_label_set_text(lbl_wifi, LV_SYMBOL_WIFI);
    lv_obj_set_style_text_color(lbl_wifi, theme_color_text_muted(), 0);
    lv_obj_set_style_text_font(lbl_wifi, &lv_font_cyr_14, 0);

    lbl_time = lv_label_create(bar_obj);
    lv_label_set_text(lbl_time, "");
    lv_obj_set_style_text_color(lbl_time, theme_color_text(), 0);
    lv_obj_set_style_text_font(lbl_time, &lv_font_cyr_12, 0);
}

void status_bar_set_wifi(bool connected) {
    wifi_connected = connected;
    if (lbl_wifi) {
        lv_obj_set_style_text_color(lbl_wifi,
            connected ? theme_color_accent() : theme_color_text_muted(), 0);
    }
}

void status_bar_set_time(const char* time_str) {
    if (lbl_time) {
        lv_label_set_text(lbl_time, time_str);
    }
}

void status_bar_update() {
    if (!wifi_connected && WiFi.status() == WL_CONNECTED) {
        status_bar_set_wifi(true);
    } else if (wifi_connected && WiFi.status() != WL_CONNECTED) {
        status_bar_set_wifi(false);
    }
}
