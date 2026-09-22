#include "fonts/fonts.h"
#include "apps/builtin/flappy_game.h"
#include "drivers/input.h"
#include "ui/theme.h"
#include <Arduino.h>
#include <cstring>

// --- Layout ---
#define CANVAS_W   320
#define CANVAS_H   210
#define BIRD_X     60
#define BIRD_S     12
#define PIPE_W     26
#define GAP_H      64
#define GROUND_Y   198
#define PIPE_SPACING 150
#define MAX_PIPES  8

// Physics (tick = 40ms)
#define GRAVITY    1
#define MAX_FALL   6
#define FLAP_VY    (-4)

// Colors (RGB565)
#define COL_SKY    0x4C9B  // sky blue
#define COL_GROUND 0x05A0  // grass green
#define COL_GLINE  0xFFFF  // ground line
#define COL_PIPE   0x05E0  // pipe green
#define COL_PIPEC  0x03E0  // pipe dark edge
#define COL_BIRD   0xFFE0  // yellow
#define COL_EYE    0x0000  // black
#define COL_BEAK   0xFD20  // orange

// --- State ---
enum GameState { STATE_MENU, STATE_PLAYING, STATE_OVER };

struct Pipe {
    int16_t x;
    int16_t gap_y;
    bool scored;
};

static lv_obj_t* parent_ref = nullptr;
static lv_obj_t* canvas_obj = nullptr;
static lv_obj_t* lbl_info = nullptr;
static lv_obj_t* menu_text = nullptr;
static lv_obj_t* hint = nullptr;
static lv_timer_t* game_timer = nullptr;
static uint8_t* canvas_buf = nullptr;
static uint32_t canvas_stride = 0;

static GameState state = STATE_MENU;
static int score = 0;
static int high_score = 0;

static int bird_y = 0;
static int bird_vy = 0;
static Pipe pipes[MAX_PIPES];
static int pipe_count = 0;

// --- Canvas helpers ---
static inline uint16_t* canvas_pixel(int x, int y) {
    return (uint16_t*)(canvas_buf + y * canvas_stride) + x;
}

static void fill_rect(int x, int y, int w, int h, uint16_t color) {
    if (w <= 0 || h <= 0) return;
    if (y < 0) { h += y; y = 0; }
    if (y + h > CANVAS_H) h = CANVAS_H - y;
    // Обрезать по границам canvas (трубы могут вылезать за экран)
    int x0 = x < 0 ? 0 : x;
    int w0 = x + w - x0;
    if (w0 > CANVAS_W - x0) w0 = CANVAS_W - x0;
    if (h <= 0 || w0 <= 0) return;
    for (int row = 0; row < h; row++) {
        uint16_t* p = canvas_pixel(x0, y + row);
        for (int col = 0; col < w0; col++) p[col] = color;
    }
}

static void spawn_pipe(int x) {
    if (pipe_count >= MAX_PIPES) return;
    int gap_y = random(30, GROUND_Y - GAP_H - 20);
    pipes[pipe_count++] = {(int16_t)x, (int16_t)gap_y, false};
}

static void reset_game() {
    bird_y = (GROUND_Y - BIRD_S) / 2;
    bird_vy = 0;
    score = 0;
    pipe_count = 0;
    spawn_pipe(340);  // первая труба заранее, есть время подготовиться
}

static bool check_collision() {
    if (bird_y + BIRD_S > GROUND_Y) return true;
    if (bird_y < 0) { bird_y = 0; bird_vy = 0; }

    for (int i = 0; i < pipe_count; i++) {
        const Pipe& p = pipes[i];
        if (BIRD_X < p.x + PIPE_W && BIRD_X + BIRD_S > p.x) {
            if (bird_y < p.gap_y || bird_y + BIRD_S > p.gap_y + GAP_H) {
                return true;
            }
        }
    }
    return false;
}

