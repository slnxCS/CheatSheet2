#include "fonts/fonts.h"
#include "ui/screens/home_screen.h"
#include "ui/theme.h"
#include "services/battery_service.h"
#include <cstring>

#define ICON_SIZE 50
#define ICON_RADIUS 25

static lv_obj_t* home_obj = nullptr;
static lv_obj_t* app_icons[APP_GRID_COLS * APP_GRID_ROWS] = {nullptr};
static lv_obj_t* app_labels[APP_GRID_COLS * APP_GRID_ROWS] = {nullptr};
static lv_obj_t* app_bgs[APP_GRID_COLS * APP_GRID_ROWS] = {nullptr};
static lv_obj_t* app_glare[APP_GRID_COLS * APP_GRID_ROWS] = {nullptr};
static lv_obj_t* page_dots = nullptr;
static lv_timer_t* bat_timer = nullptr;

static AppEntry apps[APP_GRID_COLS * APP_GRID_ROWS];
static int app_count = 0;
static int selected_index = 0;
static int scroll_offset = 0;

static const lv_color_t app_colors[] = {
    lv_color_hex(0xE53935),
    lv_color_hex(0x43A047),
    lv_color_hex(0xFB8C00),
    lv_color_hex(0x1E88E5),
    lv_color_hex(0x8E24AA),
    lv_color_hex(0x00ACC1),
    lv_color_hex(0xD81B60),
    lv_color_hex(0x6D4C41),
    lv_color_hex(0x546E7A),
    lv_color_hex(0xFFB300),
};

static lv_style_t style_icon;
static lv_style_t style_icon_sel;
static bool styles_inited = false;

static void init_styles() {
    if (styles_inited) return;
    styles_inited = true;

    lv_style_init(&style_icon);
    lv_style_set_radius(&style_icon, ICON_RADIUS);
    lv_style_set_bg_opa(&style_icon, LV_OPA_COVER);
    lv_style_set_border_width(&style_icon, 0);
    lv_style_set_shadow_width(&style_icon, 8);
    lv_style_set_shadow_opa(&style_icon, LV_OPA_30);

    lv_style_init(&style_icon_sel);
    lv_style_set_radius(&style_icon_sel, ICON_RADIUS);
    lv_style_set_bg_opa(&style_icon_sel, LV_OPA_COVER);
    lv_style_set_border_width(&style_icon_sel, 3);
    lv_style_set_border_color(&style_icon_sel, lv_color_white());
    lv_style_set_border_opa(&style_icon_sel, LV_OPA_COVER);
    lv_style_set_shadow_width(&style_icon_sel, 25);
    lv_style_set_shadow_color(&style_icon_sel, lv_color_white());
    lv_style_set_shadow_opa(&style_icon_sel, LV_OPA_70);
    lv_style_set_shadow_spread(&style_icon_sel, 5);
}

static lv_color_t get_app_color(int idx) {
    return app_colors[idx % (sizeof(app_colors) / sizeof(app_colors[0]))];
}

static void update_selection() {
    for (int i = 0; i < APP_GRID_COLS * APP_GRID_ROWS; i++) {
        if (!app_bgs[i] || i >= app_count) continue;

        lv_obj_remove_style(app_bgs[i], &style_icon, 0);
        lv_obj_remove_style(app_bgs[i], &style_icon_sel, 0);

        lv_color_t c = get_app_color(i);
        lv_obj_set_style_bg_color(app_bgs[i], c, 0);
        lv_obj_set_style_shadow_color(app_bgs[i], c, 0);

        if (i == selected_index) {
            lv_obj_add_style(app_bgs[i], &style_icon_sel, 0);
        } else {
            lv_obj_add_style(app_bgs[i], &style_icon, 0);
        }
    }

    if (page_dots) {
        char dots[16] = {0};
        int total_pages = (app_count + APP_GRID_COLS * APP_GRID_ROWS - 1) / (APP_GRID_COLS * APP_GRID_ROWS);
        if (total_pages < 1) total_pages = 1;
        int cur_page = scroll_offset / (APP_GRID_COLS * APP_GRID_ROWS);
        for (int i = 0; i < total_pages; i++) {
            strcat(dots, (i == cur_page) ? LV_SYMBOL_CLOSE " " : LV_SYMBOL_LOOP " ");
        }
        lv_label_set_text(page_dots, dots);
    }
}

// --- Battery icon (нарисована примитивами, не из шрифта) ---
static lv_obj_t* bat_container = nullptr;
static lv_obj_t* bat_fill = nullptr;
static lv_obj_t* bat_pct = nullptr;

