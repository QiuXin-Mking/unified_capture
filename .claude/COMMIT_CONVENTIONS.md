# Git Commit 规范

## 格式

```
type: 标题（一行概括）

- 细则一：具体改了什么 + 为什么
- 细则二：另一个改动点 + 收益
- 细则三：其他说明
```

第 1 行为标题，第 3~10 行为细则（最少 2 条，最多 8 条），`- ` 开头。

## type 标签

| type | 含义 | SemVer |
|------|------|--------|
| `feat` | 新功能 | MINOR |
| `fix` | 修 bug | PATCH |
| `docs` | 只动文档/注释/README | — |
| `style` | 只动格式（缩进、空格、分号、lint），不改逻辑 | — |
| `refactor` | 重构，不加功能不修 bug，只改结构/命名/解耦 | — |
| `perf` | 性能优化（算法、缓存、内存、包体积） | PATCH |
| `test` | 增删改测试用例 | — |
| `build` | 动构建系统（CMake/Dockerfile/依赖升级） | — |

## 标题要求

- **必须用中文**
- 一行概括本次 commit 的核心，50 字符以内
- 不写句号

## 细则要求

- 2~8 条，每条 `- ` 开头
- 说清：**改了什么 + 为什么 / 收益是什么**
- 每条写一个独立改动点，不要混杂
- 禁止空话：如 "优化了代码"、"完善了逻辑"
- **细则数量与代码修改量正相关**：改动越多，细则越多

| 代码修改量 | 细则数 |
|-----------|--------|
| ~10 行，单文件 | 2 条 |
| ~50 行，2~3 文件 | 3~4 条 |
| ~200 行，多文件/多模块 | 5~6 条 |
| 500+ 行，跨模块重构 | 7~8 条 |

## 示例

```
feat: 新增磁力计 AK09940 18bit 解析，IMU 码带输出 mx/my/mz

- 在 imu_sensor.cpp 新增 parse_magnetometer()，按 datasheet 解析 18bit 补码
- 每轴 3 字节小端序，量程 ±4912 μT，与参考设备对比误差 < 2%
- mx/my/mz 字段随 IMU JSONL 输出，向后兼容不影响历史数据格式
```

```
fix: 修复 V4L2 多 Session 切换偶发死锁

- 旧 BGR FrameQueue 在 stop→start 时未正确 drain，残留帧阻塞新 session
- 改为 NV12 单队列 + SimpleBarrier 同步，四路启流帧间偏差 < 1ms
- 100 次连续启停压测不再死锁，原来 ~5% 概率复现
```

```
refactor: 传感器生命周期统一为 Sensor::setup/collect/teardown

- 原先各 Sensor 线程启动停止逻辑散落，VideoSensor 200 行、ImuSensor 150 行
- 统一到 Sensor 基类虚函数，子类只需实现三个回调各 ~30 行
- 新增 SimpleBarrier 替代 std::barrier，兼容 GCC 10 + ARM64 交叉编译
```

```
perf: MJPEG 解码改用 turbojpeg，CPU 占用从 22% 降到 8%

- 原 libjpeg 解码为单线程，四路 30fps 时 CPU 跑满 2 核
- turbojpeg 利用 ARM NEON 加速，单帧解码从 3.2ms 降到 0.8ms
- 收益：四路采集总 CPU 从 88% 降到 35%，SD 卡写入零掉帧
```
