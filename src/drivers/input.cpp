#include "drivers/input.h"
#include "config/pins.h"
#include <Arduino.h>

static const int btnPins[BTN_COUNT] = {
    PIN_BTN_UP,
    PIN_BTN_DOWN,
    PIN_BTN_LEFT,
    PIN_BTN_RIGHT,
    PIN_BTN_OK
};

static bool btnState[BTN_COUNT] = {false};
static bool btnLastState[BTN_COUNT] = {false};
static uint32_t btnDebounce[BTN_COUNT] = {0};
static uint32_t btnPressStart[BTN_COUNT] = {0};
static bool btnLongFired[BTN_COUNT] = {false};

// Repeat-fire state for directional buttons
static bool btnRepeatActive[BTN_COUNT] = {false};
static uint32_t btnLastRepeat[BTN_COUNT] = {0};

static ButtonCallback userCallback = nullptr;

static const uint32_t DEBOUNCE_MS = 20;
static const uint32_t LONG_PRESS_MS = 550;
static const uint32_t REPEAT_DELAY_MS = 300;   // delay before repeat starts
static const uint32_t REPEAT_RATE_MS = 80;     // interval between repeats

static bool is_nav_button(int i) {
    return i == BTN_ID_UP || i == BTN_ID_DOWN ||
           i == BTN_ID_LEFT || i == BTN_ID_RIGHT;
}

void input_init() {
    for (int i = 0; i < BTN_COUNT; i++) {
        pinMode(btnPins[i], INPUT_PULLUP);
        btnState[i] = false;
        btnLastState[i] = false;
        btnDebounce[i] = 0;
        btnPressStart[i] = 0;
        btnLongFired[i] = false;
        btnRepeatActive[i] = false;
        btnLastRepeat[i] = 0;
    }
}

void input_set_callback(ButtonCallback cb) {
    userCallback = cb;
}

void input_update() {
    uint32_t now = millis();

    for (int i = 0; i < BTN_COUNT; i++) {
        bool reading = (digitalRead(btnPins[i]) == LOW);

        if (reading != btnLastState[i]) {
            btnDebounce[i] = now;
        }

        if ((now - btnDebounce[i]) > DEBOUNCE_MS) {
            if (reading != btnState[i]) {
                btnState[i] = reading;

                if (btnState[i]) {
                    // Button pressed
                    btnPressStart[i] = now;
                    btnLongFired[i] = false;
                    btnRepeatActive[i] = false;
                    btnLastRepeat[i] = now;
                    if (userCallback) userCallback((ButtonId)i, BTN_EVENT_PRESSED);
                } else {
                    // Button released
                    btnRepeatActive[i] = false;
                    if (!btnLongFired[i] && userCallback) {
                        userCallback((ButtonId)i, BTN_EVENT_CLICKED);
                    }
                    if (userCallback) userCallback((ButtonId)i, BTN_EVENT_RELEASED);
                }
            }

            // Long press detection
            if (btnState[i] && !btnLongFired[i] &&
                (now - btnPressStart[i]) > LONG_PRESS_MS) {
                btnLongFired[i] = true;
                if (userCallback) userCallback((ButtonId)i, BTN_EVENT_LONG_PRESSED);
            }

            // Repeat-fire for nav buttons (after long press threshold)
            if (btnState[i] && is_nav_button(i) && !btnLongFired[i]) {
                uint32_t held = now - btnPressStart[i];
                if (held >= REPEAT_DELAY_MS) {
                    if (!btnRepeatActive[i]) {
                        // First repeat
                        btnRepeatActive[i] = true;
                        btnLastRepeat[i] = now;
                        if (userCallback) userCallback((ButtonId)i, BTN_EVENT_CLICKED);
                    } else if ((now - btnLastRepeat[i]) >= REPEAT_RATE_MS) {
                        btnLastRepeat[i] = now;
                        if (userCallback) userCallback((ButtonId)i, BTN_EVENT_CLICKED);
                    }
                }
            }
        }

        btnLastState[i] = reading;
    }
}
