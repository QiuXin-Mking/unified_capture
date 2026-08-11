# VIVE Tracker 3.0

## 验证状态

- **已在项目中实现**：README 将设备标为 VIVE Tracker 3.0，接口为 USB HID；`hardware/tracker/vive_usb.h` 按 USB VID/PID `28de:2300` 扫描和匹配设备。项目以可选依赖 `libsurvive` 初始化、轮询并接收 pose 回调。
- **已观察到的验收证据**：RK3588 短时会话在停止前完成 Tracker global solve。重采样文件中，T20 有 114 行、T21 有 119 行，相邻同设备 timecode 均为 `480000`（48 MHz 时钟的 100 Hz 间隔）。

## 当前输出

采集期间，pose 回调将原始样本写入 session 根目录的 `tracker_raw.jsonl`：

| 字段 | 含义 |
|---|---|
| `ts_us` | 相对采集起点的单调时钟微秒时间戳 |
| `timecode` | libsurvive 提供的 Tracker 时钟值 |
| `codename` | libsurvive 设备代号 |
| `x`、`y`、`z` | 位置 |
| `qw`、`qx`、`qy`、`qz` | 姿态四元数 |

停止时，项目按每个 `codename` 独立建立 48 MHz timecode 网格，以最近样本重采样为 100 Hz，并写入 `tracker.jsonl`。该文件包含上述字段，另加 `method`（当前固定为 `nearest`）。不同 Tracker 不共用重采样网格。

## 运行风险与待确认

- **已知风险**：历史 RK3588 验收中，`tracker.jsonl` 已写完、其他传感器线程已结束后，进程仍阻塞于 `survive_close()`；SIGINT 后不能自行到达 `Session DONE`，测试以强制结束进程收尾。此退出路径仍是待解决问题。
- **已知风险**：全负载 USB 长时采集尚不稳定。历史较长测试出现 Tracker 断开、libusb mutex assertion，以及 `xhci-hcd` 无法及时访问内存的内核日志；现有短测不证明长时稳定性。
- **待确认**：代码匹配的 USB VID/PID 已明确为 `28de:2300`，但尚未将该匹配到的两台实际连接设备与实物库存／物理 USB 端口对应起来；线缆、供电及 USB 控制器／Hub 拓扑也仍未确认。
- **待确认**：需要在隔离／专用控制器或调整共享 USB 2.0 Hub 后，完成至少 30 秒全传感器验证：无断开或 fatal assertion、自动 `Session DONE`，且各设备持续满足 100 Hz（timecode 间隔 `480000`）。

## 依赖与边界

`libsurvive` 是可选依赖。代码包含 USB HID 占用导致 libsurvive 无法 claim 时的 usbfs unbind/rebind 辅助逻辑；这不构成对具体主机驱动状态或现场接线的保证。

## 证据来源

- `hardware/tracker/vive_tracker_sensor.h`
- `hardware/tracker/vive_usb.h`
- `README.md`
- `docs/2026-07-27-hardware-migration-sampling-validation.md`
