#include "fonts/fonts.h"
#include "apps/builtin/tetris_game.h"
#include "drivers/input.h"
#include "ui/theme.h"
#include <Arduino.h>
#include <cstring>

// --- Layout ---
// Поле 10x20 (ячейка 10px = 100x200) + панель "NEXT" 60px,
// всё по центру canvas 320x210
#define TCELL      10
#define FCOLS      10
#define FROWS      20
#define CANVAS_W   320
#define CANVAS_H   210
#define PANEL_W    60
#define PANEL_GAP  20
#define FIELD_X    ((CANVAS_W - (FCOLS * TCELL + PANEL_GAP + PANEL_W)) / 2)  // 70
#define FIELD_Y    ((CANVAS_H - FROWS * TCELL) / 2)                          // 5
#define NEXT_X     (FIELD_X + FCOLS * TCELL + PANEL_GAP)                     // 190
#define NEXT_Y     65

// Colors (RGB565)
#define COL_BG       0x0000  // black
#define COL_FIELD    0x1082  // dark field
#define COL_BORDER   0x39C7  // panel border
#define COL_GRID     0x18C3  // grid

// Piece palette (index 1..7)
static const uint16_t PAL[8] = {
    0x0000,
    0x07FF,  // I cyan
    0xFFE0,  // O yellow
    0xF81F,  // T magenta
    0x07E0,  // S green
    0xF800,  // Z red
    0x001F,  // J blue
    0xFD20   // L orange
};

// Base shapes: I O T S Z J L
static const uint8_t SHAPES[7][4][4] = {
    {{0,0,0,0},{1,1,1,1},{0,0,0,0},{0,0,0,0}},  // I
    {{0,1,1,0},{0,1,1,0},{0,0,0,0},{0,0,0,0}},  // O
    {{0,1,0,0},{1,1,1,0},{0,0,0,0},{0,0,0,0}},  // T
    {{0,1,1,0},{1,1,0,0},{0,0,0,0},{0,0,0,0}},  // S
    {{1,1,0,0},{0,1,1,0},{0,0,0,0},{0,0,0,0}},  // Z
    {{1,0,0,0},{1,1,1,0},{0,0,0,0},{0,0,0,0}},  // J
    {{0,0,1,0},{1,1,1,0},{0,0,0,0},{0,0,0,0}},  // L
};

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
static int lines_cleared = 0;

static uint8_t board[FROWS][FCOLS];
static uint8_t cur[4][4];
static uint8_t next_m[4][4];
static uint8_t pcolor = 1;
static uint8_t next_color = 1;
static int px = 3, py = 0;

// --- Canvas helpers ---
static inline uint16_t* canvas_pixel(int x, int y) {
    return (uint16_t*)(canvas_buf + y * canvas_stride) + x;
}

static void fill_rect(int x, int y, int w, int h, uint16_t color) {
    for (int row = 0; row < h; row++) {
        uint16_t* p = canvas_pixel(x, y + row);
        for (int col = 0; col < w; col++) p[col] = color;
    }
}

static void draw_cell(int cx, int cy, uint16_t color) {
    if (cy < 0 || cy >= FROWS || cx < 0 || cx >= FCOLS) return;
    int x = FIELD_X + cx * TCELL;
    int y = FIELD_Y + cy * TCELL;
    fill_rect(x + 1, y + 1, TCELL - 2, TCELL - 2, color);
}

// --- Piece logic ---
static int random_piece() { return random(0, 7); }

static void copy_matrix(uint8_t dst[4][4], const uint8_t src[4][4]) {
    memcpy(dst, src, 16);
}

// Поворот по часовой: (x,y) -> (x',y') = (3-y, x), т.е. dst[x][3-y] = src[y][x]
static void rotate_cw(uint8_t dst[4][4], const uint8_t src[4][4]) {
    for (int y = 0; y < 4; y++) {
        for (int x = 0; x < 4; x++) {
            dst[x][3 - y] = src[y][x];
        }
    }
}

static bool fits(const uint8_t m[4][4], int x, int y) {
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            if (!m[r][c]) continue;
            int cx = x + c;
            int cy = y + r;
            if (cx < 0 || cx >= FCOLS || cy >= FROWS) return false;
            if (cy >= 0 && board[cy][cx]) return false;
        }
    }
    return true;
}

static void gen_next() {
    next_color = (uint8_t)(random_piece() + 1);
    copy_matrix(next_m, SHAPES[next_color - 1]);
}

