#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

bool camera_init();
void camera_deinit();
bool camera_capture(uint8_t** buf, size_t* len);
void camera_release();
bool camera_is_ready();
