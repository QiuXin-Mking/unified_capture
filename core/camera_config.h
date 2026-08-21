#pragma once
#include <cstdint>
#include <cstring>

// IMU 码带在图像中的方向
enum class ImuOrientation {
    HORIZONTAL_TOP,     // 码带数据沿图像行排列，JHH2/JHH02: 横向码带, 仅在顶部
    VERTICAL_LEFT,      // 码带数据沿图像列排列
};

struct CameraConfig {
    const char* name;
    uint16_t vid, pid;    // USB VID/PID for device matching (1bcf:2d50, 1bcf:2d51)
    int  group_order;     // 0-based within same VID/PID group (0=first, 1=second)
    int  width, height;
    int  fps;
    int  bitrate;        // bps, e.g. 16000000
    int  gop;
    bool has_imu;
    ImuOrientation imu_orientation;  // 仅 has_imu=true 时有效
    bool output_h265 = true;         // 是否编码 H.265 → MKV
    bool output_y8  = true;          // 是否写 Y8 原始灰度文件
    bool output_audio = false;       // 是否采集麦克风音频封装进 MKV（仅六目 jhh02 有 UAC 麦克风）
};
