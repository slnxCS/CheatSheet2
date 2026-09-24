#include "apps/builtin/ai_answer_app.h"
#include "apps/builtin/camera_app.h"
#include "services/ai_link.h"
#include "services/lang_service.h"
#include "drivers/input.h"
#include "fonts/fonts.h"
#include "ui/theme.h"
#include <cstring>
#include <cstdio>

// Чат-переписка: лента из истории ai_link + панель действий внизу:
// LEFT/RIGHT — выбор кнопки, OK — нажать («Сфоткать» снимает фото здесь же,
// «Отправить» просит телефон отправить фото в ИИ; ответ приходит в ленту).
static lv_obj_t* parent_ref = nullptr;
static lv_obj_t* lbl_state = nullptr;
static lv_obj_t* chat      = nullptr;
static lv_obj_t* btn_photo = nullptr;
static lv_obj_t* btn_send  = nullptr;
static lv_timer_t* timer   = nullptr;
static uint32_t seen_hseq  = 0;
static int focus_idx = 0;           // 0 = Сфоткать, 1 = Отправить
static bool capturing = false;      // идёт снимок — кнопки не реагируют
static AiLinkState last_state = AI_LINK_IDLE;
static AiHistEntry tmp;             // копия одной записи вне критической секции

// --- Плашка (как в камере, но своя) ---
static lv_obj_t* toast = nullptr;
static lv_obj_t* toast_lbl = nullptr;
static lv_timer_t* toast_timer = nullptr;

static void toast_hide() {
    if (toast_timer) { lv_timer_del(toast_timer); toast_timer = nullptr; }
    if (toast) {
        lv_obj_delete(toast);
        toast = nullptr;
        toast_lbl = nullptr;
    }
}

static void toast_autohide(lv_timer_t* t) {
    lv_timer_del(t);
    toast_timer = nullptr;
    toast_hide();
}