static void game_tick(lv_timer_t* t) {
    (void)t;
    if (state != STATE_PLAYING) return;

    // Bird physics
    bird_vy += GRAVITY;
    if (bird_vy > MAX_FALL) bird_vy = MAX_FALL;
    bird_y += bird_vy;

    // Move pipes, remove offscreen, score
    for (int i = 0; i < pipe_count; i++) {
        pipes[i].x -= 3;
        if (!pipes[i].scored && BIRD_X > pipes[i].x + PIPE_W) {
            pipes[i].scored = true;
            score++;
        }
    }
    while (pipe_count > 0 && pipes[0].x + PIPE_W < 0) {
        memmove(&pipes[0], &pipes[1], (pipe_count - 1) * sizeof(Pipe));
        pipe_count--;
    }

    // Spawn: when rightmost pipe leaves enough space
    int maxx = -1000;
    for (int i = 0; i < pipe_count; i++) {
        if (pipes[i].x > maxx) maxx = pipes[i].x;
    }
    if (maxx <= CANVAS_W - PIPE_SPACING) {
        spawn_pipe(pipe_count == 0 ? CANVAS_W + 20 : maxx + PIPE_SPACING);
    }

    if (check_collision()) {
        state = STATE_OVER;
        if (score > high_score) high_score = score;
        lv_timer_pause(game_timer);
        if (lbl_info) {
            lv_label_set_text_fmt(lbl_info, "%s GAME OVER  |  OK",
                                  LV_SYMBOL_WARNING);
        }
        if (menu_text) {
            lv_label_set_text_fmt(menu_text, "GAME OVER\nScore: %d", score);
            lv_obj_set_style_text_color(menu_text, lv_color_hex(0xF800), 0);
            lv_obj_set_hidden(menu_text, false);
            lv_obj_set_hidden(hint, false);
        }
    }

    // Draw
    fill_rect(0, 0, CANVAS_W, CANVAS_H, COL_SKY);

    // Pipes
    for (int i = 0; i < pipe_count; i++) {
        const Pipe& p = pipes[i];
        // Top
        fill_rect(p.x, 0, PIPE_W, p.gap_y, COL_PIPE);
        fill_rect(p.x, 0, 3, p.gap_y, COL_PIPEC);
        fill_rect(p.x + PIPE_W - 3, 0, 3, p.gap_y, COL_PIPEC);
        // Bottom
        int by = p.gap_y + GAP_H;
        fill_rect(p.x, by, PIPE_W, GROUND_Y - by, COL_PIPE);
        fill_rect(p.x, by, 3, GROUND_Y - by, COL_PIPEC);
        fill_rect(p.x + PIPE_W - 3, by, 3, GROUND_Y - by, COL_PIPEC);
    }

    // Ground
    fill_rect(0, GROUND_Y, CANVAS_W, CANVAS_H - GROUND_Y, COL_GROUND);
    fill_rect(0, GROUND_Y, CANVAS_W, 2, COL_GLINE);

    // Bird
    fill_rect(BIRD_X, bird_y, BIRD_S, BIRD_S, COL_BIRD);
    fill_rect(BIRD_X + BIRD_S - 5, bird_y + 3, 3, 3, COL_EYE);       // eye
    fill_rect(BIRD_X + BIRD_S, bird_y + 6, 4, 3, COL_BEAK);          // beak
    // Wing hint
    fill_rect(BIRD_X + 2, bird_y + 6, 5, 3, COL_BEAK);

    lv_obj_invalidate(canvas_obj);

    if (state == STATE_PLAYING && lbl_info) {
        lv_label_set_text_fmt(lbl_info, "Score: %d  Best: %d", score, high_score);
    }
}

static void show_menu_overlay(bool game_over) {
    if (!menu_text || !hint) return;
    if (game_over) {
        lv_label_set_text_fmt(menu_text, "GAME OVER\nScore: %d", score);
        lv_obj_set_style_text_color(menu_text, lv_color_hex(0xF800), 0);
    } else {
        lv_label_set_text(menu_text, LV_SYMBOL_PLAY "\nFlappy");
        lv_obj_set_style_text_color(menu_text, lv_color_hex(0xFFE0), 0);
    }
    lv_label_set_text(hint, "OK");
    lv_obj_set_hidden(menu_text, false);
    lv_obj_set_hidden(hint, false);
}

static void update_info() {
    if (!lbl_info) return;
    if (state == STATE_MENU) {
        lv_label_set_text_fmt(lbl_info, "%s OK", LV_SYMBOL_PLAY);
    } else {
        lv_label_set_text_fmt(lbl_info, "Score: %d  Best: %d", score, high_score);
    }
}

static void start_game() {
    reset_game();
    state = STATE_PLAYING;
    lv_obj_set_hidden(menu_text, true);
    lv_obj_set_hidden(hint, true);
    update_info();
    lv_timer_resume(game_timer);
}

static void flap() {
    bird_vy = FLAP_VY;
}

