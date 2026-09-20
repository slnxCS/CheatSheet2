#include "fonts/fonts.h"
#include "apps/builtin/game_2048.h"
#include "drivers/input.h"
#include "ui/theme.h"
#include <Arduino.h>
#include <cstring>

#define GRID_SIZE 4
#define CELL_PX   48
#define GAP       4
#define GRID_PX   (GRID_SIZE * CELL_PX + (GRID_SIZE - 1) * GAP)  // 204
#define OFFSET_X  ((320 - GRID_PX) / 2)  // 58
#define OFFSET_Y  38

static lv_obj_t* parent_ref = nullptr;
static lv_obj_t* canvas_obj = nullptr;
static lv_obj_t* lbl_score = nullptr;
static lv_obj_t* lbl_hint = nullptr;
static lv_timer_t* anim_timer = nullptr;
static uint8_t* canvas_buf = nullptr;
static uint32_t canvas_stride = 0;

static int grid[GRID_SIZE][GRID_SIZE];
static int score = 0;
static int best_score = 0;
static bool game_over = false;
static bool game_won = false;

// RGB565 colors for tile values
static uint16_t tile_color(int val) {
    switch (val) {
        case 0:    return 0xC618;  // #3C3A32 dark
        case 2:    return 0xEF1D;  // #EEE4DA
        case 4:    return 0xEDD9;  // #EDE0C8
        case 8:    return 0xF310;  // #F2B179 orange-light
        case 16:   return 0xF410;  // #F59563
        case 32:   return 0xF450;  // #F67C5F red-light
        case 64:   return 0xF800;  // #F65E3B red
        case 128:  return 0xED26;  // #EDCF72 gold
        case 256:  return 0xED06;  // #EDCC61
        case 512:  return 0xECC6;  // #EDC850
        case 1024: return 0xE486;  // #EDC53F
        case 2048: return 0xE306;  // #EDC22E
        default:   return 0x4208;  // bright for >2048
    }
}

static uint16_t text_color(int val) {
    return (val <= 4) ? 0x7BEF : 0xFFFF;  // dark or white
}

// Canvas pixel access
static inline uint16_t* px(int x, int y) {
    return (uint16_t*)(canvas_buf + y * canvas_stride) + x;
}

static void fill_rect(int x, int y, int w, int h, uint16_t color) {
    for (int row = 0; row < h; row++) {
        uint16_t* p = px(x, y + row);
        for (int col = 0; col < w; col++) p[col] = color;
    }
}

// Draw a simple digit (0-9) at pixel position using 5x7 bitmap font
static const uint8_t DIGIT_FONT[10][7] = {
    {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E}, // 0
    {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E}, // 1
    {0x0E,0x11,0x01,0x06,0x08,0x10,0x1F}, // 2
    {0x0E,0x11,0x01,0x06,0x01,0x11,0x0E}, // 3
    {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}, // 4
    {0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E}, // 5
    {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E}, // 6
    {0x1F,0x01,0x02,0x04,0x08,0x08,0x08}, // 7
    {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E}, // 8
    {0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C}, // 9
};

static void draw_number(int num, int cx, int cy, uint16_t color) {
    if (num == 0) return;

    char buf[8];
    snprintf(buf, sizeof(buf), "%d", num);
    int len = strlen(buf);

    int char_w = 6;  // 5 pixels + 1 gap
    int char_h = 7;
    int total_w = len * char_w - 1;
    int start_x = cx - total_w / 2;
    int start_y = cy - char_h / 2;

    for (int i = 0; i < len; i++) {
        int d = buf[i] - '0';
        if (d < 0 || d > 9) continue;
        for (int row = 0; row < char_h; row++) {
            for (int col = 0; col < 5; col++) {
                if (DIGIT_FONT[d][row] & (1 << (4 - col))) {
                    int px_x = start_x + i * char_w + col;
                    int px_y = start_y + row;
                    if (px_x >= 0 && px_x < 320 && px_y >= 0 && px_y < 240) {
                        *px(px_x, px_y) = color;
                    }
                }
            }
        }
    }
}

