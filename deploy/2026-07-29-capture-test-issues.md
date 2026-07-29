# 2026-07-29 采集测试问题记录

## 测试环境

- **板端**: RK3588, `root@192.168.100.200`
- **程序**: `unified_capture --no-gpio`, banana 配置
- **配置**: `allow_missing_devices=true`, `sixcam.enabled=true`
- **时长**: 10 秒
- **预期帧率**: 全部 30fps → 预期约 300 帧/通道

## 实际采集结果

| 通道 | 设备 | 分辨率 | 帧数 | MKV | Y8 | IMU | 首帧延迟 |
|------|------|--------|------|-----|-----|------|---------|
| jhh02 | 1bcf:2d50 | 4000×1200@30 | **15** | 12MB | 69MB | 28K | ~1235 空轮询 |
| jhh04 | 1bcf:2d51 | 3104×480@30 | **49** | — | 70MB | 90K | ~1050 空轮询 |
| wrist_right | JHHSW 1bcf:2d52 | 1440×960@30 | **92** | 45MB | — | 175K | ~793 空轮询 |
| wrist_left | SL 1bcf:2d52 | 1440×960@30 | **0** | 0 | — | 0 | 始终阻塞 |

## 问题 1: 所有通道帧数严重偏低

- **现象**: 10 秒采集，jhh02 仅 15 帧，jhh04 仅 49 帧，wrist_right 仅 92 帧。按 30fps 预期应各约 300 帧。
- **复现特征**:
  - `GetFrameBuff` 频繁返回 NULL，`usleep(1000)` 空转
  - 首帧前有大量空轮询: wrist_right 793 次、jhh04 1050 次、jhh02 1235 次
  - 各通道实际分辨率与配置一致 (`actual resolution 4000x1200 (cfg=4000x1200)`)，不是分辨率不匹配
- **可能方向**: USB 带宽瓶颈（3 个 USB3 相机同时拉流）、Nori SDK 内部帧缓冲策略、轮询间隔太大
- **相关日志**:
  ```
  [jhh02] DBG collect: first frame after 1235 empty polls
  [jhh02] DBG collect: frame=15, h265=12426532
  [jhh04] DBG collect: first frame after 1050 empty polls
  [jhh04] DBG: frame=30, h265=0
  [wrist_right] DBG collect: first frame after 793 empty polls
  [wrist_right] DBG collect: frame=92, last_mjpg_len=208255, h265_total=46481070
  ```

## 问题 2: wrist_left (SL 相机) 零帧

- **现象**: wrist_left 全程无帧，`GetFrameBuff` 始终返回 NULL（7285 次空轮询后退出）
- **日志特征**:
  - `DeviceVideoInit` 成功，`VideoStart` 成功
  - `[wrist_left] setup OK (dev_id=0, 1440x960@30fps, H265=Y, Y8=N)` — setup 正常
  - 进入 `collect` 后 `GetFrameBuff NULL (first)`，之后再无帧
  - ffmpeg 报 `Output file #0 does not contain any stream`（因为没有任何 H.265 数据写入 FIFO）
  - IMU 日志: `0 frames scanned, 0 with data, 0 bytes total`
- **同一 bus 的 wrist_right (JHHSW) 正常产帧**: 92 帧，说明 USB 控制器没问题
- **可能方向**: SL 相机硬件问题（固件/线缆/供电）、SL 是否支持 MJPG@1440×960、Nori SDK 对 SL 的兼容性
- **相关日志**:
  ```
  Nori Xvision SDK: found 4 device(s)
    Device[0]: 1bcf:2d52 product="SL" formats=3
    Device[1]: 1bcf:2d52 product="JHHSW" formats=3
  ```
  两个 1bcf:2d52 设备都枚举到了，但只有 JHHSW 产帧。

## 补充上下文

- 代码已移除 hardware trigger 模式，全部使用 `NON_TRIGGER_MODE`
- wrist_left 由 `banana` profile 分配为 `group_order=0` 的 `1bcf:2d52` 设备
- SDK 轮询: `Nori_Xvision_GetFrameBuff(device_id, false, 0)`, 空帧时 `usleep(1000)`
- 可能存在内存/磁盘问题: 板端时间漂移（`检测到时钟错误`，文件时间戳显示 `4月13`）
