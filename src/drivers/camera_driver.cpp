#include "drivers/camera_driver.h"
#include "config/pins.h"
#include <Arduino.h>

#ifdef BOARD_HAS_PSRAM
#include "esp_camera.h"

static bool cam_ready = false;
static camera_fb_t* current_fb = nullptr;

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
        config.frame_size = FRAMESIZE_SVGA;  // 800x600 — достаточно для текста, быстро
        config.jpeg_quality = 12;
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
        s->set_saturation(s, -1);    // slightly less saturation to reduce color artifacts
        s->set_contrast(s, 1);       // slightly more contrast for text readability
        s->set_sharpness(s, 2);      // sharpen for text
        s->set_denoise(s, 1);        // reduce noise
        s->set_awb_gain(s, 1);       // enable AWB gain
        s->set_wb_mode(s, 0);        // auto white balance
        s->set_exposure_ctrl(s, 1);  // auto exposure
        s->set_aec2(s, 1);           // AEC DSP
        s->set_gain_ctrl(s, 1);      // auto gain
        s->set_agc_gain(s, 0);       // no manual gain
        s->set_gainceiling(s, (gainceiling_t)6);
        s->set_bpc(s, 1);            // dead pixel correction (fixes green dots)
        s->set_wpc(s, 1);            // white pixel correction
        s->set_hmirror(s, 1);        // mirror horizontally (sensor is rotated on board)
        s->set_vflip(s, 1);          // flip vertically (sensor is rotated on board)
    }

    cam_ready = true;
    Serial.println("Camera OK");
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

#else

bool camera_init() {
    Serial.println("Camera not available without PSRAM");
    return false;
}
void camera_deinit() {}
bool camera_capture(uint8_t** buf, size_t* len) { return false; }
void camera_release() {}
bool camera_is_ready() { return false; }

#endif
