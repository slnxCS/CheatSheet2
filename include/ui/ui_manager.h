#pragma once

#include <lvgl.h>

typedef enum {
    SCREEN_SPLASH,
    SCREEN_HOME,
    SCREEN_APP
} ScreenId;

void ui_manager_init();
void ui_manager_switch(ScreenId screen);
void ui_manager_reload_home();
void ui_manager_update();
ScreenId ui_manager_current();
