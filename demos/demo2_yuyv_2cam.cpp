// demo2_yuyv_2cam.cpp — 双目通道 YUYV → Y8
//
//   用法: ./demo2_yuyv_2cam <device> <num_frames> <output.y8>
//   示例: ./demo2_yuyv_2cam /dev/video0 30 twocam_yuyv.y8
//
// 管线: V4L2 YUYV → 偶数字节取 Y 分量 → 逐行写入 .y8（无损 Y8）
//
// YUYV 格式 (packed YUV 4:2:2):
//   Y0 U0 Y1 V0 | Y2 U1 Y3 V1 | ...（每 2 像素占 4 字节）
//   Y 分量位于偶数字节位置
//   stride = bytesperline（可能 > width*2）

#include "v4l2_capture.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/time.h>
#include <vector>

static double now_sec() {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    return static_cast<double>(tv.tv_sec) + static_cast<double>(tv.tv_usec) * 1e-6;
}

int main(int argc, char* argv[]) {
    if (argc != 4) {
        fprintf(stderr, "用法: %s <device> <num_frames> <output.y8>\n", argv[0]);
        fprintf(stderr, "示例: %s /dev/video0 30 twocam_yuyv.y8\n", argv[0]);
        return 1;
    }

    const char* device     = argv[1];
    int         num_frames = atoi(argv[2]);
    const char* out_path   = argv[3];

    if (num_frames <= 0) {
        fprintf(stderr, "num_frames 必须 > 0\n");
        return 1;
    }

    // ── 打开设备: YUYV 格式 ─────────────────────────────────────
    V4l2Capture cap;
    int width  = 1440;
    int height = 960;
    int fps    = 30;

    if (!cap.open(device, width, height, fps, V4L2_PIX_FMT_YUYV)) {
        fprintf(stderr, "打开设备失败: %s\n", device);
        return 1;
    }
    width  = cap.actual_width();
    height = cap.actual_height();
    int stride = cap.bytesperline();

    if (!cap.start_stream()) {
        fprintf(stderr, "启流失败\n");
        return 1;
    }

    FILE* y8_fp = fopen(out_path, "wb");
    if (!y8_fp) {
        fprintf(stderr, "无法创建输出文件: %s\n", out_path);
        return 1;
    }

    std::vector<uint8_t> y8_row(static_cast<size_t>(width));

    fprintf(stderr, "[demo2] 双目 YUYV → Y8, %dx%d, %d 帧\n", width, height, num_frames);

    int captured = 0;
    double t0 = now_sec();

    while (captured < num_frames) {
        if (!cap.wait_frame(100)) continue;

        const uint8_t* data = nullptr;
        size_t size = 0;
        if (!cap.dequeue(data, size)) continue;

        for (int row = 0; row < height; ++row) {
            const uint8_t* src = data + static_cast<size_t>(row) * static_cast<size_t>(stride);
            for (int col = 0; col < width; ++col) {
                y8_row[static_cast<size_t>(col)] = src[col * 2];
            }
            fwrite(y8_row.data(), 1, static_cast<size_t>(width), y8_fp);
        }

        captured++;
        cap.requeue();

        if (captured % 10 == 0 || captured == num_frames) {
            double elapsed = now_sec() - t0;
            fprintf(stderr, "  [%d/%d] %.1f fps\n", captured, num_frames, captured / elapsed);
        }
    }

    double elapsed = now_sec() - t0;
    size_t total_bytes = static_cast<size_t>(num_frames) * width * height;

    fclose(y8_fp);
    cap.stop_stream();
    cap.close();

    fprintf(stderr, "[demo2] 完成: %d 帧 %.2f 秒, %.1f fps, %.1f MB/s, 输出 %s (%zu bytes)\n",
            num_frames, elapsed, num_frames / elapsed,
            total_bytes / elapsed / 1e6, out_path, total_bytes);
    return 0;
}
