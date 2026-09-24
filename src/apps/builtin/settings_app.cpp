#include "drivers/input.h"
#include "fonts/fonts.h"
#include "apps/builtin/settings_app.h"
#include "services/lang_service.h"
#include "services/storage_service.h"
#include "services/wifi_service.h"
#include "ui/theme.h"
#include "drivers/display.h"
#include <cstdio>

static lv_obj_t* parent_ref = nullptr;
static lv_obj_t* slider_brightness = nullptr;
static lv_obj_t* lbl_brightness_val = nullptr;
static lv_obj_t* lbl_lang_val = nullptr;
static lv_obj_t* row_brightness = nullptr;
static lv_obj_t* row_lang = nullptr;
static lv_obj_t* row_storage = nullptr;
static lv_obj_t* lbl_storage_val = nullptr;
static lv_obj_t* row_wifi = nullptr;
static lv_obj_t* lbl_wifi_val = nullptr;
static int current_item = 0;
static int current_storage = 0; // 0 = Flash, 1 = SD Card (loaded from NVS)
static bool current_wifi = false;
static const int ITEM_COUNT = 4;

static lv_obj_t* row_for_item(int i) {
    switch (i) {
        case 0:  return row_brightness;
        case 1:  return row_lang;
        case 2:  return row_storage;
        case 3:  return row_wifi;
        default: return nullptr;
    }
}

static void highlight_items() {
    if (row_brightness) {
        lv_obj_set_style_bg_opa(row_brightness,
            (current_item == 0) ? LV_OPA_20 : LV_OPA_TRANSP, 0);
    }
    if (row_lang) {
        lv_obj_set_style_bg_opa(row_lang,
            (current_item == 1) ? LV_OPA_20 : LV_OPA_TRANSP, 0);
    }
    if (row_storage) {
        lv_obj_set_style_bg_opa(row_storage,
            (current_item == 2) ? LV_OPA_20 : LV_OPA_TRANSP, 0);
    }
    if (row_wifi) {
        lv_obj_set_style_bg_opa(row_wifi,
            (current_item == 3) ? LV_OPA_20 : LV_OPA_TRANSP, 0);
    }
}

// Экран выше 240px (4 пункта) — прокрутить выбранный пункт на вид.
// Вызывается только из навигации (при открытии layout ещё не посчитан).
static void scroll_to_current() {
    lv_obj_t* row = row_for_item(current_item + 1);
    if (row) lv_obj_scroll_to_view(row, LV_ANIM_ON);
}

void settings_app_button(int button_id, int event) {
    if (event != BTN_EVENT_CLICKED) return;

    if (button_id == BTN_ID_UP) {
        current_item--;
        if (current_item < 0) current_item = ITEM_COUNT - 1;
        highlight_items();
        scroll_to_current();
    } else if (button_id == BTN_ID_DOWN) {
        current_item++;
        if (current_item >= ITEM_COUNT) current_item = 0;
        highlight_items();
        scroll_to_current();
    } else if (button_id == BTN_ID_RIGHT) {
        if (current_item == 0 && slider_brightness) {
            int val = lv_slider_get_value(slider_brightness);
            if (val < 100) val += 5;
            lv_slider_set_value(slider_brightness, val, LV_ANIM_ON);
            char buf[8];
            snprintf(buf, sizeof(buf), "%d%%", val);
            lv_label_set_text(lbl_brightness_val, buf);
            display_set_brightness(val);
        } else if (current_item == 1) {
            lang_set(LANG_RU);
            if (lbl_lang_val)
                lv_label_set_text(lbl_lang_val,
                    lang_str_settings_lang_name(LANG_RU));
        } else if (current_item == 2) {
            current_storage = (current_storage + 1) % 2;
            storage_set(current_storage);
            if (lbl_storage_val)
                lv_label_set_text(lbl_storage_val,
                    lang_str_settings_storage_name(current_storage));
        } else if (current_item == 3) {
            wifi_service_set_enabled(!current_wifi);
            current_wifi = wifi_service_enabled();
            if (lbl_wifi_val)
                lv_label_set_text(lbl_wifi_val,
                    lang_str_settings_wifi_name(current_wifi));
        }
    } else if (button_id == BTN_ID_LEFT) {
        if (current_item == 0 && slider_brightness) {
            int val = lv_slider_get_value(slider_brightness);
            if (val > 10) val -= 5;
            lv_slider_set_value(slider_brightness, val, LV_ANIM_ON);
            char buf[8];
            snprintf(buf, sizeof(buf), "%d%%", val);
            lv_label_set_text(lbl_brightness_val, buf);
            display_set_brightness(val);
        } else if (current_item == 1) {
            lang_set(LANG_EN);
            if (lbl_lang_val)
                lv_label_set_text(lbl_lang_val,
                    lang_str_settings_lang_name(LANG_EN));
        } else if (current_item == 2) {
            current_storage = (current_storage + 1) % 2;
            storage_set(current_storage);
            if (lbl_storage_val)
                lv_label_set_text(lbl_storage_val,
                    lang_str_settings_storage_name(current_storage));
        } else if (current_item == 3) {
            wifi_service_set_enabled(!current_wifi);
            current_wifi = wifi_service_enabled();
            if (lbl_wifi_val)
                lv_label_set_text(lbl_wifi_val,
                    lang_str_settings_wifi_name(current_wifi));
        }
    }
}

