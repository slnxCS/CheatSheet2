#pragma once

#include <lvgl.h>

#define APP_NAME_MAX_LEN 32
#define APP_ICON_SIZE    50
#define APP_GRID_COLS    3
#define APP_GRID_ROWS    4

typedef void (*AppLaunchCallback)(int app_index);

typedef struct {
    char name[APP_NAME_MAX_LEN];
    const char* icon;
    AppLaunchCallback on_launch;
} AppEntry;

void home_screen_create(lv_obj_t* parent);
void home_screen_register_app(const char* name, const char* icon, AppLaunchCallback cb);
void home_screen_set_selected(int index);
int  home_screen_get_selected();
int  home_screen_get_app_count();
