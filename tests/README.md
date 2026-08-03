# 测试文档

## 测试分类

| 类别 | 位置 | 运行环境 | 说明 |
|------|------|---------|------|
| **单元测试** | `tests/test_*.cpp` | 任何 Linux | 纯逻辑验证，无硬件依赖；产物写入 `build/tests/` |
| **布局检查** | `tests/test_source_layout.sh` | 任何 Linux | 验证源码目录与 include 布局 |
| **系统测试** | `tests/test_*.sh` | 仅 RK3588 板端 | 需要硬件 + 程序运行中 |

## 运行测试

```bash
# 所有测试
make test

# 系统测试（需板端且程序运行中）
./tests/test_socket.sh

# banana 腕部 profile（每个 CASE 均使用新的、带时间戳的 PREFIX）
PREFIX=/media/usb0/capture/banana_$(date +%Y%m%d_%H%M%S) \
SOCK=/tmp/unified_capture.sock CASE=two \
./tests/test_banana_wrist_socket.sh
```

## 单元测试

### 要求

- 单文件 `.cpp`，`#include` 被测 header，`main()` 中用 `assert()` 验证
- 所有单元测试源码放在 `tests/`，对应可执行产物放在 `build/tests/`；Makefile 提供 `make test_<name>` target
- **不依赖** Nori SDK、MPP、libsurvive 等板端库

### 现有测试

| 文件 | 覆盖 |
|------|------|
| `test_output_path.cpp` | SD 卡路径校验、前缀生成、挂载检测 |
| `test_time_utils.cpp` | `mkdir_p` 递归目录创建 |
| `test_video_capture_control.cpp` | 视频启流顺序与预览请求控制 |
| `test_socket_command.cpp` | Socket 文本命令解析 |
| `test_source_layout.sh` | 源码目录与禁用旧路径检查 |
| `test_cherry_product_config.cpp` | Cherry 严格配置解析 |
| `test_cherry_discovery.cpp` | H.264 UVC 与 ttyACM 的 USB 父设备配对 |
| `test_cherry_protocol.cpp` | Sensor Bridge v3 编解码与校验 |
| `test_cherry_h264_writer.cpp` | H.264 字节直通与帧元数据 |
| `test_cherry_start_control.cpp` | 串口 START 就绪协调 |
| `test_cherry_json.cpp` | IMU/MAG/FRAME_META JSONL 序列化 |
| `test_cherry_serial_lifecycle.cpp` | 串口 START/STOP 与错误生命周期 |
| `test_cherry_process_utils.cpp` | FFmpeg 子进程监督工具 |
| `test_calc_cherry_sync.py` | Cherry 时间戳同步统计 |
| `test_sync_to_rk3588.sh` | 固定 `/root/unified_capture` 的安全 rsync 参数 |

### 命名约定

```
test_<module>.cpp          # 源文件
build/tests/test_<module>  # 单元测试产物
make test_<module>         # 单个测试 target
```

`make test` 是完整的无硬件回归：它运行全部主机单元测试和 `test_source_layout`。

同步并在板端执行同一套回归与生产链接：

```bash
./deploy/sync_to_rk3588.sh
ssh root@192.168.100.200 'cd /root/unified_capture && make clean && make test && make'
```

Cherry 硬件验收应使用新的 `/media/usb0/capture/` 子目录和 `timeout --signal=INT 30s` 的 `--no-gpio --single` 会话，不启停 systemd 服务。MKV 必须由 `ffprobe` 识别为 H.264、3200×1200、约 30 fps；每个 JSONL 文件逐行解析。`frame_meta.jsonl` 为空仅在 GPIO 同步线未接时可记为硬件阻塞，IMU、MAG、MKV 和 `video_frames.jsonl` 仍必须有效。

## 系统测试

系统测试在 **RK3588 板端**运行，需要 `unified_capture` 程序已在运行（socket 模式），通过 `/tmp/unified_capture.sock` 发送命令并验证响应。

### 参考实现

`test_socket.sh` — Socket 协议的完整验收脚本，覆盖以下场景：

| 序号 | 场景 | 预期结果 |
|------|------|---------|
| 1 | 启动阶段 status | 返回 JSON（可能 ready:false） |
| 2 | 等待设备就绪 | `"ready":true`，30 次 × 1s 轮询 |
| 3 | start 命令 | `"ok":true` |
| 4 | 采集中 status | `"running":true` + elapsed_ms |
| 5 | 重复 start | `"ok":false,"error":"already running"` |
| 6 | stop 命令 | `"ok":true,"elapsed_ms":NNN` |
| 7 | 重复 stop | `"ok":false,"error":"not running"` |
| 8 | 未知命令 | `"ok":false,"error":"unknown command"` |
| 9 | 停止后 status | `"running":false` |

`test_banana_wrist_socket.sh` — 仅用于 `product=banana` 的板端验收。它读取
`PREFIX` 与 `SOCK`，不会删除任何输出或启停服务。每次物理设备变化后重启服务，
并传入新的输出目录：

```bash
# 左、右腕均连接：必须非 degraded，并校验两路 HEVC MKV + 非空 IMU JSONL。
PREFIX=/media/usb0/capture/banana_two_$(date +%Y%m%d_%H%M%S) \
SOCK=/tmp/unified_capture.sock CASE=two ./tests/test_banana_wrist_socket.sh

# 仅连接任意一侧：degraded 仍可 start/stop，只能有已连接侧输出。
PREFIX=/media/usb0/capture/banana_one_$(date +%Y%m%d_%H%M%S) \
SOCK=/tmp/unified_capture.sock CASE=one ./tests/test_banana_wrist_socket.sh

# 左、右腕均断开：degraded 仍可 start/stop，session 下不得生成 MKV、JSONL 或 Y8。
PREFIX=/media/usb0/capture/banana_zero_$(date +%Y%m%d_%H%M%S) \
SOCK=/tmp/unified_capture.sock CASE=zero ./tests/test_banana_wrist_socket.sh
```

脚本在每个有设备的目录调用 `ffprobe`，要求视频编码为 `hevc`，并拒绝任意 `.y8`。

### 编写新的系统测试

1. 新建 `tests/test_<feature>.sh`
2. 首行 `#!/bin/bash`，使用 `nc -U /tmp/unified_capture.sock` 与程序通信
3. 每个用例按 `序号. 场景描述 → 预期结果` 结构组织
4. 响应为单行 JSON，用 `grep` 验证关键字段
5. 完成后更新本文档的"系统测试"表格

### 系统测试约定

- 脚本自身不启停 `unified_capture`，由测试者手动启停或 systemd 管理
- round-trip 验证：`start` → `status`(running) → `stop` → `status`(idle)
- 边界条件必须覆盖：重复 start/stop、未知命令
