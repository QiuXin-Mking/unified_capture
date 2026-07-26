# 多进程架构设计 — 解决 TSTC SDK 同 VID/PID 冲突

> 2026-07-26

## 问题

TSTC SDK v1.0.0 内部按 VID/PID 维护全局状态。同进程内两个 JHH2 设备（jhh2_left + 六目 jhh02，均为 `1bcf:2d50`）不能同时启流——第二个 STREAM_STATUS 必死锁。

## 方案：fork + 命令行参数 + waitpid

```
                    ┌─────────────────────┐
                    │   主进程 (master)    │
                    │  - 六目 SixCam      │
                    │  - VIVE Tracker ×2  │
                    │  - 右双目 (0b20)    │
                    │  - 会话管理         │
                    │  - 协调子进程       │
                    └──┬──────────────┬───┘
                       │ fork+exec    │ fork+exec
              ┌────────▼──┐    ┌──────▼──────┐
              │ jhh2_left │    │ jhh2_right  │
              │  (worker) │    │  (worker)   │
              │  PID 2d50 │    │  PID 0b20   │
              │  单独进程  │    │  单独进程   │
              └───────────┘    └─────────────┘
```

### 进程通讯

不需要复杂的 IPC。用最简单的方式：

| 方向 | 机制 | 用途 |
|------|------|------|
| 主 → 子 | 命令行参数 | 传递 session_dir, session_num, duration |
| 子 → 主 | exit code + stderr | 报告成功/失败 |
| 同步 | 文件锁 / ready file | 子进程在 setup 完后写 `.ready`，主进程等待 |
| 停止 | SIGTERM | 主进程杀子进程停止采集 |

### Worker 命令行

```bash
unified_worker --camera jhh2_left \
               --session /data/capture/session_001 \
               --duration 0 \     # 0=无限，等 SIGTERM
               --output-y8 \
               --output-h265
```

### 主进程流程

```
1. 创建 session_dir
2. fork 子进程 (jhh2_left, jhh2_right, ...)
3. 等待所有子进程的 .ready 文件
4. 初始化自己的传感器 (六目, VIVE)
5. 采集 (所有进程并行)
6. 采集结束 → SIGTERM 所有子进程
7. waitpid 收集退出状态
8. 清理
```

### 代价

- 子进程初始化 2-3 秒（fork + exec + TSTC setup）
- 不再有统一的 Barrier 同步点（改为 ready file 等待）
- 子进程崩溃需要主进程检测并重启

### 优点

- ✅ 彻底解决 VID/PID 冲突
- ✅ 子进程隔离，crash 不影响主进程
- ✅ 可以独立重启某个 worker
- ✅ 不需要改 TSTC SDK
- ✅ 通讯极简，没有复杂的 IPC 协议

## 备选：Unix Socket JSON 通讯

如果需要更精细的控制（动态启停、状态查询、预览帧回传），可以用 Unix socket：

```
主进程 ←→ /tmp/worker_jhh2_left.sock  (JSON 消息)
主进程 ←→ /tmp/worker_jhh2_right.sock
```

消息格式：
```json
→ {"cmd":"start","session":"/data/capture/session_001","duration":0}
← {"status":"ready"}
← {"status":"collecting","frames":1234}
← {"status":"done","frames":5678,"size_mb":123}
→ {"cmd":"stop"}
```

但这比 fork+参数方案复杂得多。建议先用简单方案，socket 作为升级选项。
