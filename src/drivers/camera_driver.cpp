#include "drivers/camera_driver.h"
#include "config/pins.h"
#include <Arduino.h>

#ifdef BOARD_HAS_PSRAM
#include "esp_camera.h"

// OV5640 VCM (Voice Coil Motor) focus registers
#define OV5640_MODULE_ID_ADDR   0x3000
#define OV5640_SC_PRE_BIAS_ADDR 0x5218
#define OV5640_SC_MAST_BIAS_ADDR 0x5217
#define OV5640_SC_LINE_ADDR     0x5216
#define OV5640_SC_STEP_ADDR     0x5215

static bool cam_ready = false;
static camera_fb_t* current_fb = nullptr;
static int current_focus = 512;

// Запись в регистр через SCCB (I2C камеры)
static void ov5640_write_reg(uint16_t reg, uint8_t val) {
    sensor_t* s = esp_camera_sensor_get();
    if (s) s->set_reg(s, reg, 0xFF, val);
}

static void ov5640_set_focus(int pos) {
    // OV5640 VCM control: разбиваем позицию на 3 регистра
    uint8_t h = (pos >> 8) & 0x03;
    uint8_t m = (pos >> 4) & 0x0F;
    uint8_t l = pos & 0x0F;

    ov5640_write_reg(0x5218, (h << 4) | l);  // SC_PRE_BIAS
    ov5640_write_reg(0x5217, m);              // SC_MAST_BIAS
    ov5640_write_reg(0x5216, 0x01);           // SC_LINE = 1 (manual mode)
    ov5640_write_reg(0x5215, 0x01);           // SC_STEP = 1 (enable direct control)

    Serial.printf("Focus set: %d\n", pos);
}

bool camera_init() {
    camera_config_t config = {};
    config.ledc_channel = LEDC_CHANNEL_0;
    config.ledc_timer = LEDC_TIMER_0;
    config.pin_d0 = PIN_CAM_D0;
    config.pin_d1 = PIN_CAM_D1;
    config.pin_d2 = PIN_CAM_D2;
    config.pin_d3 = PIN_CAM_D3;
    config.pin_d4 = PIN_CAM_D4;
    config.pin_d5 = PIN_CAM_D5;
    config.pin_d6 = PIN_CAM_D6;
    config.pin_d7 = PIN_CAM_D7;
    config.pin_xclk = PIN_CAM_XCLK;
    config.pin_pclk = PIN_CAM_PCLK;
    config.pin_vsync = PIN_CAM_VSYNC;
    config.pin_href = PIN_CAM_HREF;
    config.pin_sccb_sda = PIN_CAM_SDA;
    config.pin_sccb_scl = PIN_CAM_SCL;
    config.pin_pwdn = -1;
    config.pin_reset = -1;
    config.xclk_freq_hz = 20000000;
    config.pixel_format = PIXFORMAT_JPEG;

    if (psramFound()) {
        config.frame_size = FRAMESIZE_UXGA;  // 1600x1200
        config.jpeg_quality = 8;              // высокое качество (0-63)
        config.fb_count = 2;
        config.grab_mode = CAMERA_GRAB_LATEST;
        config.fb_location = CAMERA_FB_IN_PSRAM;
    } else {
        config.frame_size = FRAMESIZE_VGA;
        config.fb_location = CAMERA_FB_IN_DRAM;
        config.fb_count = 1;
    }

    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        Serial.printf("Camera init failed: 0x%x\n", err);
        cam_ready = false;
        return false;
    }

    sensor_t* s = esp_camera_sensor_get();
    if (s) {
        s->set_brightness(s, 0);
        s->set_saturation(s, -1);
        s->set_contrast(s, 1);
        s->set_sharpness(s, 2);
        s->set_denoise(s, 1);
        s->set_awb_gain(s, 1);
        s->set_wb_mode(s, 0);
        s->set_exposure_ctrl(s, 1);
        s->set_aec2(s, 1);
        s->set_gain_ctrl(s, 1);
        s->set_agc_gain(s, 0);
        s->set_gainceiling(s, (gainceiling_t)6);
        s->set_bpc(s, 1);
        s->set_wpc(s, 1);
        s->set_hmirror(s, 1);
        s->set_vflip(s, 1);
    }

    cam_ready = true;
    Serial.println("Camera OK (UXGA 1600x1200)");
    return true;
}

void camera_deinit() {
    esp_camera_deinit();
    cam_ready = false;
    current_fb = nullptr;
}

bool camera_capture(uint8_t** buf, size_t* len) {
    if (!cam_ready) return false;

    current_fb = esp_camera_fb_get();
    if (!current_fb) {
        Serial.println("Camera capture failed");
        return false;
    }

    *buf = current_fb->buf;
    *len = current_fb->len;
    return true;
}

void camera_release() {
    if (current_fb) {
        esp_camera_fb_return(current_fb);
        current_fb = nullptr;
    }
}

bool camera_is_ready() {
    return cam_ready;
}

void camera_set_brightness(int val) {
    if (!cam_ready) return;
    sensor_t* s = esp_camera_sensor_get();
    if (s) s->set_brightness(s, val);
}

void camera_set_focus(int pos) {
    if (!cam_ready) return;
    if (pos < 0) pos = 0;
    if (pos > 1023) pos = 1023;
    current_focus = pos;
    ov5640_set_focus(pos);
}

#else

bool camera_init() {
    Serial.println("Camera not available without PSRAM");
    return false;
}
void camera_deinit() {}
bool camera_capture(uint8_t** buf, size_t* len) { return false; }
void camera_release() {}
bool camera_is_ready() { return false; }
void camera_set_brightness(int val) {}
void camera_set_focus(int pos) {}

#endif
