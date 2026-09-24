#pragma once

#include <stddef.h>
#include <stdint.h>

namespace fs { class FS; }

// Связь «устройство → телефон → ИИ»:
//   фото кладётся в очередь (ai_link_notify_photo из камеры),
//   телефон забирает его по HTTP (GET /api/photo),
//   присылает чат-обмен (вопрос+ответ) через POST /api/chat,
//   экран «ИИ» показывает переписку (GET /api/history — для телефона).
typedef enum {
    AI_LINK_IDLE = 0,      // фото не снималось
    AI_LINK_PENDING = 1,   // фото ждёт, когда телефон его заберёт
    AI_LINK_TAKEN = 2,     // телефон забрал, ждём ответ
    AI_LINK_ANSWERED = 3,  // ответ получен
} AiLinkState;

// --- История чата (кольцо, сохраняется в LittleFS /ai_chat.log) ---
#define AI_HIST_MAX 16
#define AI_HIST_Q   256    // вопрос (в т.ч. прикреплённый промпт)
#define AI_HIST_A   1024   // ответ ИИ

typedef struct {
    uint8_t type;          // 0 = событие фото, 1 = вопрос+ответ
    char time[12];         // "12:34"
    char q[AI_HIST_Q];     // вопрос пользователя (может быть пустым)
    char a[AI_HIST_A];     // ответ ИИ (для type=0 пусто)
} AiHistEntry;

void ai_link_init();                       // история + обработчики + таймер
void ai_link_ensure_server();              // запуск сервера, когда WiFi поднят
void ai_link_notify_photo(fs::FS* f, const char* path);  // из save_photo

AiLinkState ai_link_state();
uint32_t ai_link_photo_id();
uint32_t ai_link_answer_seq();             // № ответа (для автооткрытия)

uint32_t ai_link_history_seq();            // растёт на каждое событие чата
int  ai_link_history_count();
bool ai_link_history_get(int idx, AiHistEntry* out);  // копия одной записи

// «Отправить» в чате: телефон при опросе /api/state увидит рост send_seq
// и сам заберёт фото → ИИ → ответ. false = фото ещё не снималось.
bool ai_link_request_send();
uint32_t ai_link_send_seq();
