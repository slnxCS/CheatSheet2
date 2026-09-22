#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

bool camera_init();
void camera_deinit();
bool camera_capture(uint8_t** buf, size_t* len);
// Захватить кадр, снятый не ранее after_ms (мс с загрузки). Используется
// автофокусом: кадр обязан быть сделан уже после установки фокус-мотора.
// Если время кадра недоступно — ждёт период кадра, затем возвращает
// последний доступный кадр (не блокирует дольше ~0.8 с).
bool camera_capture_after(uint8_t** buf, size_t* len, uint64_t after_ms);
void camera_release();
bool camera_is_ready();
void camera_set_brightness(int val);
void camera_set_focus(int pos);
int camera_get_focus();
