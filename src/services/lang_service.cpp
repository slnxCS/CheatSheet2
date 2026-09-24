#include "services/lang_service.h"
#include <Preferences.h>

static Preferences prefs;
static LangId current_lang = LANG_EN;

struct LangStrings {
    const char* app_camera;
    const char* app_settings;
    const char* app_explorer;
    const char* app_ai;
    const char* ai_you;
    const char* settings_title;
    const char* settings_wifi;
    const char* wifi_on;
    const char* wifi_off;
    const char* settings_brightness;
    const char* settings_language;
    const char* settings_storage;
    const char* camera_title;
    const char* camera_init;
    const char* camera_ready;
    const char* camera_captured;
    const char* camera_no_camera;
    const char* camera_error;
    const char* camera_failed;
    const char* camera_focusing;
    const char* camera_mode_fill;
    const char* camera_mode_fit;
    const char* explorer_title;
    const char* explorer_flash;
    const char* explorer_sdcard;
    const char* explorer_select;
    const char* explorer_back;
    const char* explorer_open;
    const char* ai_nothing;
    const char* ai_pending;
    const char* ai_taken;
    const char* ai_answered;
    const char* ai_take_photo;
    const char* ai_send;
    const char* ai_no_photo;
    const char* ai_waiting;
};

static const LangStrings lang_en = {
    .app_camera      = "Camera",
    .app_settings    = "Settings",
    .app_explorer    = "Explorer",
    .app_ai          = "AI",
    .ai_you          = "Me",
    .settings_title  = "Settings",
    .settings_wifi   = "WiFi network",
    .wifi_on         = "On",
    .wifi_off        = "Off",
    .settings_brightness = "Brightness",
    .settings_language   = "Language",
    .settings_storage    = "Storage",
    .camera_title    = "Camera",
    .camera_init     = "Initializing...",
    .camera_ready    = "Ready - press OK to capture",
    .camera_captured = "Captured",
    .camera_no_camera = "No camera",
    .camera_error = "Save failed",
    .camera_failed   = "Camera init failed",
    .camera_focusing = "Autofocus...",
    .camera_mode_fill = "Full width",
    .camera_mode_fit = "Full frame",
    .explorer_title  = "Explorer",
    .explorer_flash  = "Flash",
    .explorer_sdcard = "SD Card",
    .explorer_select = "select",
    .explorer_back   = "back",
    .explorer_open   = "open",
    .ai_nothing  = "No photo yet. Press \"Take photo\".",
    .ai_pending  = "Photo taken. Open the app on your phone.",
    .ai_taken    = "Sent to AI - waiting for the answer...",
    .ai_answered = "Answer received",
    .ai_take_photo = "Take photo",
    .ai_send       = "Send",
    .ai_no_photo   = "No photo - take one first",
    .ai_waiting    = "Waiting for the phone...",
};

static const LangStrings lang_ru = {
    .app_camera      = "Камера",
    .app_settings    = "Настройки",
    .app_explorer    = "Проводник",
    .app_ai          = "ИИ",
    .ai_you          = "Вы",
    .settings_title  = "Настройки",
    .settings_wifi   = "WiFi сеть",
    .wifi_on         = "Вкл",
    .wifi_off        = "Выкл",
    .settings_brightness = "Яркость",
    .settings_language   = "Язык",
    .settings_storage    = "Накопитель",
    .camera_title    = "Камера",
    .camera_init     = "Инициализация...",
    .camera_ready    = "Готово - нажмите OK",
    .camera_captured = "Снято",
    .camera_no_camera = "Нет камеры",
    .camera_error = "Ошибка сохранения",
    .camera_failed   = "Ошибка камеры",
    .camera_focusing = "Автофокус...",
    .camera_mode_fill = "Во всю ширину",
    .camera_mode_fit = "Весь кадр",
    .explorer_title  = "Проводник",
    .explorer_flash  = "Flash",
    .explorer_sdcard = "SD карта",
    .explorer_select = "выбор",
    .explorer_back   = "назад",
    .explorer_open   = "открыть",
    .ai_nothing  = "Нет фото. Нажмите «Сфоткать».",
    .ai_pending  = "Фото снято. Откройте приложение на телефоне.",
    .ai_taken    = "Отправлено в ИИ - жду ответ...",
    .ai_answered = "Ответ получен",
    .ai_take_photo = "Сфоткать",
    .ai_send       = "Отправить",
    .ai_no_photo   = "Нет фото - сначала сфоткать",
    .ai_waiting    = "Ждём телефон...",
};

