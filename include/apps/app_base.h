#pragma once

#include <lvgl.h>

#define APP_NAME_LEN 32

typedef void (*AppInitFunc)(lv_obj_t* parent);
typedef void (*AppDeinitFunc)();
typedef void (*AppButtonFunc)(int button_id, int event);

typedef struct {
    char name[APP_NAME_LEN];
    const char* icon_symbol;
    AppInitFunc on_open;
    AppDeinitFunc on_close;
    AppButtonFunc on_button;
    bool is_running;
    lv_obj_t* screen;
} AppContext;
