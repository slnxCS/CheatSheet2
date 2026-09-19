#include "apps/app_registry.h"
#include <cstring>

static AppContext apps[MAX_APPS];
static int app_count = 0;

void app_registry_init() {
    app_count = 0;
    memset(apps, 0, sizeof(apps));
}

int app_registry_add(const char* name, const char* icon, AppInitFunc init, AppDeinitFunc deinit, AppButtonFunc on_button) {
    if (app_count >= MAX_APPS) return -1;

    strncpy(apps[app_count].name, name, APP_NAME_LEN - 1);
    apps[app_count].name[APP_NAME_LEN - 1] = '\0';
    apps[app_count].icon_symbol = icon;
    apps[app_count].on_open = init;
    apps[app_count].on_close = deinit;
    apps[app_count].on_button = on_button;
    apps[app_count].is_running = false;
    apps[app_count].screen = nullptr;

    return app_count++;
}

int app_registry_count() {
    return app_count;
}

AppContext* app_registry_get(int index) {
    if (index < 0 || index >= app_count) return nullptr;
    return &apps[index];
}

AppContext* app_registry_get_running() {
    for (int i = 0; i < app_count; i++) {
        if (apps[i].is_running) return &apps[i];
    }
    return nullptr;
}

void app_registry_open(int index, lv_obj_t* parent) {
    AppContext* app = app_registry_get(index);
    if (!app || !app->on_open) return;

    if (app->is_running) return;

    app->screen = lv_obj_create(NULL);
    lv_obj_set_size(app->screen, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_opa(app->screen, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(app->screen, lv_color_hex(0x0D47A1), 0);
    lv_obj_set_style_border_width(app->screen, 0, 0);
    lv_obj_set_style_pad_all(app->screen, 0, 0);

    app->on_open(app->screen);
    app->is_running = true;

    lv_scr_load(app->screen);
}

void app_registry_close(int index) {
    AppContext* app = app_registry_get(index);
    if (!app) return;

    if (app->is_running) {
        if (app->on_close) app->on_close();
        if (app->screen) {
            lv_obj_delete(app->screen);
            app->screen = nullptr;
        }
        app->is_running = false;
    }
}
