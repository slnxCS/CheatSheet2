#include "ui/widgets/plasma_bg.h"
#include "ui/theme.h"
#include <Arduino.h>

static lv_obj_t* blobs[PLASMA_BLOBS] = {nullptr};
static float dx[PLASMA_BLOBS];
static float dy[PLASMA_BLOBS];
static float blob_x[PLASMA_BLOBS];
static float blob_y[PLASMA_BLOBS];
static bool inited = false;

void plasma_bg_create(lv_obj_t* parent) {
    if (inited) return;
    inited = true;

    for (int i = 0; i < PLASMA_BLOBS; i++) {
        blobs[i] = lv_obj_create(parent);
        int size = 60 + (i * 23) % 80;
        lv_obj_set_size(blobs[i], size, size);
        lv_obj_set_style_radius(blobs[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(blobs[i], lv_color_white(), 0);
        lv_obj_set_style_bg_opa(blobs[i], (lv_opa_t)(10 + (i % 3) * 8), 0);
        lv_obj_set_style_border_width(blobs[i], 0, 0);

        blob_x[i] = (i * 73) % 320;
        blob_y[i] = (i * 51) % 240;
        dx[i] = 0.3f + (i % 3) * 0.2f;
        dy[i] = 0.2f + (i % 2) * 0.15f;
        if (i % 2) dx[i] = -dx[i];
        if (i % 3) dy[i] = -dy[i];

        lv_obj_set_pos(blobs[i], (int)blob_x[i], (int)blob_y[i]);
        lv_obj_move_background(blobs[i]);
    }
}

void plasma_bg_update() {
    if (!inited) return;

    for (int i = 0; i < PLASMA_BLOBS; i++) {
        blob_x[i] += dx[i];
        blob_y[i] += dy[i];

        if (blob_x[i] > 340) dx[i] = -dx[i];
        if (blob_x[i] < -80) dx[i] = -dx[i];
        if (blob_y[i] > 260) dy[i] = -dy[i];
        if (blob_y[i] < -80) dy[i] = -dy[i];

        lv_obj_set_pos(blobs[i], (int)blob_x[i], (int)blob_y[i]);
    }
}
