#pragma once

#include <lvgl.h>

typedef enum { LANG_EN = 0, LANG_RU = 1, LANG_COUNT } LangId;

void lang_init();
LangId lang_get();
void lang_set(LangId lang);

const char* lang_str_app_camera();
const char* lang_str_app_settings();
const char* lang_str_settings_title();
const char* lang_str_settings_brightness();
const char* lang_str_settings_language();
const char* lang_str_settings_lang_name(LangId id);
const char* lang_str_settings_storage();
const char* lang_str_settings_storage_name(int idx);
const char* lang_str_camera_title();
const char* lang_str_camera_init();
const char* lang_str_camera_ready();
const char* lang_str_camera_captured();
const char* lang_str_camera_no_camera();
const char* lang_str_camera_error();
const char* lang_str_camera_failed();
const char* lang_str_app_explorer();
const char* lang_str_explorer_title();
const char* lang_str_explorer_flash();
const char* lang_str_explorer_sdcard();
const char* lang_str_explorer_select();
const char* lang_str_explorer_back();
const char* lang_str_explorer_open();