static void update_battery_label() {
    if (!bat_container) return;

    float v = battery_get_voltage();
    int pct = battery_get_percent();
    bool usb = battery_is_usb_connected();

    lv_color_t col;
    if (usb) {
        col = lv_color_hex(0x4CAF50);
    } else if (pct < 0) {
        col = lv_color_hex(0x9E9E9E);
    } else if (pct > 50) {
        col = lv_color_hex(0x4CAF50);
    } else if (pct > 20) {
        col = lv_color_hex(0xFF9800);
    } else {
        col = lv_color_hex(0xF44336);
    }

    // Полоска заряда
    if (bat_fill) {
        int fill_w = 0;
        if (usb) {
            fill_w = 16;
        } else if (pct >= 0) {
            fill_w = (pct * 16) / 100;
            if (fill_w < 1) fill_w = 1;
        }
        lv_obj_set_size(bat_fill, fill_w, 8);
        lv_obj_set_style_bg_color(bat_fill, col, 0);
    }

    // Текст
    if (bat_pct) {
        lv_obj_set_style_text_color(bat_pct, col, 0);
        if (usb) {
            lv_label_set_text(bat_pct, "USB");
        } else if (pct >= 0) {
            lv_label_set_text_fmt(bat_pct, "%d%%", pct);
        } else {
            lv_label_set_text_fmt(bat_pct, "%.1fV", v);
        }
    }
}

static void bat_timer_cb(lv_timer_t* t) {
    update_battery_label();
}

