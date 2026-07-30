#pragma once
/*
 * imu_decode.h — 从 BGR 帧中解码 ICM42688 IMU 数据
 *
 * 移植自 capture_dual_imu_audio.cpp 和 02-extract-imu.sh
 *
 * 两种 IMU 码带布局:
 *   JHH2 (横向): 码带仅在图像顶部, 水平排列
 *     ┌────────────────────────────────┐
 *     │ ████▓▓▓▓████    ← 码带 (顶部)   │
 *     │    图像内容                      │
 *     │    图像内容                      │
 *     │    图像内容                      │
 *     └────────────────────────────────┘
 *     扫描: 从顶部边缘的行向中间交叉扫描
 *
 *   JHH04 (竖向): 码带在图像左侧, 垂直排列
 *     ┌──┬───────────────────────────┐
 *     │█ │                            │
 *     │█ │   图像内容                   │
 *     │▓ │                            │
 *     │  │                            │
 *     └──┴───────────────────────────┘
 *     扫描: 从左/右边缘的列向中间交叉扫描
 *
 * 每行/列包含: 同步头 + 有效载荷, 总约 192 字节.
 */

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <array>

// ---- 常量 ----
constexpr int IMU_VAL0   = 50;
constexpr int IMU_VAL1   = 220;
constexpr int IMU_USIZE  = 8;
constexpr int IMU_GROUP  = 16;
constexpr int IMU_TARGET = 192;  // 12 groups × 16 bytes
constexpr float ACC_SENS = 4000.0f / 32768.0f;
constexpr float GYR_SENS = 1000.0f / 32768.0f;

// ---- 大端序解码 ----
static inline uint32_t be_u32(const uint8_t* b, int off) {
    return ((uint32_t)b[off] << 24) | ((uint32_t)b[off + 1] << 16) |
           ((uint32_t)b[off + 2] << 8) | b[off + 3];
}
static inline int16_t be_s16(const uint8_t* b, int off) {
    uint16_t v = ((uint16_t)b[off] << 8) | b[off + 1];
    return (v & 0x8000) ? (int16_t)(v - 0x10000) : (int16_t)v;
}

// ---- 解码二维码带中的一行数据 ----
static uint32_t imu_line_decode(const uint8_t* bgr_row, int row_width, uint8_t* out) {
    constexpr int C = 3, G_OFF = 1;

    // 同步头: 前 4 字节必须 > IMU_VAL1
    if (!(bgr_row[0] > IMU_VAL1 && bgr_row[1] > IMU_VAL1 &&
          bgr_row[2] > IMU_VAL1 && bgr_row[3] > IMU_VAL1))
        return 0;

    // 找同步头结束位置 (G 通道 < IMU_VAL1 的位置)
    uint32_t ind = 0;
    for (; ind < (uint32_t)row_width; ind++) {
        if (bgr_row[ind * C + G_OFF] < IMU_VAL1) {
            ind += (IMU_USIZE >> 1);
            break;
        }
    }
    if (ind >= (uint32_t)row_width) return 0;

    // 解码 bit 流
    uint8_t* bits = new uint8_t[row_width];
    uint32_t bit_count = 0;
    for (; ind < (uint32_t)row_width - 1; ind += IMU_USIZE) {
        uint16_t sum = (uint16_t)bgr_row[ind * C + G_OFF] +
                       (uint16_t)bgr_row[(ind + 1) * C + G_OFF];
        if (sum < (IMU_VAL0 << 1))
            bits[bit_count++] = 0;
        else if (sum < (IMU_VAL1 << 1))
            bits[bit_count++] = 1;
        else
            break;
    }

    // bit → 字节
    uint32_t byte_count = bit_count / 8;
    for (uint32_t i = 0; i < byte_count; i++) {
        uint8_t v = 0;
        v |= bits[i * 8 + 0] << 0;
        v |= bits[i * 8 + 1] << 1;
        v |= bits[i * 8 + 2] << 2;
        v |= bits[i * 8 + 3] << 3;
        v |= bits[i * 8 + 4] << 4;
        v |= bits[i * 8 + 5] << 5;
        v |= bits[i * 8 + 6] << 6;
        v |= bits[i * 8 + 7] << 7;
        out[i] = v;
    }
    delete[] bits;
    return byte_count;
}

