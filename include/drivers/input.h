#pragma once

typedef enum {
    BTN_ID_UP = 0,
    BTN_ID_DOWN,
    BTN_ID_LEFT,
    BTN_ID_RIGHT,
    BTN_ID_OK,
    BTN_COUNT
} ButtonId;

typedef enum {
    BTN_EVENT_CLICKED,
    BTN_EVENT_PRESSED,
    BTN_EVENT_RELEASED,
    BTN_EVENT_LONG_PRESSED
} ButtonEvent;

typedef void (*ButtonCallback)(ButtonId id, ButtonEvent event);

void input_init();
void input_set_callback(ButtonCallback cb);
void input_update();
