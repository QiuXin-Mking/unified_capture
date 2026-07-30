/*
 * test_imu_magnetometer.cpp — 单帧 IMU 码带解析, 诊断磁力计数据
 *
 * 用法:
 *   1. 编译: g++ -std=c++20 -O2 -o test_imu_mag test_imu_magnetometer.cpp \
 *            -lturbojpeg $(pkg-config --cflags --libs turbojpeg)
 *   2. 运行: ./test_imu_mag <mjpeg_file> <orientation>
 *            orientation: h=horizontal (jhh02), v=vertical (jhh04)
 *
 * 输出:
 *   - 完整解码 buffer 的 hex dump
 *   - Header 解析 (ns, flags, exp_start/end)
 *   - Acc+Gyro 样本
 *   - 剩余字节检测 (磁力计? 填充?)
 */

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <array>
#include <algorithm>

#include <turbojpeg.h>

// ============================================================
// 从 imu_decode.h 复制的常量和解码函数 (不改动, 只扩大 TARGET)
// ============================================================
constexpr int IMU_VAL0   = 50;
constexpr int IMU_VAL1   = 220;
constexpr int IMU_USIZE  = 8;
constexpr int IMU_GROUP  = 16;
constexpr int IMU_TARGET = 1024;   // ★ 从 192 扩大到 1024, 不限制解码
constexpr float ACC_SENS = 4000.0f / 32768.0f;
constexpr float GYR_SENS = 1000.0f / 32768.0f;

static inline uint32_t be_u32(const uint8_t* b, int off) {
    return ((uint32_t)b[off] << 24) | ((uint32_t)b[off + 1] << 16) |
           ((uint32_t)b[off + 2] << 8) | b[off + 3];
}
static inline int16_t be_s16(const uint8_t* b, int off) {
    uint16_t v = ((uint16_t)b[off] << 8) | b[off + 1];
    return (v & 0x8000) ? (int16_t)(v - 0x10000) : (int16_t)v;
}

// -- 从单通道亮度行解码 --
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
    if (ind >= static_cast<uint32_t>(row_width)) return 0;

    std::array<uint8_t, 8192> bits{};
    uint32_t bit_count = 0;
    for (; ind + 1 < static_cast<uint32_t>(row_width); ind += IMU_USIZE) {
        if (bit_count >= bits.size()) break;
        uint16_t sum = static_cast<uint16_t>(row[ind]) + row[ind + 1];
        if (sum < (IMU_VAL0 << 1))       bits[bit_count++] = 0;
        else if (sum < (IMU_VAL1 << 1))  bits[bit_count++] = 1;
        else break;
    }
    uint32_t byte_count = bit_count / 8;
    for (uint32_t i = 0; i < byte_count; ++i) {
        uint8_t value = 0;
        for (int bit = 0; bit < 8; ++bit)
            value |= bits[i * 8 + bit] << bit;
        out[i] = value;
    }
    return byte_count;
}

static uint32_t imu_read_luma_horizontal(const uint8_t* y, int w, int h,
                                         int stride, uint8_t* buf,
                                         int buf_capacity) {
    if (!y || !buf || w <= 0 || h <= 0 || stride < w) return 0;
    const int starts[2] = {3, 3};
    const int steps[2] = {IMU_USIZE, -IMU_USIZE};
    for (int strategy = 0; strategy < 2; ++strategy) {
        uint32_t total = 0;
        for (int row = starts[strategy]; row >= 0 && row < h; row += steps[strategy]) {
            std::array<uint8_t, 1024> line_buf{};
            uint32_t count = imu_luma_line_decode(
                y + static_cast<size_t>(row) * stride, w, line_buf.data());
            if (count > 0) {
                if (total + count > (uint32_t)buf_capacity)
                    count = buf_capacity - total;
                memcpy(buf + total, line_buf.data(), count);
                total += count;
            }
            if (total >= IMU_TARGET) break;
        }
        if (total >= IMU_GROUP) return total;
    }
    return 0;
}