// Возвращает false, если новая фигура не влезает (game over)
static bool spawn_piece() {
    pcolor = next_color;
    copy_matrix(cur, next_m);
    px = 3;
    py = 0;
    gen_next();
    return fits(cur, px, py);
}

static int clear_lines() {
    int cleared = 0;
    for (int y = FROWS - 1; y >= 0; y--) {
        bool full = true;
        for (int x = 0; x < FCOLS; x++) {
            if (!board[y][x]) { full = false; break; }
        }
        if (full) {
            cleared++;
            for (int yy = y; yy > 0; yy--) {
                memcpy(board[yy], board[yy - 1], FCOLS);
            }
            memset(board[0], 0, FCOLS);
            y++;  // проверить эту же строку снова
        }
    }
    return cleared;
}

static void update_speed() {
    int level = 1 + lines_cleared / 8;
    int period = 700 - (level - 1) * 60;
    if (period < 150) period = 150;
    if (game_timer) lv_timer_set_period(game_timer, period);
}

// Фиксация фигуры + очистка + спавн. Возвращает false при game over.
static bool lock_piece() {
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            if (!cur[r][c]) continue;
            int cx = px + c;
            int cy = py + r;
            if (cy >= 0 && cy < FROWS && cx >= 0 && cx < FCOLS) {
                board[cy][cx] = pcolor;
            }
        }
    }

    int cleared = clear_lines();
    if (cleared > 0) {
        static const int PTS[] = {0, 40, 100, 300, 1200};
        int level = 1 + lines_cleared / 8;
        score += PTS[cleared] * level;
        lines_cleared += cleared;
        update_speed();
    }

    return spawn_piece();
}

// --- Drawing ---
static void draw_field() {
    // Field background + border
    fill_rect(FIELD_X - 2, FIELD_Y - 2, FCOLS * TCELL + 4, FROWS * TCELL + 4, COL_BORDER);
    fill_rect(FIELD_X, FIELD_Y, FCOLS * TCELL, FROWS * TCELL, COL_FIELD);

    // Grid lines
    for (int x = 1; x < FCOLS; x++) {
        fill_rect(FIELD_X + x * TCELL, FIELD_Y, 1, FROWS * TCELL, COL_GRID);
    }
    for (int y = 1; y < FROWS; y++) {
        fill_rect(FIELD_X, FIELD_Y + y * TCELL, FCOLS * TCELL, 1, COL_GRID);
    }

    // Locked cells
    for (int y = 0; y < FROWS; y++) {
        for (int x = 0; x < FCOLS; x++) {
            if (board[y][x]) draw_cell(x, y, PAL[board[y][x]]);
        }
    }

    if (state != STATE_PLAYING) return;

    // Ghost piece (dim)
    int gy = py;
    while (fits(cur, px, gy + 1)) gy++;
    if (gy != py) {
        uint16_t dim = (uint16_t)((PAL[pcolor] >> 2) & (0x1F << 11 | 0x3F << 5 | 0x1F));
        for (int r = 0; r < 4; r++) {
            for (int c = 0; c < 4; c++) {
                if (cur[r][c]) draw_cell(px + c, gy + r, dim);
            }
        }
    }

    // Current piece
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            if (cur[r][c]) draw_cell(px + c, py + r, PAL[pcolor]);
        }
    }
}

static void draw_next_panel() {
    // Panel background + border
    fill_rect(NEXT_X - 2, NEXT_Y - 2, PANEL_W + 4, PANEL_W + 4, COL_BORDER);
    fill_rect(NEXT_X, NEXT_Y, PANEL_W, PANEL_W, COL_FIELD);

    if (state != STATE_PLAYING) return;

    // Next piece centered in panel (4x4 * 10 = 40, padding 10)
    int ox = NEXT_X + (PANEL_W - 4 * TCELL) / 2;
    int oy = NEXT_Y + (PANEL_W - 4 * TCELL) / 2;
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            if (!next_m[r][c]) continue;
            fill_rect(ox + c * TCELL + 1, oy + r * TCELL + 1,
                      TCELL - 2, TCELL - 2, PAL[next_color]);
        }
    }
}

static void draw_game() {
    fill_rect(0, 0, CANVAS_W, CANVAS_H, COL_BG);
    draw_field();
    draw_next_panel();
    lv_obj_invalidate(canvas_obj);
}

