#pragma once

#include <lvgl.h>

void status_bar_create(lv_obj_t* parent);
void status_bar_update();
void status_bar_set_wifi(bool connected);
void status_bar_set_time(const char* time_str);