static uint32_t imu_read_luma_vertical(const uint8_t* y, int w, int h,
                                       int stride, uint8_t* buf,
                                       int buf_capacity) {
    if (!y || !buf || w <= 0 || h <= 0 || stride < w || h > 8192) return 0;
    const int starts[4] = {3, 3, w - 3, w - 3};
    const int steps[4] = {IMU_USIZE, -IMU_USIZE, -IMU_USIZE, IMU_USIZE};
    for (int strategy = 0; strategy < 4; ++strategy) {
        uint32_t total = 0;
        for (int col = starts[strategy]; col >= 0 && col < w; col += steps[strategy]) {
            std::array<uint8_t, 8192> column{};
            for (int row = 0; row < h; ++row)
                column[row] = y[static_cast<size_t>(row) * stride + col];
            std::array<uint8_t, 1024> line_buf{};
            uint32_t count = imu_luma_line_decode(column.data(), h, line_buf.data());
            if (count > 0) {
                if (count > 256) count = 256;
                if (total + count > (uint32_t)buf_capacity)
                    count = buf_capacity - total;
                memcpy(buf + total, line_buf.data(), count);
                total += count;
            }
            if (total >= IMU_TARGET) break;
        }
        if (total >= IMU_GROUP) return total;
    }
    return 0;
}

// ============================================================
// Hex dump
// ============================================================
static void hex_dump(const uint8_t* data, uint32_t len, const char* title) {
    printf("\n=== %s (%u bytes) ===\n", title, len);
    for (uint32_t offset = 0; offset < len; offset += 16) {
        printf("%04x: ", offset);
        for (int i = 0; i < 16 && (offset + i) < len; ++i)
            printf("%02x ", data[offset + i]);
        printf(" ");
        for (int i = 0; i < 16 && (offset + i) < len; ++i) {
            uint8_t c = data[offset + i];
            printf("%c", (c >= 32 && c < 127) ? c : '.');
        }
        printf("\n");
    }
}

