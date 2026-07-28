# unified_capture 源码布局重构设计

## 目标

将 `unified_capture` 从“根目录入口文件加若干散落 header”的状态，整理为按职责划分的应用层、通用层和硬件层。同步拆分 `main.cpp`，修复 Nori Xvision 迁移后构建、测试和 README 的过期信息。

重构不改变程序可执行文件名、命令行参数、socket 协议、GPIO 行为、线程模型或采集数据格式。用户确认没有项目外部代码依赖当前头文件路径，因此允许一次性迁移，不保留旧路径的转发 header。

例外：用户于 2026-07-28 明确要求 SIGSEGV/SIGABRT 不再在信号处理器中立即 `_exit(1)`；它们与 SIGINT/SIGTERM 一样只请求 Runtime 停止。这是本次重构唯一有意改变的既有致命信号处理行为。

## 目标布局

```text
unified_capture/
├── app/                         # 进程级编排；不放硬件采集细节
│   ├── main.cpp                 # 参数解析、信号安装与 Runtime 组装
│   ├── runtime.{h,cpp}          # GPIO/socket poll 主循环和 session 调度
│   ├── session_runner.{h,cpp}   # 创建、启动、停止并回收各 Sensor
│   ├── socket_server.{h,cpp}    # Unix socket 建立、读写和命令分发
│   └── gpio_control.{h,cpp}     # GPIO/LED 初始化与按键事件
├── core/                        # 无 SDK、无具体设备依赖的可复用代码
│   ├── barrier.h
│   ├── camera_config.h
│   ├── frame_queue.h
│   ├── output_path.h
│   └── time_utils.h
├── hardware/                    # 传感器、硬件 SDK 适配及设备发现
│   ├── common/sensor.{h,cpp}
│   ├── video/
│   │   ├── device_discovery.{h,cpp}
│   │   ├── video_sensor.h
│   │   ├── sixcam_sensor.h
│   │   ├── bgr2nv12.h
│   │   └── mpp_encoder.h
│   ├── imu/imu_sensor.h
│   ├── imu/imu_decode.h
│   ├── as5600/{as5600.c,as5600.h,encoder_sensor.h}
│   └── tracker/{vive_tracker_sensor.h,resample_grid.h,vive_usb.h}
├── tests/                       # 所有单元、结构和板端系统测试
├── deploy/unified_capture.service
├── docs/
├── Makefile
├── CMakeLists.txt
└── README.md
```

目录、文件名与 `#include` 一律使用小写 snake_case；仅第三方 SDK 的原始头文件名保持原样。根目录不再存放源文件、项目私有 header 或 systemd unit。

## 模块边界与控制流

`app/main.cpp` 只处理参数、SIGINT/SIGTERM 和 `Runtime` 的构造/运行，不再直接操作 socket、GPIO、相机枚举或 Sensor。

`Runtime` 仍在唯一主线程中通过 `poll()` 处理 GPIO 与 Unix socket。它持有 `SocketServer`、`GpioControl` 与 `SessionRunner`，以状态快照向 socket 提供 `status`，并将 `start`、`stop`、`preview` 命令转交给 session 层。这个边界明确保证不新建 socket 线程，维持现有 SDK 的线程约束。

`SessionRunner` 负责输出 session 目录、`SimpleBarrier`、各 `Sensor` 的构造、启动、停止、join 和预览请求。它是唯一允许同时了解“一个 session 包含哪些传感器”的模块。

Nori 枚举、VID/PID/USB 拓扑匹配与 `--scan` 移入 `hardware/video/device_discovery`；这样 SDK 调用不再留在进程入口。相机配置仍由 `core/camera_config.h` 定义，发现结果以独立的数据结构返回给 `Runtime`/`SessionRunner`，不由 socket 模块访问可变全局变量。

`core/` 不包含 Nori、MPP、libsurvive、GPIO 或 AS5600 的头文件。硬件模块可以依赖 `core/`，`app/` 可以依赖 `core/` 和 `hardware/`；禁止反向依赖。传感器生命周期基类保持在 `hardware/common`，不进入 `core`，因为其接口包含采集专用的队列和运行状态。

