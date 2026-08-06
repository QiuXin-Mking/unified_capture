# Cherry profile implementation

## Goal

在不改变 mango/banana 行为的前提下，新增 `cherry` profile，采集 YCTC SC233HGS 双目视频以及 CDC ACM 中的 IMU、MAG 和 FRAME_META，并提供同步质量分析工具。

## Requirements

- `product=cherry` 选中新 profile，不创建 AS5600、VIVE、码带 IMU 或 Y8 输出。
- 按同一 USB 物理设备父路径关联 `5268:1218` 的 UVC capture 节点与 `/dev/ttyACM*`，排除 UVC metadata 节点。
- 串口使用 Sensor Bridge v3：921600 8N1，发送 `START(mask=0x07)` 并等待同 seq 响应，流式解析 IMU/MAG/FRAME_META，停止时发送 `STOP`。
- 串口输出为 `cherry_stereo/imu.jsonl`、`mag.jsonl`、`frame_meta.jsonl`，数据字段与已确认需求一致。
- 视频输出为 `cherry_stereo/cherry_stereo.mkv`，并记录 `video_frames.jsonl` 的 V4L2 sequence 和 timestamp。
- 串口 START 成功后才允许视频启流；任一端初始化失败时不得永久阻塞 session barrier。
- `deploy/calc_cherry_sync.py` 支持本地 session 目录和 `user@host:<path>`，输出 IMU↔视频、IMU↔MAG、MAG↔视频的 p50、p90、≤10us、≤100us、min、max，忽略超过 100ms 的匹配。
- 腕部 cherry 硬件未到货，本期只保留配置和类的可复用能力，不伪造可运行设备、分辨率或帧率。
- 视频输入按用户 2026-08-03 最终确认的实机路径实现：`V4L2 H264 -> FIFO -> ffmpeg remux -> MKV`，不经过 MPP；见 `research/2026-08-03-board-inventory.md`。

## Acceptance Criteria

- [ ] Host 单元测覆盖 START/STOP 已知字节、CRC 拒绝、粘包/拆包与损坏帧重同步、IMU/MAG/FRAME_META 解码。
- [ ] Host 单元测覆盖 cherry 配置、USB 视频/串口配对、session profile/status 和同步分析。
- [ ] `make test` 中所有与 cherry 相关的新测试通过，既有测试无新增回归。
- [ ] RK3588 板端能编译 `unified_capture`，`--scan` 能报告 cherry 的 UVC/CDC 配对。
- [ ] RK3588 实采至少 30 秒，MKV 可被 `ffprobe` 识别为 H.264，IMU 和 MAG JSONL 非空且可解析。
- [ ] 有 GPIO 帧同步数据时验证 FRAME_META；若板端无数据，记录为硬件接线依赖，不伪报通过。

## Notes

- 权威需求来源：`docs/design/cherry-profile-requirements.md`。
- 协议来源：`/Users/qiuxin/code/qiuxin_aliyun_back/嵌入式项目/15-郭总给的sdk/YCTC_SC233HGS_protocol/docs/UART_PROTOCOL.md`。