// ============================================================
// 横向扫描 (JHH2/JHH02): 从顶部边缘的行向中间交叉扫描 (底部无码带)
// ============================================================
static uint32_t imu_read_frame_horizontal(const uint8_t* bgr, int w, int h, uint8_t* buf) {
    constexpr int C = 3;
    struct { int start_row; int step; } strategies[2] = {
        {3,          (int)IMU_USIZE},   // top edge, going down
        {3,         -(int)IMU_USIZE},   // top edge, going up (hits top)
    };
    for (int s = 0; s < 2; s++) {
        int sr = strategies[s].start_row;
        int step = strategies[s].step;
        uint32_t total = 0;
        int row = sr;
        for (int i = 0; i < h / IMU_USIZE + 4; i++) {
            if (row < 0 || row >= h) break;
            uint8_t line_buf[1024];
            uint32_t n = imu_line_decode(bgr + (size_t)row * w * C, w, line_buf);
            if (n > 0) {
                if (total + n > 256) n = 256 - total;
                memcpy(buf + total, line_buf, n);
                total += n;
            }
            row += step;
            if (total >= IMU_TARGET) break;
        }
        if (total >= IMU_GROUP) return total;
    }
    return 0;
}

// ---- 竖向: 提取一列像素到连续缓冲区, 然后复用 imu_line_decode ----
static uint32_t imu_column_decode(const uint8_t* bgr, int col, int w, int h, uint8_t* out) {
    constexpr int C = 3;
    // 提取列: 每行同一列位置的 3 字节 BGR
    uint8_t* col_buf = new uint8_t[h * C];
    for (int r = 0; r < h; r++) {
        int src = (r * w + col) * C;
        col_buf[r * C + 0] = bgr[src + 0];
        col_buf[r * C + 1] = bgr[src + 1];
        col_buf[r * C + 2] = bgr[src + 2];
    }
    // 复用横向解码器 (列数据现在是连续的 "行")
    uint32_t result = imu_line_decode(col_buf, h, out);
    delete[] col_buf;
    return result;
}

// ============================================================
// 竖向扫描 (JHH04): 从左/右边缘的列向中间交叉扫描
// ============================================================
static uint32_t imu_read_frame_vertical(const uint8_t* bgr, int w, int h, uint8_t* buf) {
    struct { int start_col; int step; } strategies[4] = {
        {3,          (int)IMU_USIZE},   // left edge, going right
        {3,         -(int)IMU_USIZE},   // left edge, going left (hits edge)
        {w - 3,     -(int)IMU_USIZE},   // right edge, going left
        {w - 3,      (int)IMU_USIZE},   // right edge, going right (hits edge)
    };
    for (int s = 0; s < 4; s++) {
        int sc = strategies[s].start_col;
        int step = strategies[s].step;
        uint32_t total = 0;
        int col = sc;
        for (int i = 0; i < w / IMU_USIZE + 4; i++) {
            if (col < 0 || col >= w) break;
            uint8_t line_buf[1024];
            uint32_t n = imu_column_decode(bgr, col, w, h, line_buf);
            if (n > 0) {
                if (total + n > 256) n = 256 - total;
                memcpy(buf + total, line_buf, n);
                total += n;
            }
            col += step;
            if (total >= IMU_TARGET) break;
        }
        if (total >= IMU_GROUP) return total;
    }
    return 0;
}

// ---- 从单通道亮度行解码，避免为 IMU 构造完整 BGR 帧 ----
static uint32_t imu_luma_line_decode(const uint8_t* row, int row_width,
                                     uint8_t* out) {
    if (!row || row_width < 5 ||
        !(row[0] > IMU_VAL1 && row[1] > IMU_VAL1 &&
          row[2] > IMU_VAL1 && row[3] > IMU_VAL1)) {
        return 0;
    }

    uint32_t ind = 0;
    for (; ind < static_cast<uint32_t>(row_width); ++ind) {
        if (row[ind] < IMU_VAL1) {
            ind += (IMU_USIZE >> 1);
            break;
        }
    }
    if (ind >= static_cast<uint32_t>(row_width)) {
        return 0;
    }

    std::array<uint8_t, 8192> bits{};
    uint32_t bit_count = 0;
    for (; ind + 1 < static_cast<uint32_t>(row_width);
         ind += IMU_USIZE) {
        if (bit_count >= bits.size()) {
            break;
        }
        const uint16_t sum =
            static_cast<uint16_t>(row[ind]) + row[ind + 1];
        if (sum < (IMU_VAL0 << 1)) {
            bits[bit_count++] = 0;
        } else if (sum < (IMU_VAL1 << 1)) {
            bits[bit_count++] = 1;
        } else {
            break;
        }
    }

    const uint32_t byte_count = bit_count / 8;
    for (uint32_t i = 0; i < byte_count; ++i) {
        uint8_t value = 0;
        for (int bit = 0; bit < 8; ++bit) {
            value |= bits[i * 8 + bit] << bit;
        }
        out[i] = value;
    }
    return byte_count;
}

