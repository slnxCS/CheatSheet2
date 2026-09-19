#pragma once

#define USER_SETUP_INFO "User_Setup"

// --- Driver ---
#define ST7789_DRIVER

// --- Display size (portrait) ---
#define TFT_WIDTH  240
#define TFT_HEIGHT 320

// --- SPI pins (HSPI via GPIO matrix) ---
#define TFT_MOSI   42
#define TFT_SCLK   41
#define TFT_CS     47
#define TFT_DC     46
#define TFT_RST    -1
#define TFT_BL     -1

// --- Color order: BGR for ST7789 ---
#define TFT_RGB_ORDER TFT_BGR

// --- SPI frequency ---
#define SPI_FREQUENCY  80000000
#define SPI_READ_FREQUENCY  20000000

// --- Fonts ---
#define LOAD_GLCD
#define LOAD_FONT2
#define LOAD_FONT4
#define LOAD_FONT6
#define LOAD_FONT7
#define LOAD_FONT8
#define LOAD_GFXFF
#define SMOOTH_FONT

// --- Use HSPI port ---
#define USE_HSPI_PORT
