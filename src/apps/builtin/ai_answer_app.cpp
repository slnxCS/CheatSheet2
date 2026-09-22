#include "apps/builtin/ai_answer_app.h"
#include "services/ai_link.h"
#include "services/lang_service.h"
#include "drivers/input.h"
#include "fonts/fonts.h"
#include "ui/theme.h"
#include <cstring>

static lv_obj_t* lbl_state = nullptr;
static lv_obj_t* lbl_text  = nullptr;
static lv_obj_t* box       = nullptr;
static lv_timer_t* timer   = nullptr;

static uint32_t seen_seq = 0;
static AiLinkState last_state = AI_LINK_IDLE;
static char answer_copy[4096];

static const char* state_text(AiLinkState st) {
    switch (st) {
        case AI_LINK_PENDING:   return lang_str_ai_pending();
        case AI_LINK_TAKEN:     return lang_str_ai_taken();
        case AI_LINK_ANSWERED:  return lang_str_ai_answered();
        default:                return lang_str_ai_nothing();
    }
}

static void refresh(lv_timer_t*) {
    AiLinkState st = ai_link_state();
    if (st != last_state) {
        last_state = st;
        lv_label_set_text(lbl_state, state_text(st));
    }

    uint32_t seq = ai_link_answer_seq();
    if (seq != seen_seq) {
        seen_seq = seq;
        size_t len = ai_link_answer_copy(answer_copy, sizeof(answer_copy));
        lv_label_set_text(lbl_text, len ? answer_copy : "—");
        lv_obj_scroll_to_y(box, 0, LV_ANIM_OFF);
    }
}

void ai_answer_app_open(lv_obj_t* parent) {
    lbl_state = nullptr;
    lbl_text = nullptr;
    box = nullptr;
    seen_seq = 0;
    last_state = AI_LINK_IDLE;
    answer_copy[0] = '\0';

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
                          LV_SYMBOL_EDIT, lang_str_app_ai());
    lv_obj_set_style_text_color(lbl_title, theme_color_text(), 0);
    lv_obj_set_style_text_font(lbl_title, &lv_font_cyr_14, 0);
    lv_obj_align(lbl_title, LV_ALIGN_CENTER, 0, 0);

    // Строка состояния цепочки
    lbl_state = lv_label_create(parent);
    lv_obj_set_width(lbl_state, LV_PCT(92));
    lv_label_set_long_mode(lbl_state, LV_LABEL_LONG_WRAP);
    lv_label_set_text(lbl_state, state_text(AI_LINK_IDLE));
    lv_obj_set_style_text_color(lbl_state, theme_color_accent(), 0);
    lv_obj_set_style_text_font(lbl_state, &lv_font_cyr_14, 0);
    lv_obj_set_style_text_align(lbl_state, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(lbl_state, LV_ALIGN_TOP_MID, 0, 38);

    // Прокручиваемая область с текстом ответа
    box = lv_obj_create(parent);
    lv_obj_set_size(box, LV_PCT(94), LV_PCT(100) - 76);
    lv_obj_align(box, LV_ALIGN_TOP_MID, 0, 64);
    lv_obj_set_style_bg_color(box, theme_color_panel(), 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_70, 0);
    lv_obj_set_style_border_width(box, 0, 0);
    lv_obj_set_style_radius(box, 12, 0);
    lv_obj_set_style_pad_all(box, 10, 0);
    lv_obj_set_scrollbar_mode(box, LV_SCROLLBAR_MODE_AUTO);

    lbl_text = lv_label_create(box);
    lv_obj_set_width(lbl_text, LV_PCT(100));
    lv_label_set_long_mode(lbl_text, LV_LABEL_LONG_WRAP);
    lv_label_set_text(lbl_text, "—");
    lv_obj_set_style_text_color(lbl_text, lv_color_white(), 0);
    lv_obj_set_style_text_font(lbl_text, &lv_font_cyr_20, 0);

    timer = lv_timer_create(refresh, 300, nullptr);
    refresh(timer);   // сразу показать текущий ответ/состояние
}

void ai_answer_app_close() {
    if (timer) {
        lv_timer_del(timer);
        timer = nullptr;
    }
    lbl_state = nullptr;
    lbl_text = nullptr;
    box = nullptr;
}

void ai_answer_app_button(int button_id, int event) {
    if (event != BTN_EVENT_CLICKED || !box) return;

    lv_obj_t* parent = lv_obj_get_parent(box);
    int page = (int)lv_obj_get_height(parent) - 64 - 30;
    if (page < 40) page = 40;

    if (button_id == BTN_ID_UP) {
        lv_obj_scroll_by(box, 0, -page, LV_ANIM_ON);
    } else if (button_id == BTN_ID_DOWN) {
        lv_obj_scroll_by(box, 0, page, LV_ANIM_ON);
    }
    // LEFT/RIGHT/OK — ничего (OK-long выходит из приложения в main.cpp)
}
