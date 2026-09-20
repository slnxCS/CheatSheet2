#include "services/battery_service.h"
#include <Arduino.h>

// ============ ДЕТЕКЦИЯ USB ============
// На ESP32-S3 определяем USB через Serial (CDC)
// Если Serial подключён — USB есть.
// ======================================

static bool inited = false;
static bool last_usb_state = false;

void battery_init() {
    inited = true;
    // Проверяем USB подключение через CDC
    last_usb_state = Serial;
    Serial.printf("Battery init: USB %s\n", last_usb_state ? "connected" : "disconnected");
}

float battery_get_voltage() {
    return last_usb_state ? 5.0f : 0.0f;
}

int battery_get_percent() {
    return last_usb_state ? 100 : -1;
}

bool battery_is_usb_connected() {
    if (!inited) return false;
    last_usb_state = Serial;
    return last_usb_state;
}
