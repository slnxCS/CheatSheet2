#include "services/battery_service.h"
#include <Arduino.h>

// ============================================================
// Измерение напряжения аккумулятора через ADC + делитель
//
// Схема: Vbat ---[R1]---+---[R2]--- GND
//                        |
//                       ADC pin
//
// Vbat = Vadc * (R1 + R2) / R2
//
// Подстройте BAT_PIN и делитель под свою схему!
// ============================================================

// Пин ADC (свободный GPIO). Используйте ADC1 каналы:
// GPIO3 (ADC1_CH2), GPIO4 (ADC1_CH3) — но GPIO4 занят камерой!
// Лучший вариант: GPIO3 если он свободен.
#define BAT_PIN           3

// Напряжение опоры ESP32-S3 ADC (11db atten → ~3.3V)
#define ADC_VREF          3.30f
#define ADC_RESOLUTION    4095.0f

// Делитель: если R1=R2=100k → RATIO=2.0
// Если R1=100k, R2=47k → RATIO≈3.13
// Если R1=200k, R2=100k → RATIO=3.0
#ifndef BAT_DIVIDER_RATIO
#define BAT_DIVIDER_RATIO  2.0f
#endif

// Калибровка (подстройте по мультиметру: измерьте Vbat и Vadc)
#define BAT_CALIBRATION    1.00f

// LiPo характеристика
#define VBAT_FULL          4.20f
#define VBAT_EMPTY         3.30f
#define VBAT_LOW           3.50f

static float last_voltage = 0.0f;
static int last_percent = -1;
static bool inited = false;

void battery_init() {
    // ADC: 12 бит, аттенюация 11db (0..3.3V)
    analogReadResolution(12);
    analogSetAttenuation(ADC_11db);
    pinMode(BAT_PIN, INPUT);

    inited = true;

    // Первое чтение
    battery_get_percent();
    Serial.printf("Battery init: pin=%d ratio=%.1f V=%.2fV %d%%\n",
                  BAT_PIN, BAT_DIVIDER_RATIO, last_voltage, last_percent);
}

float battery_get_voltage() {
    if (!inited) return 0.0f;

    // Среднее из 4 чтений для стабильности
    int raw = 0;
    for (int i = 0; i < 4; i++) {
        raw += analogRead(BAT_PIN);
        delayMicroseconds(100);
    }
    raw /= 4;

    float v_adc = (raw / ADC_RESOLUTION) * ADC_VREF;
    last_voltage = v_adc * BAT_DIVIDER_RATIO * BAT_CALIBRATION;

    return last_voltage;
}

int battery_get_percent() {
    float v = battery_get_voltage();

    if (v < 1.0f) {
        // Напряжение слишком низкое — скорее всего нет аккумулятора
        last_percent = -1;
        return last_percent;
    }

    if (v >= VBAT_FULL) {
        last_percent = 100;
    } else if (v <= VBAT_EMPTY) {
        last_percent = 0;
    } else {
        // Линейная интерполяция
        float ratio = (v - VBAT_EMPTY) / (VBAT_FULL - VBAT_EMPTY);
        last_percent = (int)(ratio * 100.0f);
        if (last_percent < 0) last_percent = 0;
        if (last_percent > 100) last_percent = 100;
    }

    return last_percent;
}

bool battery_is_usb_connected() {
    // USB определяем по напряжению: если >4.5V — зарядка/USB
    // Или по Serial CDC если доступен
    if (last_voltage > 4.5f) return true;
    return false;
}