// ============================================================
// 解析: 尝试多种分组大小
// ============================================================
static void parse_and_report(const uint8_t* buf, uint32_t len) {
    if (len < IMU_GROUP) {
        printf("ERROR: buffer too small (%u < %d)\n", len, IMU_GROUP);
        return;
    }

    const uint8_t* hdr = buf;

    // Header 分析
    printf("\n=== Header 解析 ===\n");
    printf("  hdr[0]     = 0x%02x (sync?)\n", hdr[0]);
    printf("  hdr[1]     = 0x%02x  lower_nibble(ns)=%d  upper_nibble=0x%x\n",
           hdr[1], hdr[1] & 0xF, (hdr[1] >> 4) & 0xF);
    printf("  hdr[2]     = 0x%02x\n", hdr[2]);
    printf("  hdr[3]     = 0x%02x\n", hdr[3]);
    printf("  hdr[4..7]  = %02x %02x %02x %02x\n", hdr[4], hdr[5], hdr[6], hdr[7]);

    // 检测旧协议 (header 8-15 = header 0-7)
    bool old_proto = true;
    for (int i = 0; i < 8; i++) {
        if (hdr[i] != hdr[i + 8]) { old_proto = false; break; }
    }

    uint32_t exp_start = be_u32(hdr, 8);
    uint32_t exp_end   = be_u32(hdr, 12);
    int ns = hdr[1] & 0xF;

    printf("  hdr[8..11] = exp_start_us = %u\n", exp_start);
    printf("  hdr[12..15]= exp_end_us   = %u\n", exp_end);
    printf("  old_proto  = %s\n", old_proto ? "true (header重复)" : "false (新协议)");
    if (old_proto) ns = 11;
    printf("  ns         = %d samples\n", ns);

    // 尝试 16 字节一组的 acc+gyro 解析
    printf("\n=== 16字节组 (acc+gyro) 解析 ===\n");
    int max_i = (len - IMU_GROUP) / IMU_GROUP;
    printf("  最多 %d 组 (总长 %u, header %d)\n", max_i, len, IMU_GROUP);

    for (int i = 0; i < std::min(max_i, 20); i++) {
        int off = (1 + i) * IMU_GROUP;
        if (off + IMU_GROUP > (int)len) break;
        const uint8_t* s = buf + off;

        uint32_t t_us = be_u32(s, 0);
        int16_t ax = be_s16(s, 4), ay = be_s16(s, 6), az = be_s16(s, 8);
        int16_t gx = be_s16(s, 10), gy = be_s16(s, 12), gz = be_s16(s, 14);

        bool is_valid = !(ax == -1 && ay == -1 && az == -1) && gx != -32768;
        const char* tag = (i < ns) ? "[ACC+GYRO]" : "[EXTRA]";

        printf("  %s group[%2d] @%04x: "
               "t_us=%10u  acc=(%6d,%6d,%6d)  gyro=(%6d,%6d,%6d)  %s\n",
               tag, i, off, t_us, ax, ay, az, gx, gy, gz,
               is_valid ? "valid" : "INVALID");
    }

    // 检查 ns 组之后还有多少剩余数据
    int consumed = (1 + ns) * IMU_GROUP;
    int remaining = (int)len - consumed;

    printf("\n=== 剩余数据分析 ===\n");
    printf("  ns=%d, consumed=%d bytes, total=%u bytes, remaining=%d bytes\n",
           ns, consumed, len, remaining);

    if (remaining > 0) {
        printf("  ⚡ 有 %d 字节未被解析! (可能是磁力计数据)\n", remaining);
        hex_dump(buf + consumed, remaining, "剩余字节 (可能为磁力计)");

        // 尝试将剩余字节按不同结构解析
        printf("\n=== 尝试解析剩余数据 ===\n");

        // 尝试1: 16字节组, 但字段布局不同 (磁力计: mx,my,mz 替换 gx,gy,gz)
        int extra_groups = remaining / 16;
        if (extra_groups > 0) {
            printf("  --- 尝试1: 剩余数据按 16B 组解析 -> %d 组 ---\n", extra_groups);
            for (int i = 0; i < extra_groups; i++) {
                int off = consumed + i * 16;
                const uint8_t* s = buf + off;
                uint32_t v0 = be_u32(s, 0);
                int16_t v1 = be_s16(s, 4), v2 = be_s16(s, 6), v3 = be_s16(s, 8);
                int16_t v4 = be_s16(s, 10), v5 = be_s16(s, 12), v6 = be_s16(s, 14);
                printf("  [%d] @%04x: u32=%u  s16=(%d,%d,%d, %d,%d,%d)\n",
                       i, off, v0, v1, v2, v3, v4, v5, v6);
            }
        }

        // 尝试2: 前几个字节作为磁力计专用格式
        if (remaining >= 6) {
            printf("\n  --- 尝试2: 剩余数据前6字节可能是 (mx,my,mz) int16 ---\n");
            int16_t mx = be_s16(buf, consumed);
            int16_t my = be_s16(buf, consumed + 2);
            int16_t mz = be_s16(buf, consumed + 4);
            printf("  mx=%d, my=%d, mz=%d\n", mx, my, mz);
            // 假设磁力计量程 ±4800μT, 16bit → 0.15μT/LSB (典型值)
            float mag_sens = 4800.0f / 32768.0f;
            printf("  → %.3f μT, %.3f μT, %.3f μT (假设量程±4800μT)\n",
                   mx * mag_sens, my * mag_sens, mz * mag_sens);
        }

        // 尝试3: 磁力计可能是独立sample, 跟在11个acc/gyro后面
        if (remaining >= 16) {
            printf("\n  --- 尝试3: 剩余作为独立16B磁力计样本 ---\n");
            int mag_samples = remaining / 16;
            for (int i = 0; i < mag_samples; i++) {
                int off = consumed + i * 16;
                const uint8_t* s = buf + off;
                uint32_t t = be_u32(s, 0);
                int16_t mx = be_s16(s, 4), my = be_s16(s, 6), mz = be_s16(s, 8);
                // bytes 10-15: could be status, padding, or extra data
                printf("  mag[%d]: t_us=%u  mx=%d my=%d mz=%d  raw[10-15]=%02x%02x %02x%02x %02x%02x\n",
                       i, t, mx, my, mz,
                       s[10], s[11], s[12], s[13], s[14], s[15]);
            }
        }

        // 尝试4: 24字节组 (header + 6x s16)
        if (remaining >= 24) {
            printf("\n  --- 尝试4: 剩余按24B组解析 (t_us + acc + gyro + mag?) ---\n");
            int g24 = remaining / 24;
            for (int i = 0; i < g24; i++) {
                int off = consumed + i * 24;
                const uint8_t* s = buf + off;
                uint32_t t = be_u32(s, 0);
                int16_t a0 = be_s16(s, 4), a1 = be_s16(s, 6), a2 = be_s16(s, 8);
                int16_t g0 = be_s16(s, 10), g1 = be_s16(s, 12), g2 = be_s16(s, 14);
                int16_t m0 = be_s16(s, 16), m1 = be_s16(s, 18), m2 = be_s16(s, 20);
                printf("  24B[%d]: t=%u a=(%d,%d,%d) g=(%d,%d,%d) m=(%d,%d,%d)\n",
                       i, t, a0, a1, a2, g0, g1, g2, m0, m1, m2);
            }
        }
    } else if (remaining < 0) {
        printf("  ⚠ consumed %d > total %u (数据不足, 可能是截断的?)\n",
               consumed, len);
    } else {
        printf("  ✅ 恰好用尽所有数据, 无剩余字节。没有磁力计数据。\n");
    }
}