static uint32_t imu_read_luma_horizontal(const uint8_t* y, int w, int h,
                                         int stride, uint8_t* buf) {
    if (!y || !buf || w <= 0 || h <= 0 || stride < w) {
        return 0;
    }
    for (int row = 3; row < h; row += IMU_USIZE) {
        std::array<uint8_t, 1024> line_buf{};
        uint32_t count = imu_luma_line_decode(
            y + static_cast<size_t>(row) * stride, w, line_buf.data());
        if (count > 0) {
            if (count > 256) {
                count = 256;
            }
            memcpy(buf, line_buf.data(), count);
            return count;
        }
    }
    return 0;
}

static uint32_t imu_read_luma_vertical(const uint8_t* y, int w, int h,
                                       int stride, uint8_t* buf) {
    if (!y || !buf || w <= 0 || h <= 0 || stride < w || h > 8192) {
        return 0;
    }
    const int starts[2] = {3, w - 3};
    const int steps[2] = {IMU_USIZE, -IMU_USIZE};
    for (int side = 0; side < 2; ++side) {
        for (int col = starts[side]; col >= 0 && col < w;
             col += steps[side]) {
            std::array<uint8_t, 8192> column{};
            for (int row = 0; row < h; ++row) {
                column[row] = y[static_cast<size_t>(row) * stride + col];
            }
            std::array<uint8_t, 1024> line_buf{};
            uint32_t count =
                imu_luma_line_decode(column.data(), h, line_buf.data());
            if (count > 0) {
                if (count > 256) {
                    count = 256;
                }
                memcpy(buf, line_buf.data(), count);
                return count;
            }
        }
    }
    return 0;
}

// ---- 解析 IMU 数据, 写 JSONL ----
// {"frame_idx":N,"t_us":N,"ax_mg":f,"ay_mg":f,"az_mg":f,"gx_mdps":f,"gy_mdps":f,"gz_mdps":f,"exp_start_us":N,"exp_end_us":N}
static void imu_parse_and_write(const uint8_t* buf, uint32_t len,
                                uint64_t frame_idx, FILE* fp) {
    if (len < IMU_GROUP) return;
    const uint8_t* hdr = buf;
    int ns = hdr[1] & 0xF;
    if (ns < 1 || ns > 16) return;

    // 兼容旧协议: 检测头部是否重复
    bool old_proto = true;
    for (int i = 0; i < 8; i++) {
        if (hdr[i] != hdr[i + 8]) { old_proto = false; break; }
    }
    if (old_proto) ns = 11;

    uint64_t es = be_u32(hdr, 8);
    uint64_t ee = be_u32(hdr, 12);

    for (int i = 0; i < ns; i++) {
        int off = (1 + i) * IMU_GROUP;
        if (off + IMU_GROUP > (int)len) break;
        const uint8_t* s = buf + off;
        uint32_t t_us = be_u32(s, 0);
        int16_t ax = be_s16(s, 4), ay = be_s16(s, 6), az = be_s16(s, 8);
        int16_t gx = be_s16(s, 10), gy = be_s16(s, 12), gz = be_s16(s, 14);

        // 跳过无效样本
        if ((ax == -1 && ay == -1 && az == -1) || gx == -32768) continue;

        fprintf(fp,
                "{\"frame_idx\":%llu,\"t_us\":%u,"
                "\"ax_mg\":%.3f,\"ay_mg\":%.3f,\"az_mg\":%.3f,"
                "\"gx_mdps\":%.3f,\"gy_mdps\":%.3f,\"gz_mdps\":%.3f,"
                "\"exp_start_us\":%llu,\"exp_end_us\":%llu}\n",
                (unsigned long long)frame_idx, t_us,
                ax * ACC_SENS, ay * ACC_SENS, az * ACC_SENS,
                gx * GYR_SENS, gy * GYR_SENS, gz * GYR_SENS,
                (unsigned long long)es, (unsigned long long)ee);
    }
    fflush(fp);
}
