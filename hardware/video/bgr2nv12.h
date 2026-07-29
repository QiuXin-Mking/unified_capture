#pragma once
/*
 * bgr2nv12.h — BGR24 → NV12 (YUV420SP) 色域转换
 */

#include <cstdint>

// BGR24 → NV12 (YUV420SP) 色域转换。
// hor_stride 为输出 NV12 每行的字节跨度（>= w，通常为 64 对齐）。
static void bgr_to_nv12(const uint8_t* bgr, int w, int h, int hor_stride, uint8_t* nv12) {
    uint8_t* Y  = nv12;
    uint8_t* UV = nv12 + hor_stride * h;

    for (int r = 0; r < h; r++) {
        for (int c = 0; c < w; c++) {
            int off = (r * w + c) * 3;
            int R = bgr[off + 2], G = bgr[off + 1], B = bgr[off];
            Y[r * hor_stride + c] = (uint8_t)((66 * R + 129 * G + 25 * B + 128) >> 8);
            if ((r & 1) == 0 && (c & 1) == 0) {
                int uv_row = r / 2;
                int uv_col = c & ~1;
                int uv_idx = uv_row * hor_stride + uv_col;
                UV[uv_idx]     = (uint8_t)((-38 * R - 74 * G + 112 * B + 128 + 32768) >> 8);
                UV[uv_idx + 1] = (uint8_t)((112 * R - 94 * G - 18 * B + 128 + 32768) >> 8);
            }
        }
    }
}