// ms = 0 — висит, пока не заменят; иначе сама скроется
static void toast_show(const char* text, uint32_t ms, lv_color_t bg) {
    if (!parent_ref) return;
    toast_hide();

    toast = lv_obj_create(parent_ref);
    lv_obj_set_size(toast, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(toast, 12, 0);
    lv_obj_set_style_border_width(toast, 0, 0);
    lv_obj_set_style_pad_all(toast, 12, 0);
    lv_obj_set_style_bg_opa(toast, LV_OPA_90, 0);
    lv_obj_set_scrollbar_mode(toast, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(toast, LV_OBJ_FLAG_SCROLLABLE);

    toast_lbl = lv_label_create(toast);
    lv_label_set_text(toast_lbl, text);
    lv_obj_set_style_text_color(toast_lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(toast_lbl, &lv_font_cyr_14, 0);
    lv_obj_set_style_text_align(toast_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_bg_color(toast, bg, 0);
    lv_obj_center(toast);

    if (ms > 0)
        toast_timer = lv_timer_create(toast_autohide, ms, nullptr);
}

static const char* state_text(AiLinkState st) {
    switch (st) {
        case AI_LINK_PENDING:   return lang_str_ai_pending();
        case AI_LINK_TAKEN:     return lang_str_ai_taken();
        case AI_LINK_ANSWERED:  return lang_str_ai_answered();
        default:                return lang_str_ai_nothing();
    }
}

// --- Строка-«пузырь»: контейнер во всю ширину + label внутри ---

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

// --- Панель действий ---

static void focus_style() {
    if (btn_photo)
        lv_obj_set_style_bg_color(btn_photo,
            focus_idx == 0 ? theme_color_accent() : lv_color_hex(0x16283F), 0);
    if (btn_send)
        lv_obj_set_style_bg_color(btn_send,
            focus_idx == 1 ? theme_color_accent() : lv_color_hex(0x16283F), 0);
}

static lv_obj_t* make_action_btn(lv_obj_t* parent, const char* text) {
    lv_obj_t* b = lv_button_create(parent);
    lv_obj_set_size(b, LV_PCT(48), 34);
    lv_obj_set_style_radius(b, 8, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x16283F), 0);
    lv_obj_set_style_shadow_width(b, 0, 0);

    lv_obj_t* l = lv_label_create(b);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, &lv_font_cyr_14, 0);
    lv_obj_set_style_text_color(l, lv_color_white(), 0);
    lv_obj_center(l);
    return b;
}

// Этапы снимка — показываем плашку между блокирующими операциями
static void capture_stage(int stage, unsigned arg) {
    char buf[80];
    lv_color_t bg = lv_color_hex(0x2C6FBF);
    switch (stage) {
        case 0:
            snprintf(buf, sizeof(buf), "%s %s",
                     LV_SYMBOL_REFRESH, lang_str_camera_init());
            break;
        case 1:
            snprintf(buf, sizeof(buf), "%s %s",
                     LV_SYMBOL_REFRESH, lang_str_camera_focusing());
            break;
        case 2:
            snprintf(buf, sizeof(buf), "%s %s",
                     LV_SYMBOL_IMAGE, lang_str_camera_captured());
            break;
        case 3:
            snprintf(buf, sizeof(buf), "%s %s (%u KB)",
                     LV_SYMBOL_OK, lang_str_camera_captured(), arg);
            bg = lv_color_hex(0x1B7F3B);
            break;
        default:
            return;
    }
    toast_show(buf, stage == 3 ? 1500 : 0, bg);
    lv_refr_now(lv_display_get_default());
}

static void action_capture() {
    if (capturing) return;
    capturing = true;
    bool ok = camera_app_capture_headless(capture_stage);
    capturing = false;
    if (!ok)
        toast_show(lang_str_camera_error(), 2000, lv_color_hex(0xB33A3A));
    // Успех: фото уже в истории чата (save_photo → ai_link_notify_photo),
    // лента обновится сама по hist_seq.
}

static void action_send() {
    if (capturing) return;
    if (ai_link_request_send()) {
        // Телефон при следующем опросе увидит рост send и сам унесёт фото в ИИ
        toast_show(lang_str_ai_waiting(), 2000, lv_color_hex(0x2C6FBF));
    } else {
        toast_show(lang_str_ai_no_photo(), 2000, lv_color_hex(0xB33A3A));
    }
}

// --- Жизненный цикл приложения ---

void ai_answer_app_open(lv_obj_t* parent) {
    parent_ref = parent;
    lbl_state = nullptr;
    chat = nullptr;
    btn_photo = nullptr;
    btn_send = nullptr;
    seen_hseq = 0;
    focus_idx = 0;
    capturing = false;
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

    // Лента чата (низ оставлен под панель действий)
    chat = lv_obj_create(parent);
    lv_obj_set_size(chat, LV_PCT(94), 142);
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

    // Панель действий: LEFT/RIGHT — выбор, OK — нажать
    lv_obj_t* bar = lv_obj_create(parent);
    lv_obj_set_size(bar, LV_PCT(94), 36);
    lv_obj_align(bar, LV_ALIGN_BOTTOM_MID, 0, -3);
    lv_obj_set_style_bg_opa(bar, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_set_style_pad_column(bar, 8, 0);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    char cap_txt[64], send_txt[64];
    snprintf(cap_txt, sizeof(cap_txt), "%s %s",
             LV_SYMBOL_IMAGE, lang_str_ai_take_photo());
    snprintf(send_txt, sizeof(send_txt), "%s %s",
             LV_SYMBOL_UPLOAD, lang_str_ai_send());
    btn_photo = make_action_btn(bar, cap_txt);
    btn_send = make_action_btn(bar, send_txt);
    focus_style();

    timer = lv_timer_create(refresh, 300, nullptr);
    refresh(timer);   // сразу показать текущую переписку
}

void ai_answer_app_close() {
    if (timer) {
        lv_timer_del(timer);
        timer = nullptr;
    }
    toast_hide();
    parent_ref = nullptr;
    lbl_state = nullptr;
    chat = nullptr;
    btn_photo = nullptr;
    btn_send = nullptr;
    capturing = false;
}

void ai_answer_app_button(int button_id, int event) {
    if (event != BTN_EVENT_CLICKED) return;

    if (button_id == BTN_ID_LEFT) {
        focus_idx = 0;
        focus_style();
        return;
    }
    if (button_id == BTN_ID_RIGHT) {
        focus_idx = 1;
        focus_style();
        return;
    }
    if (button_id == BTN_ID_OK) {
        if (focus_idx == 0) action_capture();
        else action_send();
        return;
    }
    if (!chat) return;

    int page = (int)lv_obj_get_height(chat) - 30;
    if (page < 40) page = 40;

    if (button_id == BTN_ID_UP) {
        lv_obj_scroll_by(chat, 0, -page, LV_ANIM_ON);
    } else if (button_id == BTN_ID_DOWN) {
        lv_obj_scroll_by(chat, 0, page, LV_ANIM_ON);
    }
}