static const LangStrings* langs[LANG_COUNT] = {
    [LANG_EN] = &lang_en,
    [LANG_RU] = &lang_ru,
};

static const char* lang_names[LANG_COUNT] = {
    [LANG_EN] = "English",
    [LANG_RU] = "Русский",
};

void lang_init() {
    prefs.begin("lang", true);
    int stored = prefs.getInt("lang", LANG_EN);
    prefs.end();
    if (stored >= 0 && stored < LANG_COUNT) {
        current_lang = (LangId)stored;
    }
}

LangId lang_get() {
    return current_lang;
}

void lang_set(LangId lang) {
    if (lang < 0 || lang >= LANG_COUNT) return;
    current_lang = lang;
    prefs.begin("lang", false);
    prefs.putInt("lang", (int)lang);
    prefs.end();
}

const char* lang_str_app_camera()     { return langs[current_lang]->app_camera; }
const char* lang_str_app_settings()   { return langs[current_lang]->app_settings; }
const char* lang_str_settings_title() { return langs[current_lang]->settings_title; }
const char* lang_str_settings_brightness() { return langs[current_lang]->settings_brightness; }
const char* lang_str_settings_language()   { return langs[current_lang]->settings_language; }
const char* lang_str_camera_title()   { return langs[current_lang]->camera_title; }
const char* lang_str_camera_init()    { return langs[current_lang]->camera_init; }
const char* lang_str_camera_ready()   { return langs[current_lang]->camera_ready; }
const char* lang_str_camera_captured(){ return langs[current_lang]->camera_captured; }
const char* lang_str_camera_no_camera() { return langs[current_lang]->camera_no_camera; }
const char* lang_str_camera_error()    { return langs[current_lang]->camera_error; }
const char* lang_str_camera_failed()  { return langs[current_lang]->camera_failed; }
const char* lang_str_camera_focusing(){ return langs[current_lang]->camera_focusing; }
const char* lang_str_camera_mode_fill(){ return langs[current_lang]->camera_mode_fill; }
const char* lang_str_camera_mode_fit() { return langs[current_lang]->camera_mode_fit; }
const char* lang_str_app_explorer()   { return langs[current_lang]->app_explorer; }
const char* lang_str_app_ai()         { return langs[current_lang]->app_ai; }
const char* lang_str_ai_you()         { return langs[current_lang]->ai_you; }
const char* lang_str_ai_take_photo()  { return langs[current_lang]->ai_take_photo; }
const char* lang_str_ai_send()        { return langs[current_lang]->ai_send; }
const char* lang_str_ai_no_photo()    { return langs[current_lang]->ai_no_photo; }
const char* lang_str_ai_waiting()     { return langs[current_lang]->ai_waiting; }
const char* lang_str_settings_wifi()  { return langs[current_lang]->settings_wifi; }
const char* lang_str_ai_nothing()     { return langs[current_lang]->ai_nothing; }
const char* lang_str_ai_pending()     { return langs[current_lang]->ai_pending; }
const char* lang_str_ai_taken()       { return langs[current_lang]->ai_taken; }
const char* lang_str_ai_answered()    { return langs[current_lang]->ai_answered; }

const char* lang_str_settings_wifi_name(bool on) {
    return on ? langs[current_lang]->wifi_on : langs[current_lang]->wifi_off;
}
const char* lang_str_explorer_title() { return langs[current_lang]->explorer_title; }
const char* lang_str_explorer_flash() { return langs[current_lang]->explorer_flash; }
const char* lang_str_explorer_sdcard(){ return langs[current_lang]->explorer_sdcard; }
const char* lang_str_explorer_select(){ return langs[current_lang]->explorer_select; }
const char* lang_str_explorer_back()  { return langs[current_lang]->explorer_back; }
const char* lang_str_explorer_open()  { return langs[current_lang]->explorer_open; }

const char* lang_str_settings_lang_name(LangId id) {
    if (id < 0 || id >= LANG_COUNT) return "?";
    return lang_names[id];
}

const char* lang_str_settings_storage() {
    return langs[current_lang]->settings_storage;
}

static const char* storage_names[] = { "Flash", "SD Card" };

const char* lang_str_settings_storage_name(int idx) {
    if (idx < 0 || idx > 1) return "?";
    return storage_names[idx];
}
