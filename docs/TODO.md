# 待办

以下内容不属于本次已提交的 banana 腕部采集主链路，保留到后续硬件条件具备时处理。

## 板端验收

- 在 RK3588 上以实际 Nori SDK、Rockchip MPP 与 libsurvive 完整编译 `unified_capture`。
- 安装 `deploy/product.conf.example` 和 `deploy/camera-map.conf.example` 后，按 `tests/test_banana_wrist_socket.sh` 分别验证双腕、单腕和零设备降级场景。
- 将 `SL`、`JHHSW` 替换为量产设备实际报告的精确 Nori `iProduct`（如有差异）。

## 独立腕部硬件自检 demo

- 在 `hardware/wrist/` 新增独立的板端诊断目标：固定检查左腕 `SL`、右腕 `JHHSW`，逐路拉取默认 30 帧 MJPEG，并报告枚举、格式、首帧大小、帧计数和 SDK 错误码。
- 该工具不得依赖服务、产品配置、录像、H.265 编码或 IMU 解码；单侧失败不得中止另一侧诊断。

## 麦克风

- 确认左右腕相机是否暴露 USB 音频接口，以及设备标识、左右关联、声道数和采样率。
- 未确认前不得加入音频枚举、录制、封装或 status 字段。
