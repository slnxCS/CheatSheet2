#include "services/battery_service.h"
#include <Arduino.h>

// ============================================================
// Измерение напряжения аккумулятора через ADC
//
// Схема: Vbat ---[R1]---+---[R2]--- GND
//                        |
//                       ADC pin
//
// Vbat = Vadc * (R1 + R2) / R2
//
// Подстройте BAT_PIN и делитель под свою схему!
// ============================================================

// Пин ADC (GPIO3 = ADC1_CH2, свободен на этой плате)
#define BAT_PIN           3

// Напряжение опоры (11db atten → 0..3.3V)
#define ADC_VREF          3.30f
#define ADC_MAX           4095.0f

// Делитель: Vbat = Vadc * RATIO
// R1=R2=100k → RATIO=2.0
// R1=100k, R2=47k → RATIO=3.13
#ifndef BAT_DIVIDER_RATIO
#define BAT_DIVIDER_RATIO  2.0f
#endif

// Калибровка (измерьте мультиметром и подстройте)
#define BAT_CALIBRATION    1.00f

// LiPo
#define VBAT_FULL          4.20f
#define VBAT_EMPTY         3.30f

static float last_voltage = 0.0f;
static int last_percent = -1;
static bool inited = false;

void battery_init() {
    analogReadResolution(12);
    analogSetAttenuation(ADC_11db);
    pinMode(BAT_PIN, INPUT);

    inited = true;

    // Первое чтение
    battery_get_voltage();
    battery_get_percent();
    Serial.printf("Battery: pin=%d ratio=%.1f V=%.2fV %d%%\n",
                  BAT_PIN, BAT_DIVIDER_RATIO, last_voltage, last_percent);
}

float battery_get_voltage() {
    if (!inited) return 0.0f;

    // Среднее из 8 чтений
    uint32_t raw = 0;
    for (int i = 0; i < 8; i++) {
        raw += analogRead(BAT_PIN);
        delayMicroseconds(200);
    }
    raw /= 8;

    float v_adc = ((float)raw / ADC_MAX) * ADC_VREF;
    last_voltage = v_adc * BAT_DIVIDER_RATIO * BAT_CALIBRATION;

    Serial.printf("ADC raw=%u Vadc=%.3f Vbat=%.2f\n", raw, v_adc, last_voltage);
    return last_voltage;
}

int battery_get_percent() {
    float v = battery_get_voltage();

    // Если напряжение很低 (< 1V) — нет аккумулятора или нет делителя
    if (v < 1.0f) {
        last_percent = -1;
        return last_percent;
    }

    if (v >= VBAT_FULL) {
        last_percent = 100;
    } else if (v <= VBAT_EMPTY) {
        last_percent = 0;
    } else {
        float ratio = (v - VBAT_EMPTY) / (VBAT_FULL - VBAT_EMPTY);
        last_percent = (int)(ratio * 100.0f);
        if (last_percent < 0) last_percent = 0;
        if (last_percent > 100) last_percent = 100;
    }

    return last_percent;
}

bool battery_is_usb_connected() {
    // USB: напряжение выше满电 ( > 4.35V через делитель)
    // Или нет аккумулятора ( < 1V)
    if (!inited) return false;
    float v = last_voltage;
    if (v < 1.0f) return false;   // нет аккумулятора — неизвестно
    if (v > 4.35f) return true;   // выше满电 — USB/зарядка
    return false;
}