## 构建与部署

Makefile 和 CMake 都是受支持入口，必须引用同一组源文件并使用 Nori Xvision 的 include/library 配置、`libNori_Xvision_Std` 和 libsurvive 链接选项。删除过期的 TSTC/`USBCam_API` 名称和路径。

Makefile 改为从 `app/`、`hardware/common/`、`hardware/video/` 和 `hardware/tracker/` 显式列出 C++ 源文件，并将对象文件输出至受忽略的 `build/obj/`；C 源 `hardware/as5600/as5600.c` 继续用 C11 编译。依赖通过编译器生成的 `.d` 文件跟踪，替代当前手写且会随移动失效的长依赖行。默认目标仍输出根目录的 `./unified_capture`，保留 `make`、`make scan` 和已有测试 target 名。

CMake 改为匹配同一源清单、语言标准、依赖和可选 libsurvive 配置；它不再保留一个可配置却无法构建当前源码的旧 SDK 版本。`deploy/unified_capture.service` 移动后，README 的部署命令改为从该路径复制；用户于 2026-07-28 批准 unit 的 `ExecStart` 更新为 `--socket --single /media/usb0/capture`，以删除未实现的 `--no-vive` 并满足 SD 卡输出限制。

## 测试、文档与失败处理

纯逻辑测试全部位于 `tests/`，Makefile 的 `test_output_path` 与 `test_time_utils` 指向新路径并在 `build/tests/` 生成可执行文件。现有布局检查重命名为 `test_source_layout.sh`，只验证约定目录、关键文件和构建入口，不将每一个私有 include 作为接口契约。板端 `test_socket.sh` 路径不变。

重构本身不得改变运行时错误处理：socket 协议仍返回当前的错误 JSON；GPIO、设备发现和 session 初始化失败仍记录原因并拒绝启动；某个采集线程的清理仍必须发生在 session 结束时。模块之间以明确的错误返回值/状态快照传递失败，避免新的全局状态。

README、`tests/README.md` 与 `CLAUDE.md` 更新到 Nori Xvision、当前 `.y8` 视频灰度输出及 IMU/编码器/Tracker JSONL 输出、真实源码路径和构建命令。`docs/bugs`、`docs/decisions`、`docs/plans` 与 `docs/records` 是历史记录，除失效链接外不改写其历史事实。

## 迁移顺序

1. 建立 `app/`、`core/`、`deploy/`，将无行为变化的文件移动并更新 include。
2. 将 `hardware/IMU`、`hardware/VideoSensor` 规范为小写目录和文件名，将 tracker USB 辅助代码移入 tracker 模块；先让结构检查覆盖新布局。
3. 提取设备发现、GPIO、socket 和 session runner；每步保持主线程 `poll()` 与现有 Sensor 生命周期不变。
4. 改写 Makefile/CMake 的源清单、依赖和测试目标，更新部署路径。
5. 更新用户/测试文档，执行无硬件单元测试、结构检查、构建与板端 socket 回归。

每个移动或提取提交后都运行 `git diff --check`。如果某次拆分改变了可观察协议、参数、线程数量或输出命名，停止并先补充设计，而不是在结构重构中顺带改变行为。

## 验收标准

- 根目录仅保留构建入口、README、docs 与顶层职责目录；不含项目 C/C++ 源、私有 header 或 service 文件。
- `main.cpp` 不再含 socket 读写、GPIO sysfs 操作、Nori 枚举或 Sensor 创建/回收实现。
- `make test_output_path`、`make test_time_utils` 和布局检查均可从干净工作树运行；Make 与 CMake 都使用 Nori Xvision 而非 TSTC。
- `make` 仍产出 `./unified_capture`，既有参数与 socket 命令无需调用方修改。
- 在 RK3588 上，`make scan`、一次 GPIO/session 采集以及 `tests/test_socket.sh` 与重构前的行为一致。