static void draw_board() {
    // Background
    fill_rect(0, 0, 320, 240, 0x4208);

    // Grid background
    fill_rect(OFFSET_X - 4, OFFSET_Y - 4, GRID_PX + 8, GRID_PX + 8, 0x2945);

    for (int r = 0; r < GRID_SIZE; r++) {
        for (int c = 0; c < GRID_SIZE; c++) {
            int x = OFFSET_X + c * (CELL_PX + GAP);
            int y = OFFSET_Y + r * (CELL_PX + GAP);
            int val = grid[r][c];

            fill_rect(x, y, CELL_PX, CELL_PX, tile_color(val));

            if (val > 0) {
                int cx = x + CELL_PX / 2;
                int cy = y + CELL_PX / 2;
                draw_number(val, cx, cy, text_color(val));
            }
        }
    }

    lv_obj_invalidate(canvas_obj);
}

static void update_score_label() {
    if (lbl_score)
        lv_label_set_text_fmt(lbl_score, "%d  |  %d", score, best_score);
}

// --- Game logic ---
static void add_random_tile() {
    int empty[GRID_SIZE * GRID_SIZE][2];
    int count = 0;
    for (int r = 0; r < GRID_SIZE; r++)
        for (int c = 0; c < GRID_SIZE; c++)
            if (grid[r][c] == 0) {
                empty[count][0] = r;
                empty[count][1] = c;
                count++;
            }
    if (count == 0) return;

    int idx = random(0, count);
    grid[empty[idx][0]][empty[idx][1]] = (random(0, 10) < 9) ? 2 : 4;
}

static bool can_move() {
    for (int r = 0; r < GRID_SIZE; r++)
        for (int c = 0; c < GRID_SIZE; c++) {
            if (grid[r][c] == 0) return true;
            if (c < GRID_SIZE - 1 && grid[r][c] == grid[r][c + 1]) return true;
            if (r < GRID_SIZE - 1 && grid[r][c] == grid[r + 1][c]) return true;
        }
    return false;
}

static bool has_2048() {
    for (int r = 0; r < GRID_SIZE; r++)
        for (int c = 0; c < GRID_SIZE; c++)
            if (grid[r][c] >= 2048) return true;
    return false;
}

// Slide a single row to the left, merge, return points
static int slide_row_left(int row[GRID_SIZE]) {
    int pts = 0;
    // Compact non-zero to left
    int tmp[GRID_SIZE] = {};
    int pos = 0;
    for (int i = 0; i < GRID_SIZE; i++)
        if (row[i] != 0) tmp[pos++] = row[i];

    // Merge adjacent equal
    for (int i = 0; i < GRID_SIZE - 1; i++) {
        if (tmp[i] != 0 && tmp[i] == tmp[i + 1]) {
            tmp[i] *= 2;
            pts += tmp[i];
            tmp[i + 1] = 0;
            if (tmp[i] >= 2048) game_won = true;
        }
    }
    // Compact again
    pos = 0;
    for (int i = 0; i < GRID_SIZE; i++)
        if (tmp[i] != 0) row[pos++] = tmp[i];
    while (pos < GRID_SIZE) row[pos++] = 0;

    return pts;
}

// Rotate grid 90° CW for easier merge logic
static void rotate_cw() {
    int tmp[GRID_SIZE][GRID_SIZE];
    for (int r = 0; r < GRID_SIZE; r++)
        for (int c = 0; c < GRID_SIZE; c++)
            tmp[c][GRID_SIZE - 1 - r] = grid[r][c];
    memcpy(grid, tmp, sizeof(grid));
}

static void rotate_ccw() {
    int tmp[GRID_SIZE][GRID_SIZE];
    for (int r = 0; r < GRID_SIZE; r++)
        for (int c = 0; c < GRID_SIZE; c++)
            tmp[GRID_SIZE - 1 - c][r] = grid[r][c];
    memcpy(grid, tmp, sizeof(grid));
}

// Save grid for change detection
static bool grids_equal(int a[GRID_SIZE][GRID_SIZE], int b[GRID_SIZE][GRID_SIZE]) {
    for (int r = 0; r < GRID_SIZE; r++)
        for (int c = 0; c < GRID_SIZE; c++)
            if (a[r][c] != b[r][c]) return false;
    return true;
}

