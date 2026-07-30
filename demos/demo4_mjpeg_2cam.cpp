// demo4_mjpeg_2cam.cpp — 双目通道 MJPEG → Y8（当前生产管线路径）
//
//   用法: ./demo4_mjpeg_2cam <device> <num_frames> <output.y8>
//   示例: ./demo4_mjpeg_2cam /dev/video0 30 twocam_mjpeg.y8
//
// 管线: V4L2 MJPEG → tjDecompressToYUVPlanes → Y 平面 → 逐行写入 .y8
// 与生产代码 mjpeg_yuv_decoder.h + video_frame_processor.h 一致

#include "v4l2_capture.h"

#include <turbojpeg.h>

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
        fprintf(stderr, "示例: %s /dev/video0 30 twocam_mjpeg.y8\n", argv[0]);
        return 1;
    }

    const char* device     = argv[1];
    int         num_frames = atoi(argv[2]);
    const char* out_path   = argv[3];

    if (num_frames <= 0) {
        fprintf(stderr, "num_frames 必须 > 0\n");
        return 1;
    }

    // ── 打开设备: MJPEG 格式 ─────────────────────────────────────
    V4l2Capture cap;
    int width  = 1440;
    int height = 960;
    int fps    = 30;

    if (!cap.open(device, width, height, fps, V4L2_PIX_FMT_MJPEG)) {
        fprintf(stderr, "打开设备失败: %s\n", device);
        return 1;
    }
    width  = cap.actual_width();
    height = cap.actual_height();

    if (!cap.start_stream()) {
        fprintf(stderr, "启流失败\n");
        return 1;
    }

    tjhandle tj = tjInitDecompress();
    if (!tj) {
        fprintf(stderr, "tjInitDecompress 失败\n");
        return 1;
    }

    FILE* y8_fp = fopen(out_path, "wb");
    if (!y8_fp) {
        fprintf(stderr, "无法创建输出文件: %s\n", out_path);
        tjDestroy(tj);
        return 1;
    }

    std::vector<uint8_t> y8_row(static_cast<size_t>(width));

    fprintf(stderr, "[demo4] 双目 MJPEG → Y8, %dx%d, %d 帧\n", width, height, num_frames);

    int captured = 0;
    double t0 = now_sec();

    while (captured < num_frames) {
        if (!cap.wait_frame(100)) continue;

        const uint8_t* jpeg_data = nullptr;
        size_t jpeg_size = 0;
        if (!cap.dequeue(jpeg_data, jpeg_size)) continue;

        // ── MJPEG → YUV planes ────────────────────────────────
        int jpeg_width, jpeg_height, subsamp, colorspace;
        if (tjDecompressHeader3(tj,
                reinterpret_cast<const unsigned char*>(jpeg_data),
                static_cast<unsigned long>(jpeg_size),
                &jpeg_width, &jpeg_height, &subsamp, &colorspace) < 0) {
            fprintf(stderr, "tjDecompressHeader3 失败: %s\n", tjGetErrorStr2(tj));
            cap.requeue();
            continue;
        }

        int plane_count = (subsamp == TJSAMP_GRAY) ? 1 : 3;

        std::vector<uint8_t> y_plane(static_cast<size_t>(jpeg_width) * jpeg_height);
        std::vector<uint8_t> u_plane, v_plane;
        if (plane_count >= 3) {
            int chroma_w = tjPlaneWidth(1, jpeg_width, subsamp);
            int chroma_h = tjPlaneHeight(1, jpeg_height, subsamp);
            u_plane.resize(static_cast<size_t>(chroma_w) * chroma_h);
            v_plane.resize(static_cast<size_t>(chroma_w) * chroma_h);
        }

        const unsigned char* planes[3] = { y_plane.data(), u_plane.data(), v_plane.data() };
        int strides[3] = { jpeg_width, 0, 0 };
        if (plane_count >= 3) {
            strides[1] = tjPlaneWidth(1, jpeg_width, subsamp);
            strides[2] = tjPlaneWidth(1, jpeg_width, subsamp);
        }

        if (tjDecompressToYUVPlanes(tj,
                reinterpret_cast<const unsigned char*>(jpeg_data),
                static_cast<unsigned long>(jpeg_size),
                const_cast<unsigned char**>(planes),
                jpeg_width, strides, jpeg_height, TJFLAG_FASTDCT) < 0) {
            fprintf(stderr, "tjDecompressToYUVPlanes 失败: %s\n", tjGetErrorStr2(tj));
            cap.requeue();
            continue;
        }

        // ── Y 平面 → Y8 ───────────────────────────────────────
        int y_stride = jpeg_width;
        for (int row = 0; row < jpeg_height; ++row) {
            const uint8_t* src = y_plane.data() + static_cast<size_t>(row) * y_stride;
            fwrite(src, 1, static_cast<size_t>(jpeg_width), y8_fp);
        }

        captured++;
        cap.requeue();

        if (captured % 10 == 0 || captured == num_frames) {
            double elapsed = now_sec() - t0;
            fprintf(stderr, "  [%d/%d] %.1f fps, 解码 %dx%d (subsamp=%d)\n",
                    captured, num_frames, captured / elapsed,
                    jpeg_width, jpeg_height, subsamp);
        }
    }

    double elapsed = now_sec() - t0;
    size_t total_bytes = static_cast<size_t>(num_frames) * width * height;

    fclose(y8_fp);
    tjDestroy(tj);
    cap.stop_stream();
    cap.close();

    fprintf(stderr, "[demo4] 完成: %d 帧 %.2f 秒, %.1f fps, %.1f MB/s, 输出 %s (%zu bytes)\n",
            num_frames, elapsed, num_frames / elapsed,
            total_bytes / elapsed / 1e6, out_path, total_bytes);
    return 0;
}