static void on_brightness_changed(lv_event_t* e) {
    lv_obj_t* s = (lv_obj_t*)lv_event_get_target(e);
    int val = lv_slider_get_value(s);
    display_set_brightness(val);
    if (lbl_brightness_val) {
        char buf[8];
        snprintf(buf, sizeof(buf), "%d%%", val);
        lv_label_set_text(lbl_brightness_val, buf);
    }
}

void settings_app_open(lv_obj_t* parent) {
    parent_ref = parent;
    current_item = 0;
    current_storage = storage_get(); // Load from NVS
    current_wifi = wifi_service_enabled();

    lv_obj_set_style_bg_color(parent, lv_color_hex(0x0A1628), 0);

    lv_obj_t* header = lv_obj_create(parent);
    lv_obj_set_size(header, LV_PCT(100), 32);
    lv_obj_set_style_bg_color(header, theme_color_panel(), 0);
    lv_obj_set_style_bg_opa(header, LV_OPA_80, 0);
    lv_obj_set_style_border_width(header, 0, 0);
    lv_obj_set_style_radius(header, 0, 0);
    lv_obj_align(header, LV_ALIGN_TOP_MID, 0, 0);

    lv_obj_t* lbl_title = lv_label_create(header);
    lv_label_set_text_fmt(lbl_title, "%s %s",
                          LV_SYMBOL_SETTINGS, lang_str_settings_title());
    lv_obj_set_style_text_color(lbl_title, theme_color_text(), 0);
    lv_obj_set_style_text_font(lbl_title, &lv_font_cyr_14, 0);
    lv_obj_align(lbl_title, LV_ALIGN_CENTER, 0, 0);

    lv_obj_t* section = lv_obj_create(parent);
    lv_obj_set_size(section, LV_PCT(90), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(section, theme_color_panel(), 0);
    lv_obj_set_style_bg_opa(section, LV_OPA_60, 0);
    lv_obj_set_style_border_width(section, 0, 0);
    lv_obj_set_style_radius(section, 12, 0);
    lv_obj_set_style_pad_all(section, 12, 0);
    lv_obj_align(section, LV_ALIGN_TOP_MID, 0, 44);
    lv_obj_set_flex_flow(section, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(section, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    // --- Brightness ---
    lv_obj_t* lbl_br = lv_label_create(section);
    lv_label_set_text_fmt(lbl_br, "%s %s",
                          LV_SYMBOL_IMAGE, lang_str_settings_brightness());
    lv_obj_set_style_text_color(lbl_br, theme_color_text(), 0);
    lv_obj_set_style_text_font(lbl_br, &lv_font_cyr_14, 0);

    row_brightness = lv_obj_create(section);
    lv_obj_set_size(row_brightness, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(row_brightness, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_color(row_brightness, theme_color_accent(), 0);
    lv_obj_set_style_border_width(row_brightness, 0, 0);
    lv_obj_set_style_pad_all(row_brightness, 4, 0);
    lv_obj_set_flex_flow(row_brightness, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row_brightness, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    slider_brightness = lv_slider_create(row_brightness);
    lv_slider_set_range(slider_brightness, 10, 100);
    lv_slider_set_value(slider_brightness, 80, LV_ANIM_OFF);
    lv_obj_set_width(slider_brightness, LV_PCT(65));
    lv_obj_set_style_bg_color(slider_brightness, theme_color_primary(), LV_PART_MAIN);
    lv_obj_set_style_bg_color(slider_brightness, theme_color_accent(), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider_brightness, theme_color_text(), LV_PART_KNOB);
    lv_obj_add_event_cb(slider_brightness, on_brightness_changed,
                        LV_EVENT_VALUE_CHANGED, nullptr);

    lbl_brightness_val = lv_label_create(row_brightness);
    lv_label_set_text(lbl_brightness_val, "80%");
    lv_obj_set_style_text_color(lbl_brightness_val, theme_color_text(), 0);
    lv_obj_set_style_text_font(lbl_brightness_val, &lv_font_cyr_12, 0);

    // Separator
    lv_obj_t* sep1 = lv_obj_create(section);
    lv_obj_set_size(sep1, LV_PCT(100), 1);
    lv_obj_set_style_bg_color(sep1, theme_color_text_muted(), 0);
    lv_obj_set_style_bg_opa(sep1, LV_OPA_30, 0);
    lv_obj_set_style_border_width(sep1, 0, 0);
    lv_obj_set_style_pad_all(sep1, 0, 0);

    // --- Language ---
    lv_obj_t* lbl_lang = lv_label_create(section);
    lv_label_set_text_fmt(lbl_lang, "%s %s",
                          LV_SYMBOL_LOOP, lang_str_settings_language());
    lv_obj_set_style_text_color(lbl_lang, theme_color_text(), 0);
    lv_obj_set_style_text_font(lbl_lang, &lv_font_cyr_14, 0);

    row_lang = lv_obj_create(section);
    lv_obj_set_size(row_lang, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(row_lang, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_color(row_lang, theme_color_accent(), 0);
    lv_obj_set_style_border_width(row_lang, 0, 0);
    lv_obj_set_style_pad_all(row_lang, 4, 0);
    lv_obj_set_flex_flow(row_lang, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row_lang, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lbl_lang_val = lv_label_create(row_lang);
    lv_label_set_text(lbl_lang_val, lang_str_settings_lang_name(lang_get()));
    lv_obj_set_style_text_color(lbl_lang_val, theme_color_accent(), 0);
    lv_obj_set_style_text_font(lbl_lang_val, &lv_font_cyr_14, 0);

    // Separator
    lv_obj_t* sep2 = lv_obj_create(section);
    lv_obj_set_size(sep2, LV_PCT(100), 1);
    lv_obj_set_style_bg_color(sep2, theme_color_text_muted(), 0);
    lv_obj_set_style_bg_opa(sep2, LV_OPA_30, 0);
    lv_obj_set_style_border_width(sep2, 0, 0);
    lv_obj_set_style_pad_all(sep2, 0, 0);

    // --- Storage device ---
    lv_obj_t* lbl_st = lv_label_create(section);
    lv_label_set_text_fmt(lbl_st, "%s %s",
                          LV_SYMBOL_SD_CARD, lang_str_settings_storage());
    lv_obj_set_style_text_color(lbl_st, theme_color_text(), 0);
    lv_obj_set_style_text_font(lbl_st, &lv_font_cyr_14, 0);

    row_storage = lv_obj_create(section);
    lv_obj_set_size(row_storage, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(row_storage, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_color(row_storage, theme_color_accent(), 0);
    lv_obj_set_style_border_width(row_storage, 0, 0);
    lv_obj_set_style_pad_all(row_storage, 4, 0);
    lv_obj_set_flex_flow(row_storage, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row_storage, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lbl_storage_val = lv_label_create(row_storage);
    lv_label_set_text(lbl_storage_val,
                      lang_str_settings_storage_name(current_storage));
    lv_obj_set_style_text_color(lbl_storage_val, theme_color_accent(), 0);
    lv_obj_set_style_text_font(lbl_storage_val, &lv_font_cyr_14, 0);

    // Separator
    lv_obj_t* sep3 = lv_obj_create(section);
    lv_obj_set_size(sep3, LV_PCT(100), 1);
    lv_obj_set_style_bg_color(sep3, theme_color_text_muted(), 0);
    lv_obj_set_style_bg_opa(sep3, LV_OPA_30, 0);
    lv_obj_set_style_border_width(sep3, 0, 0);
    lv_obj_set_style_pad_all(sep3, 0, 0);

    // --- WiFi (SoftAP для связи с телефоном) ---
    lv_obj_t* lbl_wifi_hdr = lv_label_create(section);
    lv_label_set_text_fmt(lbl_wifi_hdr, "%s %s",
                          LV_SYMBOL_WIFI, lang_str_settings_wifi());
    lv_obj_set_style_text_color(lbl_wifi_hdr, theme_color_text(), 0);
    lv_obj_set_style_text_font(lbl_wifi_hdr, &lv_font_cyr_14, 0);

    row_wifi = lv_obj_create(section);
    lv_obj_set_size(row_wifi, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(row_wifi, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_color(row_wifi, theme_color_accent(), 0);
    lv_obj_set_style_border_width(row_wifi, 0, 0);
    lv_obj_set_style_pad_all(row_wifi, 4, 0);
    lv_obj_set_flex_flow(row_wifi, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row_wifi, LV_FLEX_ALIGN_END,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lbl_wifi_val = lv_label_create(row_wifi);
    lv_label_set_text(lbl_wifi_val, lang_str_settings_wifi_name(current_wifi));
    lv_obj_set_style_text_color(lbl_wifi_val, theme_color_accent(), 0);
    lv_obj_set_style_text_font(lbl_wifi_val, &lv_font_cyr_14, 0);

    // Экран выше 240px — разрешить прокрутку контента вниз
    lv_obj_remove_flag(parent, LV_OBJ_FLAG_SCROLL_ELASTIC);
    lv_obj_set_scrollbar_mode(parent, LV_SCROLLBAR_MODE_AUTO);

    highlight_items();
}

void settings_app_close() {
    parent_ref = nullptr;
    slider_brightness = nullptr;
    lbl_brightness_val = nullptr;
    lbl_lang_val = nullptr;
    row_brightness = nullptr;
    row_lang = nullptr;
    row_storage = nullptr;
    lbl_storage_val = nullptr;
    row_wifi = nullptr;
    lbl_wifi_val = nullptr;
}
