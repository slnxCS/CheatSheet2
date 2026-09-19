#pragma once

#include <lvgl.h>

void display_init();
void display_set_brightness(uint8_t percent);
lv_display_t* display_get_lvgl();
