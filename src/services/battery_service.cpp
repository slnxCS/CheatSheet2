#include "services/battery_service.h"
#include <Arduino.h>

// ============ БЕЗ ИЗМЕНЕНИЯ СХЕМЫ ============
// ESP32-S3 определяет наличие USB-питания через детекцию VBUS.
// Показывает: USB подключён (заряжается) / USB отключён (от батареи).
// Точного % заряда без делителя напряжения НЕТ — только статус.
// ============================================

// Пин VBUS detection на ESP32-S3 (встроенная детекция USB)
// На большинстве плат VBUS подключён к GPIO
#ifndef VBUS_PIN
#define VBUS_PIN  44   // Типичный пин для USB VBUS detect на ESP32-S3
#endif

static bool usb_connected = false;
static bool inited = false;

void battery_init() {
    // Пробуем детектировать USB через GPIO
    // Если пин недоступен — считаем что всегда на батарее
    pinMode(VBUS_PIN, INPUT_PULLDOWN);
    inited = true;

    usb_connected = digitalRead(VBUS_PIN);
    Serial.printf("Battery init: USB %s\n", usb_connected ? "connected" : "disconnected");
}

float battery_get_voltage() {
    // Нет делителя — не можем измерить реальное напряжение
    // Возвращаем0 если на батарее,5.0 если USB
    return usb_connected ? 5.0f : 0.0f;
}

int battery_get_percent() {
    // Без ADC-делителя точный процент невозможен
    // Возвращаем: 100 если USB, -1 если неизвестно (батарея без измерения)
    return usb_connected ? 100 : -1;
}

bool battery_is_usb_connected() {
    if (!inited) return false;
    usb_connected = digitalRead(VBUS_PIN);
    return usb_connected;
}
