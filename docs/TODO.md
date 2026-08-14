# 待办

以下内容不属于本次已提交的 mango 腕部采集主链路，保留到后续硬件条件具备时处理。

## V4L2 迁移端到端验证 (2026-07-29)

- 已完成 Nori Xvision SDK → 纯 Linux V4L2/UVC 代码迁移（14 文件 + 1 新文件 `v4l2_device.h`）。
- **未在 RK3588 板端编译或运行**，仅在 macOS 上完成逻辑替换。
- 需在板端依次验证：编译 → `--scan` 发现 → 单路 JHH2 → SixCam → Banana → Mango 全量 → 多 Session 稳定性。

## 板端验收

- ✅ 在 RK3588 (LubanCat-4) 上以 Nori SDK v10.00.09、Rockchip MPP 1.5.0 与 libsurvive 完整编译 `unified_capture`。
- ✅ 安装 `deploy/product.conf.example` 和 `deploy/camera-map.conf.example` 后，通过 `test_mango_wrist_socket.sh` (CASE=two) 验证双腕场景。
- ✅ `SL`、`JHHSW` iProduct 匹配正确（`1bcf:2d52` 相机）。
- ✅ IMU 解码正常：腕部码带位于顶部行（横向，和 JHH2 一样），`ImuOrientation::HORIZONTAL_TOP`。
  - 左腕: 550 IMU 样本 (103KB)、右腕: 638 IMU 样本 (118KB)，3 秒采集。
- **编译修复**: 
  - `encoder_sensor.h`、`imu_sensor.h` 需 `#include <unistd.h>` (GCC 10 C++20)
  - `device_discovery.cpp`、`runtime.cpp` 移除 Nori 头文件的 `extern "C"` 包裹 (SDK 内含 C++ 头文件)
  - `wrist_profile.cpp` 中 `has_imu=true`, `ImuOrientation::HORIZONTAL_TOP`

## 独立腕部硬件自检 demo

- 在 `hardware/wrist/` 新增独立的板端诊断目标：固定检查左腕 `SL`、右腕 `JHHSW`，逐路拉取默认 30 帧 MJPEG，并报告枚举、格式、首帧大小、帧计数和 SDK 错误码。
- 该工具不得依赖服务、产品配置、录像、H.265 编码或 IMU 解码；单侧失败不得中止另一侧诊断。

## 麦克风

- 确认左右腕相机是否暴露 USB 音频接口，以及设备标识、左右关联、声道数和采样率。
- 未确认前不得加入音频枚举、录制、封装或 status 字段。