// ============================================================
// main
// ============================================================
int main(int argc, char* argv[]) {
    if (argc < 3) {
        fprintf(stderr, "用法: %s <mjpeg_file> <h|v>\n", argv[0]);
        fprintf(stderr, "  h = horizontal (jhh02)\n");
        fprintf(stderr, "  v = vertical (jhh04)\n");
        return 1;
    }
    const char* path = argv[1];
    char orientation = argv[2][0];

    // -- 1. 读 MJPEG 文件 --
    FILE* fm = fopen(path, "rb");
    if (!fm) { perror("fopen"); return 1; }
    fseek(fm, 0, SEEK_END);
    long fsize = ftell(fm);
    fseek(fm, 0, SEEK_SET);
    uint8_t* mjpg = new uint8_t[fsize];
    fread(mjpg, 1, fsize, fm);
    fclose(fm);
    printf("读取 MJPEG: %ld bytes\n", fsize);

    // -- 2. MJPEG → Y 平面 --
    tjhandle tj = tjInitDecompress();
    int w, h, subsamp;
    tjDecompressHeader2(tj, mjpg, fsize, &w, &h, &subsamp);
    printf("图像尺寸: %dx%d, subsamp=%d\n", w, h, subsamp);

    // 分配 Y 平面
    uint8_t* y_plane = new uint8_t[w * h];
    // 直接解出 Y 平面: tjDecompressToYUVPlanes
    uint8_t* planes[3] = {y_plane, nullptr, nullptr};
    int strides[3] = {w, 0, 0};
    int ret = tjDecompressToYUVPlanes(tj, mjpg, fsize, planes, w, strides, 0,
                                       TJPF_GRAY);
    if (ret != 0) {
        fprintf(stderr, "tjDecompressToYUVPlanes failed: %s\n", tjGetErrorStr2(tj));
        // fallback: use tjDecompress2 to GRAY
        ret = tjDecompress2(tj, mjpg, fsize, y_plane, w, 0, h, TJPF_GRAY,
                            TJFLAG_FASTDCT);
        if (ret != 0) {
            fprintf(stderr, "tjDecompress2(GRAY) also failed: %s\n",
                    tjGetErrorStr2(tj));
            delete[] mjpg;
            delete[] y_plane;
            tjDestroy(tj);
            return 1;
        }
    }
    tjDestroy(tj);
    delete[] mjpg;

    printf("Y平面: %dx%d stride=%d\n", w, h, w);

    // -- 3. IMU 解码 (使用扩大的 TARGET) --
    constexpr int BUF_CAP = 2048;
    uint8_t imu_buf[BUF_CAP] = {};
    uint32_t imu_len = 0;

    if (orientation == 'h') {
        imu_len = imu_read_luma_horizontal(y_plane, w, h, w, imu_buf, BUF_CAP);
    } else {
        imu_len = imu_read_luma_vertical(y_plane, w, h, w, imu_buf, BUF_CAP);
    }
    delete[] y_plane;

    printf("\nIMU 解码: %u bytes (限制: BUF_CAP=%d, IMU_TARGET=%d)\n",
           imu_len, BUF_CAP, IMU_TARGET);

    if (imu_len == 0) {
        printf("ERROR: 未解码到 IMU 数据\n");
        return 1;
    }

    // -- 4. 全量 hex dump --
    hex_dump(imu_buf, imu_len, "完整解码数据");

    // -- 5. 解析 --
    parse_and_report(imu_buf, imu_len);

    printf("\n=== DONE ===\n");
    return 0;
}