static void update_info() {
    if (!lbl_info) return;
    if (state == STATE_MENU) {
        lv_label_set_text_fmt(lbl_info, "%s OK", LV_SYMBOL_PLAY);
    } else if (state == STATE_PLAYING) {
        lv_label_set_text_fmt(lbl_info, "Score: %d  Best: %d", score, high_score);
    } else {
        lv_label_set_text_fmt(lbl_info, "%s GAME OVER  |  OK",
                              LV_SYMBOL_WARNING);
    }
}

static void show_menu_overlay(bool game_over) {
    if (!menu_text || !hint) return;
    if (game_over) {
        lv_label_set_text_fmt(menu_text, "GAME OVER\nScore: %d", score);
        lv_obj_set_style_text_color(menu_text, lv_color_hex(0xF800), 0);
        lv_label_set_text(hint, "OK");
    } else {
        lv_label_set_text(menu_text, LV_SYMBOL_PLAY "\nTetris");
        lv_obj_set_style_text_color(menu_text, lv_color_hex(0x07FF), 0);
        lv_label_set_text(hint, "OK");
    }
    lv_obj_set_hidden(menu_text, false);
    lv_obj_set_hidden(hint, false);
}

static void reset_game() {
    memset(board, 0, sizeof(board));
    score = 0;
    lines_cleared = 0;
    gen_next();
    spawn_piece();
    update_speed();
}

static void start_game() {
    reset_game();
    state = STATE_PLAYING;
    lv_obj_set_hidden(menu_text, true);
    lv_obj_set_hidden(hint, true);
    update_info();
    draw_game();
    lv_timer_resume(game_timer);
}

static void game_over() {
    state = STATE_OVER;
    if (score > high_score) high_score = score;
    lv_timer_pause(game_timer);
    update_info();
    show_menu_overlay(true);
    draw_game();
}

// --- Timer ---
static void game_tick(lv_timer_t* t) {
    (void)t;
    if (state != STATE_PLAYING) return;

    if (fits(cur, px, py + 1)) {
        py++;
    } else {
        if (!lock_piece()) {
            game_over();
            return;
        }
    }

    draw_game();
    update_info();
}

// --- Soft/hard drop ---
static void hard_drop() {
    int dropped = 0;
    while (fits(cur, px, py + 1)) {
        py++;
        dropped++;
    }
    score += dropped * 2;
    if (!lock_piece()) {
        game_over();
        return;
    }
    draw_game();
    update_info();
}

static void soft_drop() {
    if (fits(cur, px, py + 1)) {
        py++;
        score += 1;
        draw_game();
        update_info();
    } else {
        if (!lock_piece()) {
            game_over();
            return;
        }
        draw_game();
        update_info();
    }
}

static void try_rotate() {
    uint8_t rot[4][4];
    rotate_cw(rot, cur);
    static const int KICKS[] = {0, -1, 1, -2, 2};
    for (int k = 0; k < 5; k++) {
        if (fits(rot, px + KICKS[k], py)) {
            memcpy(cur, rot, 16);
            px += KICKS[k];
            draw_game();
            return;
        }
    }
}

// --- Public API ---
void tetris_game_open(lv_obj_t* parent) {
    parent_ref = parent;
    state = STATE_MENU;
    score = 0;
    lines_cleared = 0;

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
    lv_label_set_text(lbl_title, "Tetris");
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

    // Initial screen: field preview behind menu
    memset(board, 0, sizeof(board));
    gen_next();
    draw_game();
    show_menu_overlay(false);
    update_info();

    game_timer = lv_timer_create(game_tick, 700, nullptr);
    lv_timer_pause(game_timer);
}

void tetris_game_close() {
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

void tetris_game_button(int button_id, int event) {
    if (event != BTN_EVENT_CLICKED) return;

    if (state == STATE_MENU) {
        if (button_id == BTN_ID_OK) start_game();
        return;
    }

    if (state == STATE_OVER) {
        if (button_id == BTN_ID_OK) start_game();
        return;
    }

    // STATE_PLAYING
    switch (button_id) {
        case BTN_ID_LEFT:
            if (fits(cur, px - 1, py)) { px--; draw_game(); }
            break;
        case BTN_ID_RIGHT:
            if (fits(cur, px + 1, py)) { px++; draw_game(); }
            break;
        case BTN_ID_DOWN:
            soft_drop();
            break;
        case BTN_ID_UP:
            hard_drop();
            break;
        case BTN_ID_OK:
            try_rotate();
            break;
    }
}
