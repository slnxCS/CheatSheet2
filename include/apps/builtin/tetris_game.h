#pragma once

#include <lvgl.h>

void tetris_game_open(lv_obj_t* parent);
void tetris_game_close();
void tetris_game_button(int button_id, int event);