// Move: dir 0=left, 1=down, 2=right, 3=up
static void do_move(int dir) {
    if (game_over) return;

    int old_grid[GRID_SIZE][GRID_SIZE];
    memcpy(old_grid, grid, sizeof(grid));

    int points = 0;

    // Rotate so desired direction becomes "left", merge, rotate back
    int rotations = dir;
    for (int i = 0; i < rotations; i++) rotate_cw();

    for (int r = 0; r < GRID_SIZE; r++) {
        points += slide_row_left(grid[r]);
    }

    for (int i = 0; i < rotations; i++) rotate_ccw();

    if (!grids_equal(old_grid, grid)) {
        score += points;
        if (score > best_score) best_score = score;
        add_random_tile();
        draw_board();
        update_score_label();

        if (has_2048() && !game_won) {
            game_won = true;
            game_over = true;
            if (lbl_hint) lv_label_set_text(lbl_hint, "2048! OK");
        } else if (!can_move()) {
            game_over = true;
            if (lbl_hint) lv_label_set_text(lbl_hint, "Game Over! OK");
        }
    }
}

static void reset_game() {
    memset(grid, 0, sizeof(grid));
    score = 0;
    game_over = false;
    game_won = false;
    add_random_tile();
    add_random_tile();
    draw_board();
    update_score_label();
    if (lbl_hint) lv_label_set_text(lbl_hint, "Arrows + OK");
}

// --- Public API ---
void game_2048_open(lv_obj_t* parent) {
    parent_ref = parent;
    lv_obj_set_style_bg_color(parent, lv_color_hex(0x0A0A1A), 0);

    // Score label at top
    lbl_score = lv_label_create(parent);
    lv_obj_set_style_text_color(lbl_score, theme_color_text(), 0);
    lv_obj_set_style_text_font(lbl_score, &lv_font_cyr_14, 0);
    lv_obj_align(lbl_score, LV_ALIGN_TOP_MID, 0, 6);

    // Hint at bottom
    lbl_hint = lv_label_create(parent);
    lv_obj_set_style_text_color(lbl_hint, theme_color_text_muted(), 0);
    lv_obj_set_style_text_font(lbl_hint, &lv_font_cyr_12, 0);
    lv_obj_align(lbl_hint, LV_ALIGN_BOTTOM_MID, 0, -2);

    // Canvas
    canvas_stride = lv_draw_buf_width_to_stride(320, LV_COLOR_FORMAT_RGB565);
    size_t buf_size = canvas_stride * 240;
    canvas_buf = (uint8_t*)ps_malloc(buf_size);
    if (!canvas_buf) canvas_buf = (uint8_t*)malloc(buf_size);
    if (!canvas_buf) return;
    memset(canvas_buf, 0, buf_size);

    canvas_obj = lv_canvas_create(parent);
    lv_canvas_set_buffer(canvas_obj, canvas_buf, 320, 240, LV_COLOR_FORMAT_RGB565);
    lv_obj_set_style_bg_opa(canvas_obj, LV_OPA_TRANSP, 0);
    lv_obj_align(canvas_obj, LV_ALIGN_CENTER, 0, 0);

    reset_game();
}

void game_2048_close() {
    if (canvas_buf) { free(canvas_buf); canvas_buf = nullptr; }
    parent_ref = nullptr;
    canvas_obj = nullptr;
    lbl_score = nullptr;
    lbl_hint = nullptr;
}

void game_2048_button(int button_id, int event) {
    if (event != BTN_EVENT_CLICKED) return;

    if (button_id == BTN_ID_OK) {
        reset_game();
        if (lbl_hint) lv_label_set_text(lbl_hint, "Arrows + OK");
        return;
    }

    if (game_over) return;

    switch (button_id) {
        case BTN_ID_LEFT:  do_move(0); break;
        case BTN_ID_DOWN:  do_move(1); break;
        case BTN_ID_RIGHT: do_move(2); break;
        case BTN_ID_UP:    do_move(3); break;
    }
}