static void create_icon_grid(lv_obj_t* parent) {
    int total_w = 320;
    int total_h = 240 - 28 - 12;

    lv_obj_t* grid = lv_obj_create(parent);
    lv_obj_set_size(grid, total_w, total_h);
    lv_obj_set_style_bg_opa(grid, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(grid, 0, 0);
    lv_obj_set_style_pad_all(grid, 0, 0);
    lv_obj_set_style_radius(grid, 0, 0);
    lv_obj_align(grid, LV_ALIGN_TOP_MID, 0, 28);

    int row_counts[3] = {3, 4, 3};
    int row_h = total_h / 3;

    int idx = 0;
    for (int row = 0; row < 3; row++) {
        int cols = row_counts[row];
        int area_w = (cols == 4) ? total_w : 280;
        int col_w = area_w / cols;

        for (int col = 0; col < cols; col++) {
            if (idx >= APP_GRID_COLS * APP_GRID_ROWS) break;

            int x_off = (cols == 4) ? 0 : (total_w - area_w) / 2;
            int x = x_off + col * col_w + (col_w - ICON_SIZE) / 2;
            int y = row * row_h + (row_h - ICON_SIZE - 10) / 2;

            app_bgs[idx] = lv_obj_create(grid);
            lv_obj_set_size(app_bgs[idx], ICON_SIZE, ICON_SIZE);
            lv_obj_set_style_pad_all(app_bgs[idx], 0, 0);
            lv_obj_set_pos(app_bgs[idx], x, y);
            lv_obj_add_style(app_bgs[idx], &style_icon, 0);

            lv_color_t c = get_app_color(idx);
            lv_color_t c_light = lv_color_lighten(c, 60);
            lv_color_t c_dark = lv_color_darken(c, 40);
            lv_obj_set_style_bg_color(app_bgs[idx], c_dark, 0);

            lv_grad_dsc_t gdsc;
            gdsc.dir = LV_GRAD_DIR_VER;
            gdsc.stops_count = 2;
            gdsc.stops[0].color = c_light;
            gdsc.stops[0].opa = LV_OPA_COVER;
            gdsc.stops[0].frac = 0;
            gdsc.stops[1].color = c_dark;
            gdsc.stops[1].opa = LV_OPA_COVER;
            gdsc.stops[1].frac = 255;
            lv_obj_set_style_bg_grad(app_bgs[idx], &gdsc, 0);

            app_glare[idx] = lv_obj_create(app_bgs[idx]);
            lv_obj_remove_style_all(app_glare[idx]);
            lv_obj_set_size(app_glare[idx], 18, 18);
            lv_obj_set_style_radius(app_glare[idx], LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_bg_color(app_glare[idx], lv_color_white(), 0);
            lv_obj_set_style_bg_opa(app_glare[idx], LV_OPA_50, 0);
            lv_obj_align(app_glare[idx], LV_ALIGN_TOP_RIGHT, -20, 6);

            app_icons[idx] = lv_label_create(app_bgs[idx]);
            lv_obj_set_style_text_font(app_icons[idx], &lv_font_cyr_20, 0);
            lv_obj_set_style_text_color(app_icons[idx], lv_color_white(), 0);
            lv_obj_center(app_icons[idx]);

            app_labels[idx] = lv_label_create(grid);
            lv_obj_set_style_text_font(app_labels[idx], &lv_font_cyr_10, 0);
            lv_obj_set_style_text_color(app_labels[idx], lv_color_white(), 0);
            lv_obj_set_style_pad_all(app_labels[idx], 0, 0);
            lv_obj_set_width(app_labels[idx], ICON_SIZE);
            lv_obj_set_style_text_align(app_labels[idx], LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_set_pos(app_labels[idx], x, y + ICON_SIZE + 1);

            idx++;
        }
    }

    for (int i = 0; i < app_count && i < APP_GRID_COLS * APP_GRID_ROWS; i++) {
        if (app_icons[i]) lv_label_set_text(app_icons[i], apps[i].icon ? apps[i].icon : LV_SYMBOL_IMAGE);
        if (app_labels[i]) lv_label_set_text(app_labels[i], apps[i].name);
    }

    page_dots = lv_label_create(parent);
    lv_obj_set_style_text_font(page_dots, &lv_font_cyr_10, 0);
    lv_obj_set_style_text_color(page_dots, lv_color_white(), 0);
    lv_obj_align(page_dots, LV_ALIGN_BOTTOM_MID, 0, -2);
    lv_label_set_text(page_dots, ".");

    update_selection();
}

void home_screen_create(lv_obj_t* parent) {
    home_obj = lv_obj_create(parent);
    lv_obj_set_size(home_obj, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_opa(home_obj, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(home_obj, 0, 0);
    lv_obj_set_style_pad_all(home_obj, 0, 0);
    lv_obj_set_style_radius(home_obj, 0, 0);

    // --- Battery icon in header ---
    // Контейнер: корпус батареи (рамка + наконечник)
    bat_container = lv_obj_create(home_obj);
    lv_obj_set_size(bat_container, 22, 12);   // корпус + наконечник
    lv_obj_set_style_bg_color(bat_container, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(bat_container, LV_OPA_30, 0);
    lv_obj_set_style_border_width(bat_container, 1, 0);
    lv_obj_set_style_border_color(bat_container, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_opa(bat_container, LV_OPA_50, 0);
    lv_obj_set_style_radius(bat_container, 2, 0);
    lv_obj_set_style_pad_all(bat_container, 1, 0);
    lv_obj_align(bat_container, LV_ALIGN_TOP_RIGHT, -40, 3);

    // Полоска заряда внутри корпуса
    bat_fill = lv_obj_create(bat_container);
    lv_obj_set_size(bat_fill, 16, 8);
    lv_obj_set_style_bg_color(bat_fill, lv_color_hex(0x4CAF50), 0);
    lv_obj_set_style_bg_opa(bat_fill, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bat_fill, 0, 0);
    lv_obj_set_style_radius(bat_fill, 1, 0);
    lv_obj_set_style_pad_all(bat_fill, 0, 0);
    lv_obj_align(bat_fill, LV_ALIGN_LEFT_MID, 0, 0);

    // Процент рядом с иконкой
    bat_pct = lv_label_create(home_obj);
    lv_obj_set_style_text_font(bat_pct, &lv_font_cyr_10, 0);
    lv_obj_align(bat_pct, LV_ALIGN_TOP_RIGHT, -4, 3);

    update_battery_label();

    // Timer: обновлять каждые 5 секунд
    bat_timer = lv_timer_create(bat_timer_cb, 5000, nullptr);

    init_styles();
    create_icon_grid(home_obj);
}

void home_screen_register_app(const char* name, const char* icon, AppLaunchCallback cb) {
    if (app_count >= APP_GRID_COLS * APP_GRID_ROWS) return;

    strncpy(apps[app_count].name, name, APP_NAME_MAX_LEN - 1);
    apps[app_count].name[APP_NAME_MAX_LEN - 1] = '\0';
    apps[app_count].icon = icon;
    apps[app_count].on_launch = cb;
    app_count++;

    if (app_count <= APP_GRID_COLS * APP_GRID_ROWS) {
        int idx = app_count - 1;
        if (app_icons[idx]) {
            lv_label_set_text(app_icons[idx], icon ? icon : LV_SYMBOL_IMAGE);
        }
        if (app_labels[idx]) {
            lv_label_set_text(app_labels[idx], apps[idx].name);
        }
    }

    update_selection();
}

void home_screen_set_selected(int index) {
    if (index < 0 || index >= app_count) return;
    selected_index = index;
    update_selection();
}

int home_screen_get_selected() {
    return selected_index;
}

int home_screen_get_app_count() {
    return app_count;
}
