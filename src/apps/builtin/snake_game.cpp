#include "fonts/fonts.h"
#include "apps/builtin/snake_game.h"
#include "drivers/input.h"
#include "services/lang_service.h"
#include "ui/theme.h"
#include <Arduino.h>
#include <cstring>

// --- Grid ---
#define CELL 12
#define COLS 26
#define ROWS 15
#define MAX_SNAKE (COLS * ROWS)

// Canvas: 320 × 210 (полная высота под шапкой)
// Сетка: 312 × 180, по центру
#define GAME_W (COLS * CELL)   // 312
#define GAME_H (ROWS * CELL)   // 180
#define CANVAS_H 210
#define OFFSET_X ((320 - GAME_W) / 2)  // 4
#define OFFSET_Y ((CANVAS_H - GAME_H) / 2)  // 15

// Colors (RGB565)
#define COL_BG      0x0000  // black
#define COL_GRID    0x0842  // dark gray grid
#define COL_SNAKE   0x07E0  // green
#define COL_HEAD    0xFFFF  // white head
#define COL_FOOD    0xF800  // red
#define COL_TEXT    0xFFFF  // white

// --- State ---
enum GameState { STATE_MENU, STATE_PLAYING, STATE_OVER };

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

// Snake
struct Point { int8_t x, y; };
static Point snake[MAX_SNAKE];
static int snake_len = 0;
static int dir = 0;  // 0=right, 1=down, 2=left, 3=up
static int next_dir = 0;

// Food
static Point food;

// Direction vectors: right, down, left, up
static const int8_t DX[] = {1, 0, -1, 0};
static const int8_t DY[] = {0, 1, 0, -1};

// --- Canvas helpers ---
static inline uint16_t* canvas_pixel(int x, int y) {
    return (uint16_t*)(canvas_buf + y * canvas_stride) + x;
}

static void fill_rect(int x, int y, int w, int h, uint16_t color) {
    for (int row = 0; row < h; row++) {
        uint16_t* p = canvas_pixel(x, y + row);
        for (int col = 0; col < w; col++) {
            p[col] = color;
        }
    }
}

static void draw_cell(int cx, int cy, uint16_t color) {
    int x = OFFSET_X + cx * CELL;
    int y = OFFSET_Y + cy * CELL;
    fill_rect(x + 1, y + 1, CELL - 2, CELL - 2, color);
}

// --- Game logic ---
static void spawn_food() {
    // Collect empty cells
    bool occupied[COLS * ROWS] = {};
    for (int i = 0; i < snake_len; i++) {
        int idx = snake[i].y * COLS + snake[i].x;
        if (idx >= 0 && idx < COLS * ROWS) occupied[idx] = true;
    }

    int empty_count = COLS * ROWS - snake_len;
    if (empty_count <= 0) return;

    int target = random(0, empty_count);
    int cnt = 0;
    for (int y = 0; y < ROWS; y++) {
        for (int x = 0; x < COLS; x++) {
            if (!occupied[y * COLS + x]) {
                if (cnt == target) {
                    food.x = x;
                    food.y = y;
                    return;
                }
                cnt++;
            }
        }
    }
}

static void reset_game() {
    snake_len = 5;
    for (int i = 0; i < snake_len; i++) {
        snake[i].x = COLS / 2 - i;
        snake[i].y = ROWS / 2;
    }
    dir = 0;
    next_dir = 0;
    score = 0;
    spawn_food();
}

static void draw_game() {
    // Clear canvas
    fill_rect(0, 0, 320, CANVAS_H, COL_BG);

    // Grid dots
    for (int y = 0; y < ROWS; y++) {
        for (int x = 0; x < COLS; x++) {
            int px = OFFSET_X + x * CELL;
            int py = OFFSET_Y + y * CELL;
            canvas_pixel(px, py)[0] = COL_GRID;
        }
    }

    // Food (with glow)
    int fx = OFFSET_X + food.x * CELL;
    int fy = OFFSET_Y + food.y * CELL;
    fill_rect(fx, fy, CELL, CELL, COL_FOOD);
    // Bright center
    fill_rect(fx + 3, fy + 3, CELL - 6, CELL - 6, 0xFFFF);

    // Snake body
    for (int i = snake_len - 1; i >= 1; i--) {
        draw_cell(snake[i].x, snake[i].y, COL_SNAKE);
    }
    // Snake head
    draw_cell(snake[0].x, snake[0].y, COL_HEAD);

    lv_obj_invalidate(canvas_obj);
}

static void update_info() {
    if (!lbl_info) return;
    if (state == STATE_MENU) {
        lv_label_set_text_fmt(lbl_info, "%s %s",
            LV_SYMBOL_PLAY, "OK");
    } else if (state == STATE_PLAYING) {
        lv_label_set_text_fmt(lbl_info, "Score: %d   Best: %d", score, high_score);
    } else {
        lv_label_set_text_fmt(lbl_info, "%s %s  |  %s",
            LV_SYMBOL_WARNING, "GAME OVER", "OK");
    }
}

