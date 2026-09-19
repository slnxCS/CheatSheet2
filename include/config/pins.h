#pragma once

// ============================================================
// CheatSheet2 Pin Definitions
// ESP32-S3-N16R8 CAM + ST7789 240x320 + 5 кнопок
//
// SD-карта на плате занимает: GPIO 38, 39, 40
// RST дисплея: tied to 3.3V ( pull-up 10k )
// ============================================================

// --- TFT Display (ST7789, HSPI через GPIO matrix) ---
// RST не подключён к GPIO — напрямую на 3.3V через 10k pull-up
#define PIN_TFT_MOSI    42
#define PIN_TFT_SCLK    41
#define PIN_TFT_CS      47
#define PIN_TFT_DC      46
#define PIN_TFT_BL      -1

// --- Кнопки (к GND, internal pull-up) ---
#define PIN_BTN_UP       1
#define PIN_BTN_DOWN     2
#define PIN_BTN_LEFT    14
#define PIN_BTN_RIGHT   45
#define PIN_BTN_OK      21

// --- Камера OV5640 (DVP, фиксирована на плате) ---
#define PIN_CAM_XCLK    15
#define PIN_CAM_SDA      4
#define PIN_CAM_SCL      5
#define PIN_CAM_VSYNC    6
#define PIN_CAM_HREF     7
#define PIN_CAM_PCLK    13
#define PIN_CAM_D0      11
#define PIN_CAM_D1       9
#define PIN_CAM_D2       8
#define PIN_CAM_D3      10
#define PIN_CAM_D4      12
#define PIN_CAM_D5      18
#define PIN_CAM_D6      17
#define PIN_CAM_D7      16

// --- SD-карта (распаяна на плате, НЕЛЬЗЯ использовать) ---
// GPIO 38 = SD_CS
// GPIO 39 = SD_CLK
// GPIO 40 = SD_MOSI
