# Cherry 板端端到端采集与同步验收设计

## 目标

新增板端一键验收脚本，采集一段真实 Cherry 数据，验证视频和串口产物完整性，并统计 IMU、MAG、GPIO FRAME_META 与视频帧结束时刻之间的同步差值。

脚本只输出同步统计，不对时间差设置通过/失败阈值。只有采集过程失败、必需产物缺失或为空、JSONL/视频格式非法时才返回非零。

## 文件与职责

### `deploy/test_cherry_e2e.py`

板端验收入口，职责如下：

1. 检查 `unified_capture`、`ffprobe`、Cherry 配置和必需设备节点。
2. 创建唯一输出前缀并启动：

   ```bash
   unified_capture --no-gpio --single <output_prefix>
   ```

3. 达到指定采集时长后向采集进程发送 SIGINT，并在宽限期内等待正常 teardown；超时后才强制终止。
4. 定位本次生成的 `session_NNN/cherry_stereo`，禁止误用历史 session。
5. 使用 `ffprobe` 验证 MKV：H.264、3200×1200、约 30 fps，并输出帧数、时长和文件大小。
6. 逐行解析 `imu.jsonl`、`mag.jsonl`、`frame_meta.jsonl`、`video_frames.jsonl`，验证文件非空、每行是合法 JSON，并输出批次/帧数。
7. 调用共享同步分析函数输出四组统计。

CLI 支持：

```text
--duration SEC       采集时长，默认 30
--output-root PATH   输出根目录，默认 /media/usb0/capture
--binary PATH        unified_capture 路径，默认当前项目的 ./unified_capture
--analyse-only PATH  不采集，只分析已有 session 或 cherry_stereo 目录
```

真实采样默认保留，脚本在结尾打印绝对路径，便于后续复查。

### `deploy/calc_cherry_sync.py`

保留现有离线 CLI，并扩展为可复用模块：

- 加载 `video_frames.jsonl.v4l2_timestamp_us`，命名为 `VIDEO_END`。
- 将 V4L2 完成 dequeue 并复制压缩帧时记录的单调时钟视为“视频帧结束时刻”。
- 提供结构化统计函数，返回 `count/p50/p90/min/max`。
- 继续保留最近邻绝对时间差算法和 100 ms 最大配对窗口；超过窗口的样本不进入统计。

## 同步统计

脚本输出以下四组最近邻绝对差值，单位为微秒：

1. `IMU ↔ MAG`
2. `IMU ↔ VIDEO_END`
3. `MAG ↔ VIDEO_END`
4. `FRAME_META ↔ VIDEO_END`

每组输出：

```text
count=<n> p50=<us> p90=<us> min=<us> max=<us>
```

IMU 时间线由 gyro 和 accelerometer 样本的 `pts_us` 合并、去重并排序；MAG 使用 `pts_us`；FRAME_META 使用 `frame_pts_us`；VIDEO_END 使用 `v4l2_timestamp_us`。

若某条时间线为空，该组打印 `unavailable`，同时因为必需 JSONL 非空校验失败，端到端采集模式返回非零。离线分析模式允许打印 `unavailable`，便于诊断不完整的历史数据。

## 通过与失败语义

端到端模式的必需条件：

- 采集进程能够启动并在 SIGINT 后完成 teardown；GNU `timeout` 风格的 124 不作为本脚本接口。
- `cherry_stereo.mkv` 存在、非空，codec/resolution/fps 正确。
- 四个 JSONL 文件均存在、非空且每行可解析。
- IMU、MAG、FRAME_META、VIDEO_END 时间线均非空。
- 四组同步统计至少各有一个 100 ms 窗口内的配对。

同步数值本身不设阈值，不因 p90 较大返回失败。

## 错误处理与安全

- 脚本不修改 `/etc/unified_capture` 配置，不启动或重启 systemd 服务。
- 启动前确认活动配置为 `product=cherry`；否则明确报错退出。
- 输出目录使用时间戳和 PID，避免覆盖历史数据。
- SIGINT 后等待有限宽限期；仅在进程不退出时发送 SIGKILL，并返回失败。
- 子进程 stdout/stderr 同时写入验收日志并在终端显示，便于定位协议、ffmpeg 或 V4L2 错误。
- 不删除真实采样数据。

## 测试策略

遵循 TDD：

1. 扩展 `tests/test_calc_cherry_sync.py`，先验证 VIDEO_END 加载及四组结构化统计。
2. 新增 `tests/test_cherry_e2e.py`，使用临时目录、假采集进程和假 ffprobe，覆盖：
   - SIGINT 后正常退出；
   - 精确定位本次 session；
   - MKV/JSONL 成功验收；
   - 文件缺失、空文件、坏 JSON、错误 codec/resolution/fps；
   - 无同步配对；
   - analyse-only 模式。
3. 将测试加入 `make test` 和源码布局检查。
4. 主机完整测试通过后运行 rsync。
5. RK3588 上执行至少一次 30 秒真实采集，并保存命令、产物计数、ffprobe 和四组同步统计。

## 非目标

- 不改变采集协议、时间戳生成方式或设备固件。
- 不根据统计结果自动校准时间轴。
- 不为 p50/p90 设置业务阈值。
- 不操作 systemd 服务或自动改写板端产品配置。
