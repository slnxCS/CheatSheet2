#include <Arduino.h>
#include "config/pins.h"
#include "drivers/display.h"
#include "drivers/input.h"
#include "ui/ui_manager.h"
#include "ui/screens/home_screen.h"
#include "apps/app_registry.h"
#include "apps/app_base.h"
#include "apps/builtin/camera_app.h"
#include "apps/builtin/settings_app.h"
#include "apps/builtin/file_explorer_app.h"
#include "apps/builtin/snake_game.h"
#include "apps/builtin/game_2048.h"
#include "apps/builtin/tetris_game.h"
#include "apps/builtin/flappy_game.h"
#include "apps/builtin/ai_answer_app.h"
#include "services/lang_service.h"
#include "services/storage_service.h"
#include "services/battery_service.h"
#include "services/wifi_service.h"
#include "services/ai_link.h"
#include "ui/app_host.h"
#include "ui/theme.h"
#include "SD_MMC.h"
#include "LittleFS.h"

static bool in_app = false;
static int current_app_idx = -1;
static int ai_app_idx = -1;

static void init_fs() {
    storage_set_constrain(LittleFS.begin(true) ? 0 : -1);

    SD_MMC.setPins(39,38,40);
    storage_set_constrain(SD_MMC.begin("/sdcard", true) ? 1 : 0);
}

static void host_open(int idx) {
    current_app_idx = idx;
    in_app = true;
    ui_manager_switch(SCREEN_APP);
    app_registry_open(idx, lv_screen_active());
}

static void host_close_to_home() {
    app_registry_close(current_app_idx);
    in_app = false;
    current_app_idx = -1;
    ui_manager_reload_home();
}

// --- app_host: программное открытие (ai_link → экран «ИИ») ---

void app_host_open_index(int idx) {
    if (idx < 0 || idx >= app_registry_count()) return;
    if (in_app) {
        if (current_app_idx == idx) return;
        // Переключение: закрыть текущее приложение без возврата на домой
        app_registry_close(current_app_idx);
        current_app_idx = -1;
        in_app = false;
    }
    host_open(idx);
}

bool app_host_in_app() { return in_app; }
int  app_host_current() { return current_app_idx; }
int  app_host_ai_index() { return ai_app_idx; }

static void on_button(ButtonId id, ButtonEvent event) {
    if (in_app) {
        if (id == BTN_ID_OK && event == BTN_EVENT_LONG_PRESSED) {
            host_close_to_home();
            return;
        }
        // PRESSED и LONG_PRESSED тоже пробрасываем: PRESSED — игры (Flappy)
        // реагируют на нажатие, LONG (кроме OK — он выходит из приложения) —
        // переключение режима превью в камере. Остальные приложения
        // фильтруют по BTN_EVENT_CLICKED.
        if (event == BTN_EVENT_CLICKED || event == BTN_EVENT_PRESSED ||
            event == BTN_EVENT_LONG_PRESSED) {
            AppContext* app = app_registry_get_running();
            if (app && app->on_button) {
                app->on_button((int)id, (int)event);
            }
        }
        return;
    }

    if (event != BTN_EVENT_CLICKED) return;

    int selected = home_screen_get_selected();
    int count = home_screen_get_app_count();
    if (count == 0) return;

    int cols = APP_GRID_COLS;

    switch (id) {
        case BTN_ID_UP:
            if (selected >= cols) selected -= cols;
            break;
        case BTN_ID_DOWN:
            if (selected + cols < count) selected += cols;
            break;
        case BTN_ID_LEFT:
            if (selected > 0) selected--;
            break;
        case BTN_ID_RIGHT:
            if (selected < count - 1) selected++;
            break;
        case BTN_ID_OK:
            host_open(selected);
            return;
        default:
            break;
    }

    home_screen_set_selected(selected);
}

void setup() {
    Serial.begin(115200);
    delay(1000);

    Serial.println("=== CheatSheet2 ===");
    Serial.printf("CPU freq: %d MHz\n", getCpuFrequencyMhz());
    Serial.printf("Free heap: %u bytes\n", (unsigned)ESP.getFreeHeap());

    if (psramFound()) {
        Serial.printf("PSRAM: %u bytes free\n", (unsigned)ESP.getFreePsram());
    } else {
        Serial.println("WARNING: PSRAM not found!");
    }

    Serial.println("[1/6] Display...");
    display_init();
    Serial.println("[1/6] Display DONE");

    Serial.println("[2.7/6] Battery...");
    battery_init();

    Serial.println("[2/6] Input...");
    input_init();
    input_set_callback(on_button);
    Serial.println("[2/6] Input DONE");

    Serial.println("[2.5/6] Language...");
    lang_init();
    Serial.printf("[2.5/6] Language: %d\n", lang_get());

    Serial.println("[2.6/6] Storage...");
    storage_init();
    Serial.printf("[2.6/6] Storage: %d\n", storage_get());

    Serial.println("[3/6] Apps...");
    app_registry_init();
    app_registry_add(lang_str_app_camera(),  LV_SYMBOL_IMAGE,  camera_app_open,  camera_app_close, camera_app_button);
    app_registry_add(lang_str_app_settings(), LV_SYMBOL_SETTINGS, settings_app_open, settings_app_close, settings_app_button);
    app_registry_add(lang_str_app_explorer(), LV_SYMBOL_DIRECTORY, file_explorer_open, file_explorer_close, file_explorer_button);
    app_registry_add("Snake", LV_SYMBOL_PLAY, snake_game_open, snake_game_close, snake_game_button);
    app_registry_add("2048", LV_SYMBOL_PLAY, game_2048_open, game_2048_close, game_2048_button);
    app_registry_add("Tetris", LV_SYMBOL_PLAY, tetris_game_open, tetris_game_close, tetris_game_button);
    app_registry_add("Flappy", LV_SYMBOL_PLAY, flappy_game_open, flappy_game_close, flappy_game_button);
    ai_app_idx = app_registry_add(lang_str_app_ai(), LV_SYMBOL_EDIT,
                                  ai_answer_app_open, ai_answer_app_close,
                                  ai_answer_app_button);
    Serial.printf("[3/6] Apps registered: %d\n", app_registry_count());

    Serial.println("[4/6] UI...");
    ui_manager_init();
    Serial.println("[4/6] UI DONE");

    Serial.println("[5/6] Registering apps on home...");
    for (int i = 0; i < app_registry_count() && i < APP_GRID_COLS * APP_GRID_ROWS; i++) {
        AppContext* app = app_registry_get(i);
        if (app) {
            home_screen_register_app(app->name, app->icon_symbol, nullptr);
        }
    }
    Serial.println("[5/6] Apps registered on home");

    Serial.println("[6/6] Boot complete!");

    init_fs();

    // WiFi SoftAP («CSCAM») + HTTP-мост «устройство → телефон → ИИ»
    Serial.println("[7/7] WiFi + AI link...");
    wifi_service_init();
    ai_link_init();
    Serial.println("[7/7] WiFi + AI link DONE");
}

static unsigned long last_tick = 0;

void loop() {
    unsigned long now = millis();
    lv_tick_inc(now - last_tick);
    last_tick = now;

    input_update();
    ui_manager_update();
    lv_timer_handler();
    delay(2);
}
