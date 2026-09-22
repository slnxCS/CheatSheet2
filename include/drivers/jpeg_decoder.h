#pragma once

#include <stdint.h>
#include <stddef.h>

/**
 * Open JPEG and report its native dimensions (without decoding).
 */
bool jpeg_open(const uint8_t* jpeg_data, size_t jpeg_len, int* w, int* h);
void jpeg_close();

/**
 * Decode JPEG data to RGB565 buffer.
 * If transpose = true, output is transposed (rows/cols swapped) —
 * matches camera preview orientation.
 */
bool jpeg_decode_to_rgb565(const uint8_t* jpeg_data, size_t jpeg_len,
                            uint8_t* out_buf, int out_w, int out_h,
                            uint32_t out_stride = 0, int scale = 0,
                            int* actual_w = nullptr, int* actual_h = nullptr,
                            bool transpose = false);