// --- Public API ---
void flappy_game_open(lv_obj_t* parent) {
    parent_ref = parent;
    state = STATE_MENU;
    score = 0;

    lv_obj_set_style_bg_color(parent, lv_color_hex(0x0A0A1A), 0);

    // Header
    lv_obj_t* header = lv_obj_create(parent);
    lv_obj_set_size(header, LV_PCT(100), 28);
    lv_obj_set_style_bg_color(header, theme_color_panel(), 0);
    lv_obj_set_style_bg_opa(header, LV_OPA_80, 0);
    lv_obj_set_style_border_width(header, 0, 0);
    lv_obj_set_style_radius(header, 0, 0);
    lv_obj_align(header, LV_ALIGN_TOP_MID, 0, 0);

    lv_obj_t* lbl_title = lv_label_create(header);
    lv_label_set_text(lbl_title, "Flappy");
    lv_obj_set_style_text_color(lbl_title, theme_color_text(), 0);
    lv_obj_set_style_text_font(lbl_title, &lv_font_cyr_14, 0);
    lv_obj_align(lbl_title, LV_ALIGN_CENTER, 0, 0);

    // Canvas
    canvas_stride = lv_draw_buf_width_to_stride(CANVAS_W, LV_COLOR_FORMAT_RGB565);
    size_t buf_size = canvas_stride * CANVAS_H;
    canvas_buf = (uint8_t*)ps_malloc(buf_size);
    if (!canvas_buf) canvas_buf = (uint8_t*)malloc(buf_size);
    if (!canvas_buf) return;
    memset(canvas_buf, 0, buf_size);

    canvas_obj = lv_canvas_create(parent);
    lv_canvas_set_buffer(canvas_obj, canvas_buf, CANVAS_W, CANVAS_H, LV_COLOR_FORMAT_RGB565);
    lv_obj_align(canvas_obj, LV_ALIGN_TOP_MID, 0, 30);

    // Info label
    lbl_info = lv_label_create(parent);
    lv_obj_set_style_text_color(lbl_info, theme_color_text(), 0);
    lv_obj_set_style_text_font(lbl_info, &lv_font_cyr_12, 0);
    lv_obj_align(lbl_info, LV_ALIGN_BOTTOM_MID, 0, -2);

    // Menu overlay
    menu_text = lv_label_create(parent);
    lv_obj_set_style_text_align(menu_text, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(menu_text, &lv_font_cyr_20, 0);
    lv_obj_align(menu_text, LV_ALIGN_CENTER, 0, 10);

    hint = lv_label_create(parent);
    lv_obj_set_style_text_color(hint, theme_color_text_muted(), 0);
    lv_obj_set_style_text_font(hint, &lv_font_cyr_12, 0);
    lv_obj_align(hint, LV_ALIGN_CENTER, 0, 50);

    // Initial scene (static menu backdrop)
    reset_game();
    fill_rect(0, 0, CANVAS_W, CANVAS_H, COL_SKY);
    fill_rect(0, GROUND_Y, CANVAS_W, CANVAS_H - GROUND_Y, COL_GROUND);
    fill_rect(0, GROUND_Y, CANVAS_W, 2, COL_GLINE);
    fill_rect(BIRD_X, bird_y, BIRD_S, BIRD_S, COL_BIRD);
    lv_obj_invalidate(canvas_obj);

    show_menu_overlay(false);
    update_info();

    game_timer = lv_timer_create(game_tick, 40, nullptr);
    lv_timer_pause(game_timer);
}

void flappy_game_close() {
    if (game_timer) {
        lv_timer_del(game_timer);
        game_timer = nullptr;
    }
    if (canvas_buf) { free(canvas_buf); canvas_buf = nullptr; }
    parent_ref = nullptr;
    canvas_obj = nullptr;
    lbl_info = nullptr;
    menu_text = nullptr;
    hint = nullptr;
    state = STATE_MENU;
}

void flappy_game_button(int button_id, int event) {
    // Взмах в момент НАЖАТИЯ (PRESSED) — реакция без задержки отпускания
    if (event == BTN_EVENT_PRESSED) {
        if (state == STATE_PLAYING && button_id == BTN_ID_OK) flap();
        return;
    }

    if (event != BTN_EVENT_CLICKED) return;

    if (state == STATE_MENU) {
        if (button_id == BTN_ID_OK) start_game();
        return;
    }

    if (state == STATE_OVER) {
        if (button_id == BTN_ID_OK) start_game();
        return;
    }
}
