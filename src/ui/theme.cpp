#include "ui/theme.h"

static lv_color_t color_primary;
static lv_color_t color_secondary;
static lv_color_t color_accent;
static lv_color_t color_bg;
static lv_color_t color_panel;
static lv_color_t color_text;
static lv_color_t color_text_muted;

void theme_init() {
    color_primary    = lv_color_hex(0x1565C0);
    color_secondary  = lv_color_hex(0x42A5F5);
    color_accent     = lv_color_hex(0xBBDEFB);
    color_bg         = lv_color_hex(0x0D47A1);
    color_panel      = lv_color_hex(0x90CAF9);
    color_text       = lv_color_hex(0xFFFFFF);
    color_text_muted = lv_color_hex(0xBBDEFB);

    lv_theme_t* th = lv_theme_default_init(
        lv_display_get_default(),
        color_accent,
        color_primary,
        true,
        LV_FONT_DEFAULT
    );
    lv_display_set_theme(lv_display_get_default(), th);

    lv_obj_t* scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, color_bg, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
}

lv_color_t theme_color_primary()    { return color_primary; }
lv_color_t theme_color_secondary()  { return color_secondary; }
lv_color_t theme_color_accent()     { return color_accent; }
lv_color_t theme_color_bg()         { return color_bg; }
lv_color_t theme_color_panel()      { return color_panel; }
lv_color_t theme_color_text()       { return color_text; }
lv_color_t theme_color_text_muted() { return color_text_muted; }