// --- Timer callback ---
static void game_tick(lv_timer_t* t) {
    if (state != STATE_PLAYING) return;

    // Apply direction
    dir = next_dir;

    // New head position
    Point new_head;
    new_head.x = snake[0].x + DX[dir];
    new_head.y = snake[0].y + DY[dir];

    // Wall collision
    if (new_head.x < 0 || new_head.x >= COLS ||
        new_head.y < 0 || new_head.y >= ROWS) {
        state = STATE_OVER;
        if (score > high_score) high_score = score;
        update_info();
        return;
    }

    // Self collision
    for (int i = 0; i < snake_len; i++) {
        if (snake[i].x == new_head.x && snake[i].y == new_head.y) {
            state = STATE_OVER;
            lv_obj_set_hidden(lbl_info, false);
                lv_obj_set_hidden(menu_text, false);
            if (score > high_score) high_score = score;
            update_info();
            return;
        }
    }

    // Check food
    bool ate = (new_head.x == food.x && new_head.y == food.y);

    // Move body
    if (!ate) {
        // Shift tail
        for (int i = snake_len - 1; i > 0; i--) {
            snake[i] = snake[i - 1];
        }
    } else {
        // Grow: shift everything, keep tail
        if (snake_len < MAX_SNAKE) {
            for (int i = snake_len; i > 0; i--) {
                snake[i] = snake[i - 1];
            }
            snake_len++;
        }
        score += 10;
        spawn_food();
    }

    snake[0] = new_head;

    draw_game();
    update_info();
}

// --- Public API ---
void snake_game_open(lv_obj_t* parent) {
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
    lv_label_set_text_fmt(lbl_title, "%s Snake", LV_SYMBOL_DUMMY);
    lv_obj_set_style_text_color(lbl_title, theme_color_text(), 0);
    lv_obj_set_style_text_font(lbl_title, &lv_font_cyr_14, 0);
    lv_obj_align(lbl_title, LV_ALIGN_CENTER, 0, 0);

    // Canvas for game
    canvas_stride = lv_draw_buf_width_to_stride(320, LV_COLOR_FORMAT_RGB565);
    size_t buf_size = canvas_stride * CANVAS_H;
    canvas_buf = (uint8_t*)ps_malloc(buf_size);
    if (!canvas_buf) canvas_buf = (uint8_t*)malloc(buf_size);
    if (!canvas_buf) return;
    memset(canvas_buf, 0, buf_size);

    canvas_obj = lv_canvas_create(parent);
    lv_canvas_set_buffer(canvas_obj, canvas_buf, 320, CANVAS_H, LV_COLOR_FORMAT_RGB565);
    lv_obj_align(canvas_obj, LV_ALIGN_TOP_MID, 0, 30);

    // Info label
    lbl_info = lv_label_create(parent);
    lv_obj_set_style_text_color(lbl_info, theme_color_text(), 0);
    lv_obj_set_style_text_font(lbl_info, &lv_font_cyr_12, 0);
    lv_obj_align(lbl_info, LV_ALIGN_BOTTOM_MID, 0, -2);

    // Show menu
    fill_rect(0, 0, 320, CANVAS_H, COL_BG);

    // Title text in center of game area
    menu_text = lv_label_create(parent);
    lv_label_set_text(menu_text, LV_SYMBOL_PLAY "\nSnake");
    lv_obj_set_style_text_color(menu_text, lv_color_hex(0x07E0), 0);
    lv_obj_set_style_text_font(menu_text, &lv_font_cyr_20, 0);
    lv_obj_set_style_text_align(menu_text, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(menu_text, LV_ALIGN_CENTER, 0, 10);

    lv_obj_t* hint = lv_label_create(parent);
    lv_label_set_text(hint, "OK");
    lv_obj_set_style_text_color(hint, theme_color_text_muted(), 0);
    lv_obj_set_style_text_font(hint, &lv_font_cyr_12, 0);
    lv_obj_align(hint, LV_ALIGN_CENTER, 0, 50);

    update_info();

    // Timer starts when game begins
    game_timer = lv_timer_create(game_tick, 150, nullptr);
    lv_timer_pause(game_timer);
}

void snake_game_close() {
    if (game_timer) {
        lv_timer_del(game_timer);
        game_timer = nullptr;
    }
    if (canvas_buf) { free(canvas_buf); canvas_buf = nullptr; }
    parent_ref = nullptr;
    canvas_obj = nullptr;
    lbl_info = nullptr;
}

void snake_game_button(int button_id, int event) {
    if (event != BTN_EVENT_CLICKED) return;

    if (state == STATE_MENU) {
        if (button_id == BTN_ID_OK) {
            lv_obj_set_hidden(lbl_info, true);
            lv_obj_set_hidden(menu_text, true);
            reset_game();
            state = STATE_PLAYING;
            update_info();
            draw_game();
            lv_timer_resume(game_timer);
        }
    } else if (state == STATE_PLAYING) {
        // Prevent 180° turns
        switch (button_id) {
            case BTN_ID_UP:    if (dir != 1) next_dir = 3; break;
            case BTN_ID_DOWN:  if (dir != 3) next_dir = 1; break;
            case BTN_ID_LEFT:  if (dir != 0) next_dir = 2; break;
            case BTN_ID_RIGHT: if (dir != 2) next_dir = 0; break;
            case BTN_ID_OK:
                // Pause
                lv_timer_pause(game_timer);
                lv_obj_set_hidden(lbl_info, false);
                lv_obj_set_hidden(menu_text, false);
                state = STATE_MENU;
                update_info();
                break;
        }
    } else if (state == STATE_OVER) {
        if (button_id == BTN_ID_OK) {
            lv_timer_pause(game_timer);
            state = STATE_MENU;
            update_info();
        }
    }
}
