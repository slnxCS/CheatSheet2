#include "services/battery_service.h"
#include <Arduino.h>
#include "esp_adc_cal.h"
#include "driver/adc.h"

// ============================================================
// GPIO0 = ADC1_CH1 — единственный рабочий ADC на этой плате
// (GPIO3 залипает на 4095, остальные ADC1 заняты)
//
// Делитель: Vbat --[100k]-- GPIO0 --[100k]-- GND
// ============================================================

#define BAT_GPIO          0
#define BAT_PIN           ADC1_CHANNEL_1

#ifndef BAT_DIVIDER_RATIO
#define BAT_DIVIDER_RATIO  2.0f
#endif

#define BAT_CALIBRATION    1.00f
#define VBAT_FULL          4.20f
#define VBAT_EMPTY         3.30f

static esp_adc_cal_characteristics_t adc_chars;
static float last_voltage = 0.0f;
static int last_percent = -1;
static bool inited = false;

void battery_init() {
    gpio_reset_pin((gpio_num_t)BAT_GPIO);
    gpio_set_direction((gpio_num_t)BAT_GPIO, GPIO_MODE_INPUT);
    gpio_set_pull_mode((gpio_num_t)BAT_GPIO, GPIO_FLOATING);

    adc1_config_width(ADC_WIDTH_BIT_12);
    adc1_config_channel_atten(BAT_PIN, ADC_ATTEN_DB_12);
    esp_adc_cal_characterize(ADC_UNIT_1, ADC_ATTEN_DB_12,
                             ADC_WIDTH_BIT_12, 1100, &adc_chars);

    inited = true;
    battery_get_percent();
    Serial.printf("Battery: GPIO%d V=%.2fV %d%%\n",
                  BAT_GPIO, last_voltage, last_percent);
}

float battery_get_voltage() {
    if (!inited) return 0.0f;

    gpio_set_pull_mode((gpio_num_t)BAT_GPIO, GPIO_FLOATING);

    uint32_t raw = 0;
    for (int i = 0; i < 16; i++) {
        raw += adc1_get_raw(BAT_PIN);
        delayMicroseconds(200);
    }
    raw /= 16;

    uint32_t v_mv = esp_adc_cal_raw_to_voltage(raw, &adc_chars);
    float v_adc = v_mv / 1000.0f;
    last_voltage = v_adc * BAT_DIVIDER_RATIO * BAT_CALIBRATION;

    return last_voltage;
}

int battery_get_percent() {
    float v = battery_get_voltage();

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
    if (!inited) return false;
    if (last_voltage < 1.0f) return false;
    if (last_voltage > 4.35f) return true;
    return false;
}
