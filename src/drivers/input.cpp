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

static ButtonCallback userCallback = nullptr;

static const uint32_t DEBOUNCE_MS = 50;
static const uint32_t LONG_PRESS_MS = 800;

void input_init() {
    for (int i = 0; i < BTN_COUNT; i++) {
        pinMode(btnPins[i], INPUT_PULLUP);
        btnState[i] = false;
        btnLastState[i] = false;
        btnDebounce[i] = 0;
        btnPressStart[i] = 0;
        btnLongFired[i] = false;
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
                    btnPressStart[i] = now;
                    btnLongFired[i] = false;
                    if (userCallback) userCallback((ButtonId)i, BTN_EVENT_PRESSED);
                } else {
                    if (!btnLongFired[i] && userCallback) {
                        userCallback((ButtonId)i, BTN_EVENT_CLICKED);
                    }
                    if (userCallback) userCallback((ButtonId)i, BTN_EVENT_RELEASED);
                }
            }

            if (btnState[i] && !btnLongFired[i] &&
                (now - btnPressStart[i]) > LONG_PRESS_MS) {
                btnLongFired[i] = true;
                if (userCallback) userCallback((ButtonId)i, BTN_EVENT_LONG_PRESSED);
            }
        }

        btnLastState[i] = reading;
    }
}
