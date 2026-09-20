#include "services/battery_service.h"
#include <Arduino.h>
#include "esp_adc_cal.h"
#include "driver/adc.h"

// ============================================================
// GPIO3 = ADC1_CH2
// Делитель: 5V(batt) --[100k]-- GPIO3 --[100k]-- GND
// Vbat = Vadc * 2.0
//
// Проблема: внутренняя подтяжка GPIO3 держит пин на 3.3V.
// Решение: отключаем pull-up через регистр IO_MUX.
// ============================================================

#define BAT_GPIO          0
#define BAT_PIN           ADC1_CHANNEL_0

// Регистр подтяжки GPIO3 на ESP32-S3
// IO_MUX_REG = 0x3FF49000 + (GPIO * 4)
// Bit 7: PU (pull-up enable) — сбросить в 0
// Bit 8: PD (pull-down enable) — сбросить в 0
// IO_MUX register access via direct pointer
// ESP32-S3: IO_MUX base = 0x3FF49000
// GPIO3 IO_MUX register = base + (3 * 4) = 0x3FF4900C
// Bit 7: PU (pull-up enable)
// Bit 8: PD (pull-down enable)
// IO_MUX register: base 0x3FF49000 + GPIO*4
// Bit 7: PU (pull-up)  Bit 8: PD (pull-down)
#define IO_MUX_BASE_ADDR  0x3FF49000UL
#define IO_MUX_GPIO_ADDR  (IO_MUX_BASE_ADDR + (BAT_GPIO * 4))
#define IO_MUX_PU_BIT     (1 << 7)
#define IO_MUX_PD_BIT     (1 << 8)

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

static void disable_pullups() {
    // Полный сброс пина
    gpio_reset_pin((gpio_num_t)BAT_GPIO);
    gpio_set_direction((gpio_num_t)BAT_GPIO, GPIO_MODE_INPUT);
    gpio_set_pull_mode((gpio_num_t)BAT_GPIO, GPIO_FLOATING);

    // Принудительно отключаем PU и PD через регистр IO_MUX
    volatile uint32_t* iomux = (volatile uint32_t*)IO_MUX_GPIO_ADDR;
    uint32_t reg = *iomux;
    reg &= ~(IO_MUX_PU_BIT | IO_MUX_PD_BIT);
    *iomux = reg;
}

void battery_init() {
    disable_pullups();

    // ADC напрямую
    adc1_config_width(ADC_WIDTH_BIT_12);
    adc1_config_channel_atten(BAT_PIN, ADC_ATTEN_DB_12);
    esp_adc_cal_characterize(ADC_UNIT_1, ADC_ATTEN_DB_12,
                             ADC_WIDTH_BIT_12, 1100, &adc_chars);

    inited = true;

    // Несколько тестовых чтений
    Serial.println("=== Battery ADC test ===");
    for (int i = 0; i < 5; i++) {
        uint32_t raw = adc1_get_raw(BAT_PIN);
        uint32_t v_mv = esp_adc_cal_raw_to_voltage(raw, &adc_chars);
        Serial.printf("  test[%d]: raw=%u v_mv=%u\n", i, raw, v_mv);
        delay(100);
    }
    Serial.println("=== end test ===");

    battery_get_voltage();
    battery_get_percent();
    Serial.printf("Battery: GPIO%d ratio=%.1f V=%.2fV %d%%\n",
                  BAT_GPIO, BAT_DIVIDER_RATIO, last_voltage, last_percent);
}

float battery_get_voltage() {
    if (!inited) return 0.0f;

    disable_pullups();  // на всякий случай перед каждым чтением

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
