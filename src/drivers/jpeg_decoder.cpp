#include "drivers/jpeg_decoder.h"
#include <JPEGDEC.h>

struct DecodeCtx {
    uint16_t* buf;
    int out_w;
    int out_h;
    uint32_t stride;  // bytes per row
};

static DecodeCtx decode_ctx;
static JPEGDEC* jpeg_ptr = nullptr;
static int cb_count = 0;
static bool jpeg_is_open = false;

static int jpeg_draw_cb(JPEGDRAW* pDraw) {
    DecodeCtx* ctx = (DecodeCtx*)pDraw->pUser;
    if (!ctx || !ctx->buf) return 0;

    cb_count++;
    int16_t x = pDraw->x;
    int16_t y = pDraw->y;
    int16_t w = pDraw->iWidth;
    int16_t h = pDraw->iHeight;
    uint16_t* src = pDraw->pPixels;

    for (int row = 0; row < h; row++) {
        int dst_y = y + row;
        if (dst_y < 0 || dst_y >= ctx->out_h) continue;

        int copy_w = w;
        int src_x_off = 0;
        if (x < 0) {
            src_x_off = -x;
            copy_w += x;
            x = 0;
        }
        if (x + copy_w > ctx->out_w) {
            copy_w = ctx->out_w - x;
        }
        if (copy_w <= 0) continue;

        uint16_t* dst = (uint16_t*)((uint8_t*)ctx->buf + dst_y * ctx->stride) + x;
        uint16_t* s = src + row * w + src_x_off;
        for (int i = 0; i < copy_w; i++) {
            dst[i] = s[i];
        }
    }
    return 1;
}

bool jpeg_open(const uint8_t* jpeg_data, size_t jpeg_len, int* w, int* h) {
    if (!jpeg_ptr) jpeg_ptr = new JPEGDEC();

    // openRAM returns non-zero on SUCCESS
    int rc = jpeg_ptr->openRAM((uint8_t*)jpeg_data, (int)jpeg_len, jpeg_draw_cb);
    if (!rc) {
        Serial.printf("jpegdec: openRAM failed, err=%d\n", jpeg_ptr->getLastError());
        return false;
    }
    jpeg_is_open = true;

    *w = jpeg_ptr->getWidth();
    *h = jpeg_ptr->getHeight();
    Serial.printf("jpegdec: native %dx%d bpp=%d\n", *w, *h, jpeg_ptr->getBpp());
    return true;
}

void jpeg_close() {
    if (jpeg_ptr && jpeg_is_open) {
        jpeg_ptr->close();
        jpeg_is_open = false;
    }
}

bool jpeg_decode_to_rgb565(const uint8_t* jpeg_data, size_t jpeg_len,
                            uint8_t* out_buf, int out_w, int out_h,
                            uint32_t out_stride, int scale,
                            int* actual_w, int* actual_h) {
    cb_count = 0;

    if (!jpeg_ptr) jpeg_ptr = new JPEGDEC();

    // Open if not already open
    if (!jpeg_is_open) {
        int rc = jpeg_ptr->openRAM((uint8_t*)jpeg_data, (int)jpeg_len, jpeg_draw_cb);
        if (!rc) {
            Serial.printf("jpegdec: openRAM failed, err=%d\n", jpeg_ptr->getLastError());
            return false;
        }
        jpeg_is_open = true;
    }

    if (out_stride == 0) out_stride = out_w * 2;

    decode_ctx.buf = (uint16_t*)out_buf;
    decode_ctx.out_w = out_w;
    decode_ctx.out_h = out_h;
    decode_ctx.stride = out_stride;

    jpeg_ptr->setUserPointer(&decode_ctx);

    int w = jpeg_ptr->getWidth();
    int h = jpeg_ptr->getHeight();

    int opts = JPEG_SCALE_EIGHTH;
    if (scale > 0) {
        if (scale >= 8) opts = JPEG_SCALE_EIGHTH;
        else if (scale >= 4) opts = JPEG_SCALE_QUARTER;
        else if (scale >= 2) opts = JPEG_SCALE_HALF;
        else opts = 0;
    } else {
        if (w <= out_w * 2 && h <= out_h * 2) {
            opts = JPEG_SCALE_HALF;
        } else if (w <= out_w * 4 && h <= out_h * 4) {
            opts = JPEG_SCALE_QUARTER;
        }
    }

    // Compute actual decoded dimensions after scale
    int shift = 0;
    if (opts == JPEG_SCALE_HALF) shift = 1;
    else if (opts == JPEG_SCALE_QUARTER) shift = 2;
    else if (opts == JPEG_SCALE_EIGHTH) shift = 3;

    int dec_w = (w + (1 << shift) - 1) >> shift;
    int dec_h = (h + (1 << shift) - 1) >> shift;

    if (actual_w) *actual_w = dec_w;
    if (actual_h) *actual_h = dec_h;

    int rc = jpeg_ptr->decode(0, 0, opts);
    jpeg_close();

    Serial.printf("jpegdec: decode=%d callbacks=%d dec=%dx%d px[0]=%04X\n",
                  rc, cb_count, dec_w, dec_h, ((uint16_t*)out_buf)[0]);

    return rc > 0;
}
