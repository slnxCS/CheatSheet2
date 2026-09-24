#include "apps/builtin/ai_answer_app.h"
#include "services/ai_link.h"
#include "services/lang_service.h"
#include "drivers/input.h"
#include "fonts/fonts.h"
#include "ui/theme.h"
#include <cstring>
#include <cstdio>

// Чат-переписка: лента из истории ai_link (события фото + вопрос/ответ).
static lv_obj_t* lbl_state = nullptr;
static lv_obj_t* chat      = nullptr;
static lv_timer_t* timer   = nullptr;
static uint32_t seen_hseq  = 0;
static AiLinkState last_state = AI_LINK_IDLE;
static AiHistEntry tmp;             // копия одной записи вне критической секции

static const char* state_text(AiLinkState st) {
    switch (st) {
        case AI_LINK_PENDING:   return lang_str_ai_pending();
        case AI_LINK_TAKEN:     return lang_str_ai_taken();
        case AI_LINK_ANSWERED:  return lang_str_ai_answered();
        default:                return lang_str_ai_nothing();
    }
}

// Строка-«пузырь»: контейнер во всю ширину + label внутри,
// выравнивание задаёт flex-выравнивание контейнера.
static lv_obj_t* add_row(uint8_t kind, const char* text) {
    lv_obj_t* row = lv_obj_create(chat);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_radius(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    const bool is_q = (kind == 1 && text && text[0]);
    lv_obj_set_flex_align(row,
        kind == 0 ? LV_FLEX_ALIGN_CENTER : (is_q ? LV_FLEX_ALIGN_END : LV_FLEX_ALIGN_START),
        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t* b = lv_label_create(row);
    lv_label_set_long_mode(b, LV_LABEL_LONG_WRAP);
    lv_label_set_text(b, text ? text : "");
    lv_obj_set_style_text_font(b, &lv_font_cyr_14, 0);
    lv_obj_set_style_radius(b, 8, 0);
    lv_obj_set_style_pad_all(b, 6, 0);
    lv_obj_set_width(b, kind == 0 ? LV_SIZE_CONTENT : LV_PCT(86));

    if (kind == 0) {          // событие фото — приглушённое, по центру
        lv_obj_set_style_bg_color(b, lv_color_hex(0x0F2035), 0);
        lv_obj_set_style_text_color(b, theme_color_text_muted(), 0);
    } else if (is_q) {        // вопрос — справа, акцентный фон
        lv_obj_set_style_bg_color(b, lv_color_hex(0x1B4B7A), 0);
        lv_obj_set_style_text_color(b, lv_color_white(), 0);
    } else {                  // ответ — слева, панельный фон
        lv_obj_set_style_bg_color(b, lv_color_hex(0x16283F), 0);
        lv_obj_set_style_text_color(b, lv_color_white(), 0);
    }
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    return row;
}

static void rebuild_chat() {
    if (!chat) return;
    lv_obj_clean(chat);

    int cnt = ai_link_history_count();
    lv_obj_t* last = nullptr;
    for (int i = 0; i < cnt; i++) {
        if (!ai_link_history_get(i, &tmp)) continue;
        if (tmp.type == 0) {
            char line[96];
            snprintf(line, sizeof(line), "%s  %s  %s",
                     LV_SYMBOL_IMAGE, lang_str_camera_captured(), tmp.time);
            last = add_row(0, line);
        } else {
            if (tmp.q[0]) {
                char line[AI_HIST_Q + 16];
                snprintf(line, sizeof(line), "%s: %s", lang_str_ai_you(), tmp.q);
                last = add_row(1, line);
            }
            last = add_row(2, tmp.a[0] ? tmp.a : "…");
        }
    }
    if (!last) {
        last = add_row(0, lang_str_ai_nothing());
    }
    lv_obj_scroll_to_view(last, LV_ANIM_OFF);   // вниз к последнему
}

static void refresh(lv_timer_t*) {
    AiLinkState st = ai_link_state();
    if (st != last_state) {
        last_state = st;
        if (lbl_state) lv_label_set_text(lbl_state, state_text(st));
    }

    uint32_t hseq = ai_link_history_seq();
    if (hseq != seen_hseq) {
        seen_hseq = hseq;
        rebuild_chat();
    }
}

void ai_answer_app_open(lv_obj_t* parent) {
    lbl_state = nullptr;
    chat = nullptr;
    seen_hseq = 0;
    last_state = AI_LINK_IDLE;

    lv_obj_set_style_bg_color(parent, lv_color_hex(0x0A1628), 0);

    // Шапка
    lv_obj_t* header = lv_obj_create(parent);
    lv_obj_set_size(header, LV_PCT(100), 32);
    lv_obj_set_style_bg_color(header, theme_color_panel(), 0);
    lv_obj_set_style_bg_opa(header, LV_OPA_80, 0);
    lv_obj_set_style_border_width(header, 0, 0);
    lv_obj_set_style_radius(header, 0, 0);
    lv_obj_align(header, LV_ALIGN_TOP_MID, 0, 0);

    lv_obj_t* lbl_title = lv_label_create(header);
    lv_label_set_text_fmt(lbl_title, "%s %s",
                          LV_SYMBOL_ENVELOPE, lang_str_app_ai());
    lv_obj_set_style_text_color(lbl_title, theme_color_text(), 0);
    lv_obj_set_style_text_font(lbl_title, &lv_font_cyr_14, 0);
    lv_obj_align(lbl_title, LV_ALIGN_CENTER, 0, 0);

    // Строка состояния цепочки
    lbl_state = lv_label_create(parent);
    lv_obj_set_width(lbl_state, LV_PCT(94));
    lv_label_set_long_mode(lbl_state, LV_LABEL_LONG_WRAP);
    lv_label_set_text(lbl_state, state_text(AI_LINK_IDLE));
    lv_obj_set_style_text_color(lbl_state, theme_color_accent(), 0);
    lv_obj_set_style_text_font(lbl_state, &lv_font_cyr_14, 0);
    lv_obj_set_style_text_align(lbl_state, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(lbl_state, LV_ALIGN_TOP_MID, 0, 34);

    // Лента чата
    chat = lv_obj_create(parent);
    lv_obj_set_size(chat, LV_PCT(94), 240 - 60);
    lv_obj_align(chat, LV_ALIGN_TOP_MID, 0, 54);
    lv_obj_set_style_bg_color(chat, lv_color_hex(0x08111F), 0);
    lv_obj_set_style_bg_opa(chat, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(chat, 0, 0);
    lv_obj_set_style_radius(chat, 10, 0);
    lv_obj_set_style_pad_all(chat, 5, 0);
    lv_obj_set_style_pad_row(chat, 4, 0);
    lv_obj_set_flex_flow(chat, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(chat, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_scrollbar_mode(chat, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_remove_flag(chat, LV_OBJ_FLAG_SCROLL_ELASTIC);

    timer = lv_timer_create(refresh, 300, nullptr);
    refresh(timer);   // сразу показать текущую переписку
}

void ai_answer_app_close() {
    if (timer) {
        lv_timer_del(timer);
        timer = nullptr;
    }
    lbl_state = nullptr;
    chat = nullptr;
}

void ai_answer_app_button(int button_id, int event) {
    if (event != BTN_EVENT_CLICKED || !chat) return;

    int page = (int)lv_obj_get_height(chat) - 30;
    if (page < 40) page = 40;

    if (button_id == BTN_ID_UP) {
        lv_obj_scroll_by(chat, 0, -page, LV_ANIM_ON);
    } else if (button_id == BTN_ID_DOWN) {
        lv_obj_scroll_by(chat, 0, page, LV_ANIM_ON);
    }
    // LEFT/RIGHT — лента быстрее не прокрутить; OK-long выходит (main.cpp)
}
