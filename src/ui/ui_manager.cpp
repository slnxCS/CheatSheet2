#include "ui/ui_manager.h"
#include "ui/screens/splash_screen.h"
#include "ui/screens/home_screen.h"
#include "ui/widgets/status_bar.h"
#include "ui/widgets/plasma_bg.h"
#include "ui/theme.h"

static ScreenId current_screen = SCREEN_SPLASH;
static bool splash_done = false;
static lv_obj_t* home_screen_obj = nullptr;

static void create_home_screen(lv_obj_t* scr) {
    lv_obj_set_style_bg_color(scr, theme_color_bg(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    plasma_bg_create(scr);
    status_bar_create(scr);
    home_screen_create(scr);
}

void ui_manager_init() {
    theme_init();
    splash_screen_create();
    current_screen = SCREEN_SPLASH;
    splash_done = false;
    home_screen_obj = nullptr;
}

void ui_manager_switch(ScreenId screen) {
    switch (screen) {
        case SCREEN_HOME: {
            splash_screen_destroy();

            if (!home_screen_obj) {
                home_screen_obj = lv_obj_create(NULL);
                lv_obj_set_size(home_screen_obj, LV_PCT(100), LV_PCT(100));
                create_home_screen(home_screen_obj);
            }
            lv_scr_load(home_screen_obj);
            break;
        }
        case SCREEN_APP:
            break;
        default:
            break;
    }

    current_screen = screen;
}

void ui_manager_reload_home() {
    if (home_screen_obj) {
        lv_scr_load(home_screen_obj);
    }
    current_screen = SCREEN_HOME;
}

void ui_manager_update() {
    switch (current_screen) {
        case SCREEN_SPLASH:
            if (!splash_done && splash_screen_update()) {
                splash_done = true;
                ui_manager_switch(SCREEN_HOME);
            }
            break;
        case SCREEN_HOME:
            status_bar_update();
            plasma_bg_update();
            break;
        case SCREEN_APP:
            break;
    }
}

ScreenId ui_manager_current() {
    return current_screen;
}
