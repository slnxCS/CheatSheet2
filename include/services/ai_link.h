#pragma once

#include <stddef.h>
#include <stdint.h>

namespace fs { class FS; }

// Связь «устройство → телефон → ИИ»:
//   фото кладётся в очередь (ai_link_notify_photo из камеры),
//   телефон забирает его по HTTP (GET /api/photo),
//   присылает текстовый ответ (POST /api/answer),
//   ответ показывается на экране устройства (экран «ИИ»).
typedef enum {
    AI_LINK_IDLE = 0,      // фото не снималось
    AI_LINK_PENDING = 1,   // фото ждёт, когда телефон его заберёт
    AI_LINK_TAKEN = 2,     // телефон забрал, ждём ответ
    AI_LINK_ANSWERED = 3,  // ответ получен
} AiLinkState;

void ai_link_init();                       // обработчики + LVGL-таймер автооткрытия
void ai_link_ensure_server();              // запуск сервера, когда WiFi поднят
void ai_link_notify_photo(fs::FS* f, const char* path);  // вызывается из save_photo

AiLinkState ai_link_state();
uint32_t ai_link_photo_id();
uint32_t ai_link_answer_seq();             // № ответа (0 = ответов ещё не было)
size_t ai_link_answer_copy(char* out, size_t max);       // копия текста ответа
