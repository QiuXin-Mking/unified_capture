# unified_capture 源码布局重构 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 将源码按 app/core/hardware 职责分层，并把 `main.cpp` 拆为可独立理解和验证的进程控制模块，同时不改变 RK3588 的采集行为。

**Architecture:** `app/` 保留入口、唯一主线程 `poll()`、socket、GPIO 和 session 调度；`core/` 只放无 SDK 的通用类型；`hardware/` 包含设备发现与传感器实现。`Runtime` 拥有进程状态，`SessionRunner` 只负责传感器生命周期，socket 永远不创建额外 pthread。

**Tech Stack:** C++20 / GCC 10、C11、GNU Make、CMake 3.16、Nori Xvision v10.00.09、Rockchip MPP、turbojpeg、libgpiod、libsurvive、Unix Domain Socket。

## Global Constraints

- 保持 `./unified_capture`、既有参数、socket 文本协议、GPIO 行为、传感器线程数和输出文件格式不变；用户于 2026-07-28 授权例外：SIGSEGV/SIGABRT 与 SIGINT/SIGTERM 一样只请求 Runtime 停止，不再调用 `_exit(1)`。
- 保持 `--scan`、`--no-gpio`、`--socket`、`--no-as5600`、`--no-imu`、`--no-h265`、`--single`、`-h/--help`；README 不再声明当前未实现的 `--no-vive`。
- 输出继续限制在 `/media/usb0/capture`；Nori 初始化在进程退出时调用一次 `Nori_Xvision_UnInit()`。
- `core/` 不得包含 Nori、MPP、libsurvive、gpiod 或 AS5600 头文件；依赖只能是 `app → {core, hardware}` 和 `hardware → core`。
- 项目目录和文件名使用小写 snake_case；没有旧 include 路径兼容层。
- 每个任务结束前运行指定测试、`git diff --check`，并提交单一目的的变更。

---

## File Structure

| 路径 | 职责 |
|---|---|
| `app/main.cpp` | CLI、信号处理器、构造 `Runtime`。 |
| `app/runtime.{h,cpp}` | 主线程 poll、模式循环、命令响应、资源清理。 |
| `app/session_runner.{h,cpp}` | session 目录、Sensor 构造/启动/join、`SimpleBarrier`。 |
| `app/socket_server.{h,cpp}` | Unix socket 生命周期、请求解析、单客户端读写。 |
| `app/gpio_control.{h,cpp}` | GPIO 按键和 LED 生命周期。 |
| `core/{barrier,camera_config,frame_queue,output_path,time_utils}.h` | 无硬件依赖代码。 |
| `hardware/video/device_discovery.{h,cpp}` | Nori 扫描与以 USB bus 为边界的相机分配。 |
| `hardware/video/capture_control.h` | 视频启流协调和预览请求状态。 |
| `hardware/{common,video,imu,as5600,tracker}/` | 统一小写的硬件实现目录。 |
| `tests/test_source_layout.sh` | 目录契约与根目录清洁度。 |
| `tests/test_video_capture_control.cpp` | 启流重置和预览请求单元测试。 |
| `tests/test_socket_command.cpp` | socket 命令解析单元测试。 |
| `deploy/unified_capture.service` | systemd 部署单元。 |

## Task 1: 建立目录契约并完成纯移动

**Files:**
- Create: `app/`, `core/`, `deploy/`, `tests/test_source_layout.sh`
- Move: `main.cpp` → `app/main.cpp`；根目录五个通用 header → `core/`；`vive_usb.h` → `hardware/tracker/vive_usb.h`；`unified_capture.service` → `deploy/unified_capture.service`
- Move: `hardware/VideoSensor/` → `hardware/video/`，`hardware/IMU/` → `hardware/imu/`；`VideoSensor.h`、`SixCamSensor.h`、`ImuSensor.h`、`ViveTrackerSensor.h` 分别改为全小写文件名
- Modify: 所有第一方 `#include`、两个现有 C++ 单元测试
- Rename: `tests/test_hardware_header_layout.sh` → `tests/test_source_layout.sh`

**Interfaces:**
- Consumes: 当前 header-only Sensor 和 `hardware/common/sensor.{h,cpp}`。
- Produces: 第一方 include 前缀：`core/...`、`hardware/common/...`、`hardware/video/...`、`hardware/imu/...`、`hardware/tracker/...`。

- [ ] **Step 1: 先写失败的目录检查。**

将重命名后的 `tests/test_source_layout.sh` 替换为：

```sh
#!/bin/sh
set -eu
for path in app/main.cpp core/barrier.h core/camera_config.h core/frame_queue.h core/output_path.h core/time_utils.h hardware/common/sensor.h hardware/common/sensor.cpp hardware/video/video_sensor.h hardware/video/sixcam_sensor.h hardware/video/bgr2nv12.h hardware/video/mpp_encoder.h hardware/imu/imu_sensor.h hardware/imu/imu_decode.h hardware/as5600/as5600.c hardware/as5600/as5600.h hardware/as5600/encoder_sensor.h hardware/tracker/vive_tracker_sensor.h hardware/tracker/vive_usb.h deploy/unified_capture.service; do test -f "$path"; done
for path in main.cpp barrier.h camera_config.h frame_queue.h output_path.h time_utils.h vive_usb.h unified_capture.service; do test ! -e "$path"; done
for path in hardware/IMU hardware/VideoSensor; do ! git ls-files | grep -Fqx "$path"; done
```

在大小写敏感文件系统上，第二个循环同样证明旧目录没有被追踪。在 macOS 等大小写不敏感文件系统上，`test ! -e hardware/IMU` 会把新目录 `hardware/imu` 当作同一路径，因此使用 Git 的精确、大小写敏感的已追踪路径作为跨平台目录迁移断言。用户已于 2026-07-28 批准这项调整。

- [ ] **Step 2: 运行检查并确认失败。**

Run: `sh tests/test_source_layout.sh`
Expected: FAIL，第一处缺失为 `app/main.cpp` 或 `core/barrier.h`。

- [ ] **Step 3: 使用 `git mv` 一次性移动文件。**

```bash
mkdir -p app core deploy
git mv main.cpp app/main.cpp
git mv barrier.h camera_config.h frame_queue.h output_path.h time_utils.h core/
git mv vive_usb.h hardware/tracker/vive_usb.h
git mv unified_capture.service deploy/unified_capture.service
git mv hardware/VideoSensor hardware/video
git mv hardware/IMU hardware/imu
git mv hardware/video/VideoSensor.h hardware/video/video_sensor.h
git mv hardware/video/SixCamSensor.h hardware/video/sixcam_sensor.h
git mv hardware/imu/ImuSensor.h hardware/imu/imu_sensor.h
git mv hardware/tracker/ViveTrackerSensor.h hardware/tracker/vive_tracker_sensor.h
git mv tests/test_hardware_header_layout.sh tests/test_source_layout.sh
```

- [ ] **Step 4: 改写 include 到新前缀。**

所有项目 include 采用下列形式：

```cpp
#include "core/camera_config.h"
#include "core/frame_queue.h"
#include "core/time_utils.h"
#include "hardware/common/sensor.h"
#include "hardware/video/video_sensor.h"
#include "hardware/video/sixcam_sensor.h"
#include "hardware/imu/imu_sensor.h"
#include "hardware/tracker/vive_tracker_sensor.h"
```

`hardware/common/sensor.cpp` 包含 `hardware/common/sensor.h` 和 `core/barrier.h`；`hardware/video/sixcam_sensor.h` 包含 `hardware/imu/imu_decode.h`；两个 C++ 单元测试分别包含 `core/output_path.h` 与 `core/time_utils.h`。不得留下 `../common`、`../../camera_config.h`、`hardware/IMU` 或 `hardware/VideoSensor`。

- [ ] **Step 5: 运行检查和残留扫描。**

Run: `sh tests/test_source_layout.sh`
Expected: exit 0。

Run: `! rg -n 'hardware/(IMU|VideoSensor)|"(barrier|camera_config|frame_queue|output_path|time_utils|vive_usb)\.h"' --glob '*.{c,cc,cpp,h,hpp}' .`
Expected: exit 0。

- [ ] **Step 6: 检查并提交。**

Run: `git diff --check && git status --short`
Expected: 只有本任务移动、include 和结构测试变更。

```bash
git add app core hardware tests deploy
git commit -m "refactor: organize unified capture source layout"
```

## Task 2: 让 Make、CMake 与无硬件测试适配新布局

**Files:**
- Modify: `Makefile`, `CMakeLists.txt`, `tests/test_output_path.cpp`, `tests/test_time_utils.cpp`
- Test: `tests/test_output_path.cpp`, `tests/test_time_utils.cpp`, `tests/test_source_layout.sh`

**Interfaces:**
- Consumes: Task 1 的 `app/main.cpp`、`core/` 和硬件路径。
- Produces: `make test`、`make test_output_path`、`make test_time_utils`、`make test_source_layout`；对象在 `build/obj/`，测试二进制在 `build/tests/`。

- [ ] **Step 1: 先证明 Makefile 仍使用旧测试路径。**

Run: `make -n test_output_path`
Expected: FAIL，提示 `test_output_path.cpp` 不存在，或 dry-run 仍引用旧根目录路径。

- [ ] **Step 2: 重写 Makefile 的源码、对象和依赖规则。**

定义下列规则，保留现有 Nori、MPP 与 libsurvive 的 include/library 覆盖变量：

```make
CPP_SOURCES := app/main.cpp hardware/common/sensor.cpp
C_SOURCES := hardware/as5600/as5600.c
CPP_OBJECTS := $(patsubst %.cpp,build/obj/%.o,$(CPP_SOURCES))
C_OBJECTS := $(patsubst %.c,build/obj/%.o,$(C_SOURCES))
OBJS := $(CPP_OBJECTS) $(C_OBJECTS)
DEPS := $(OBJS:.o=.d)
build/obj/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -MMD -MP -c -o $@ $<
build/obj/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -MMD -MP -c -o $@ $<
-include $(DEPS)
```

`INCLUDES` 必须含 `-I.`。两个既有 target 编译 `tests/test_*.cpp` 到 `build/tests/test_*` 后执行；新增 `test_source_layout: ; sh tests/test_source_layout.sh`，并使 `test` 依赖这三个 target。`clean` 只删除 `build` 和 `unified_capture`。

- [ ] **Step 3: 将 CMake 从 TSTC 更新到 Nori。**

以 `NORI_INC`、`NORI_LIB`、`MPP_INC`、`SURVIVE_DIR` cache 变量替代 `TSTC_SDK_DIR`。此阶段 `add_executable` 的源清单为 `app/main.cpp`、`hardware/common/sensor.cpp`、`hardware/as5600/as5600.c`；include 目录含项目根、Nori `Nori_Xvision_API`、MPP 和 Makefile 当前使用的 libsurvive include 目录。链接库为 `Nori_Xvision_Std rockchip_mpp turbojpeg gpiod survive pthread rt udev m`，并配置 Nori 与 libsurvive bin rpath。

- [ ] **Step 4: 运行纯逻辑和结构测试。**

Run: `make clean && make test`
Expected: 三项测试 exit 0，测试产物仅在 `build/tests/`。

Run: `cmake -S . -B build/cmake-configure`
Expected: configure 成功且不出现 `TSTC_SDK_DIR` 或 `USBCam_API`。

- [ ] **Step 5: 检查并提交。**

Run: `git diff --check && git status --short`
Expected: 只有构建配置和测试路径变更。

```bash
git add Makefile CMakeLists.txt tests
git commit -m "build: align Nori toolchains with source layout"
```

## Task 3: 提取硬件发现和 session 生命周期，消除视频全局变量

**Files:**
- Create: `hardware/video/capture_control.h`, `hardware/video/device_discovery.h`, `hardware/video/device_discovery.cpp`, `app/session_runner.h`, `app/session_runner.cpp`, `tests/test_video_capture_control.cpp`
- Modify: `app/main.cpp`, `hardware/video/video_sensor.h`, `hardware/video/sixcam_sensor.h`, `Makefile`, `CMakeLists.txt`
- Test: `tests/test_video_capture_control.cpp`

**Interfaces:**
- Consumes: `CameraConfig`、现有 Sensor 构造函数和 Task 2 的构建规则。
- Produces: `CameraDiscoveryResult discover_cameras()`，`void scan_devices()`，`VideoCaptureControl`，以及 `SessionRunner::run()`、`cameras_json()`、`request_preview()`。

- [ ] **Step 1: 编写 `VideoCaptureControl` 的失败测试。**

创建 `tests/test_video_capture_control.cpp`：

```cpp
#include "hardware/video/capture_control.h"
#include <cassert>
#include <string>

int main() {
    VideoCaptureControl control;
    control.reset_stream_start(2, true);
    assert(control.jhh2_remaining.load() == 3);
    assert(!control.jhh02_init_done.load());
    control.reset_stream_start(2, false);
    assert(control.jhh2_remaining.load() == 2);
    assert(control.jhh02_init_done.load());
    control.request_preview("/tmp/preview.jpg");
    std::string path;
    assert(control.take_preview(path));
    assert(path == "/tmp/preview.jpg");
    assert(!control.take_preview(path));
}
```

在 Makefile 中增加 `test_video_capture_control`，产物为 `build/tests/test_video_capture_control`。

- [ ] **Step 2: 运行测试，确认 header 尚不存在时失败。**

Run: `make test_video_capture_control`
Expected: FAIL，错误为找不到 `hardware/video/capture_control.h`。

- [ ] **Step 3: 实现显式视频控制状态，并传入两个视频 Sensor。**

创建下列接口：

```cpp
class VideoCaptureControl {
public:
    void reset_stream_start(int independent_jhh2_count, bool sixcam_jhh02_available);
    void request_preview(std::string path);
    bool take_preview(std::string& path);
    std::atomic<int> jhh2_remaining{0};
    std::atomic<bool> jhh02_init_done{false};
private:
    bool preview_pending_ = false;
    std::string preview_path_;
    std::mutex preview_mutex_;
};
```

`reset_stream_start` 设定独立 JHH2 数加 `sixcam_jhh02_available ? 1 : 0`，没有 SixCam JHH02 时立即令 `jhh02_init_done=true`。`request_preview` 在 mutex 内保存路径并设置 pending；`take_preview` 在同一 mutex 内检查/清除 pending、移动路径，无待处理请求时返回 `false`。

`VideoSensor` 和 `SixCamSensor` 的构造函数都增加 `VideoCaptureControl& control`，保存为引用。删除两个 header 中 `g_jhh2_remaining`、`g_jhh02_init_done` 和三个 preview `extern` 声明；所有现有访问替换为 `control_`。JPEG 写入点调用 `take_preview(path)`，仍只在下一张成功解码帧写文件。

- [ ] **Step 4: 提取 Nori 设备发现和 `SessionRunner`。**

在 `hardware/video/device_discovery.h` 定义：

```cpp
struct CameraSlot { CameraConfig config; bool enabled = false; };
struct SixCamDevices { bool enabled = false; uint32_t jhh04_id = 0; uint32_t jhh02_id = 0; };
struct CameraDiscoveryResult {
    std::array<CameraSlot, 2> jhh2;
    SixCamDevices sixcam;
    int active_count = 0;
};
void scan_devices();
CameraDiscoveryResult discover_cameras();
```

`discover_cameras()` 原样迁移当前 VID/PID、USB bus 匹配、日志和“不立即 `Nori_Xvision_UnInit()`”语义；失败以 `active_count==0` 表示。`scan_devices()` 保留 scan 完立即 uninit 的现有行为。

在 `app/session_runner.h` 定义：

```cpp
struct SessionOptions { bool use_imu; bool use_as5600; bool use_vive; bool use_h265; };
using ControlPump = std::function<void(int timeout_ms)>;
class SessionRunner {
public:
    SessionRunner(const CameraDiscoveryResult&, SessionOptions, std::atomic<bool>& session_running);
    std::string make_session_dir(const std::string& prefix, int session_number) const;
    void run(const std::string& session_dir, int session_number, const ControlPump& pump);
    std::string cameras_json() const;
    void request_preview(std::string path);
private:
    CameraDiscoveryResult cameras_;
    SessionOptions options_;
    std::atomic<bool>& session_running_;
    VideoCaptureControl capture_control_;
};
```

在 `app/session_runner.cpp` 定义全局 `timespec g_t0` 并迁移原 `make_session_dir()` 与 `run_session()`。`run()` 创建与当前相同的 Video/SixCam/IMU/Encoder/VIVE Sensor 组合，用 `std::vector<std::unique_ptr<Sensor>>` 管理，启动同一个 `SimpleBarrier`。每次 `wait_all_arrived(100)` 超时后调用 `pump(0)`；采集循环中调用 `pump(50)`。它不包含 socket、GPIO、`poll` 或 fd；`pump` 只负责把外部事件转换成 `session_running_=false`。`use_h265` 只修改 session 内 JHH2/JHH02 配置副本。将 `app/session_runner.cpp` 和 `hardware/video/device_discovery.cpp` 同时加入 Make 的 `CPP_SOURCES` 和 CMake 的 `add_executable` 源清单。

- [ ] **Step 5: 让当前 main 使用提取后的类型。**

从 `app/main.cpp` 删除 `CAMS`、`SixCamEntry`、`g_jhh2_remaining`、`g_jhh02_init_done`、三个 preview 全局、`scan_devices()`、`resolve_camera_devices()`、`make_session_dir()` 和 `run_session()`。保留临时的原始 socket/GPIO 循环，但在设备发现后构造一次 `SessionRunner`；socket handler 改接收 `SessionRunner&`，将 preview 请求转交 `request_preview()`；status 从 `cameras_json()` 取得相机 JSON。定义局部 `ControlPump pump_controls`，它复用原 `poll()` 的 socket/GPIO fd、接受客户端并调用该 handler、在 falling edge 或 stop 命令时将 session flag 设为 false。每次启动改为：

```cpp
std::string dir = sessions.make_session_dir(prefix, session_num);
sessions.run(dir, session_num, pump_controls);
```

该 pump 不创建线程。

- [ ] **Step 6: 运行测试和板端发现回归。**

Run: `make test_video_capture_control && make test`
Expected: 四项无硬件测试 exit 0。

Run on RK3588: `make clean && make && ./unified_capture --scan`
Expected: 编译成功，Nori 扫描日志与重构前相同，未启动 socket 线程。

- [ ] **Step 7: 检查并提交。**

Run: `git diff --check && ! rg -n '\bg_(jhh2_remaining|jhh02_init_done|preview_pending|preview_path|preview_mutex)\b' app hardware`
Expected: exit 0。

```bash
git add app hardware/video tests Makefile CMakeLists.txt
git commit -m "refactor: isolate camera discovery and session lifecycle"
```

## Task 4: 抽出 socket、GPIO 和 Runtime，收缩入口文件

**Files:**
- Create: `app/runtime.h`, `app/runtime.cpp`, `app/socket_server.h`, `app/socket_server.cpp`, `app/gpio_control.h`, `app/gpio_control.cpp`, `tests/test_socket_command.cpp`
- Modify: `app/main.cpp`, `app/session_runner.cpp`, `Makefile`, `CMakeLists.txt`, `tests/test_source_layout.sh`
- Test: `tests/test_socket_command.cpp`, `tests/test_socket.sh`

**Interfaces:**
- Consumes: Task 3 的 `SessionRunner`、`CameraDiscoveryResult` 和 `VideoCaptureControl`。
- Produces: `RuntimeOptions`、`Runtime::run()`、`SocketServer::serve_one()`、`GpioControl::consume_event()`；`app/main.cpp` 不再包含 socket/gpiod/Nori/Sensor header。

- [ ] **Step 1: 写 socket 命令解析的失败测试。**

创建 `tests/test_socket_command.cpp`：

```cpp
#include "app/socket_server.h"
#include <cassert>
int main() {
    assert(parse_socket_command("start\n").kind == SocketCommandKind::start);
    assert(parse_socket_command("stop").kind == SocketCommandKind::stop);
    SocketCommand preview = parse_socket_command("preview:/tmp/p.jpg\n");
    assert(preview.kind == SocketCommandKind::preview);
    assert(preview.preview_path == "/tmp/p.jpg");
    assert(parse_socket_command("status").kind == SocketCommandKind::status);
    assert(parse_socket_command("unknown").kind == SocketCommandKind::unknown);
}
```

增加 `make test_socket_command`；首次运行必须因缺少 `app/socket_server.h` 失败。

- [ ] **Step 2: 实现命令解析和 Unix socket 生命周期。**

在 `app/socket_server.h` 定义：

```cpp
enum class SocketCommandKind { start, stop, preview, status, unknown };
struct SocketCommand { SocketCommandKind kind; std::string preview_path; };
SocketCommand parse_socket_command(std::string_view request);
class SocketServer {
public:
    explicit SocketServer(std::string path = "/tmp/unified_capture.sock");
    ~SocketServer();
    bool open();
    int fd() const;
    void close();
    void serve_one(const std::function<std::string(const SocketCommand&)>& handler);
};
```

`parse_socket_command` 去掉末尾单个换行，仅接受 `start`、`stop`、`status` 和非空 `preview:<path>`。`open()` 保留 stale socket 探测、`bind`、`listen(4)`、非阻塞设置和日志。`serve_one()` accept 一个客户端，读取最多 255 字节，调用 handler，追加恰好一个换行后写回并关闭；读失败时直接关闭。

- [ ] **Step 3: 封装 GPIO 和 LED。**

在 `app/gpio_control.h` 定义：

```cpp
enum class ButtonEvent { none, falling_edge, error };
class GpioControl {
public:
    bool open();
    void close();
    int event_fd() const;
    ButtonEvent consume_event();
    void set_led(bool on) const;
    void disable_led_trigger() const;
};
```

`open()` 使用当前 `/dev/gpiochip2`、line 8 和 `gpiod_line_request_both_edges_events("capture-btn")`；失败时释放所有已取得资源并返回 `false`。`consume_event()` 只有读取到 falling edge 时返回 `falling_edge`。LED 方法保持 `/sys/class/leds/sys_led` 的写入和静默失败策略。

- [ ] **Step 4: 将顶层循环移入 Runtime。**

在 `app/runtime.h` 定义 `RuntimeOptions` 字段 `scan_only`、`output_prefix`、`use_gpio`、`socket_mode`、`single_shot`、`use_imu`、`use_as5600`、`use_h265`，以及：

```cpp
class Runtime {
public:
    explicit Runtime(RuntimeOptions options);
    int run();
    std::atomic<bool>& keep_running();
    std::atomic<bool>& session_running();
};
```

`Runtime::run()` 先在 `scan_only` 时调用 `scan_devices()` 并直接返回，保留 `--scan` 不要求 SD 卡的现有语义。其他模式的固定顺序是：SD 路径校验、目录创建、`discover_cameras()`、`detect_vive_trackers()`、`SocketServer::open()`、GPIO 初始化、按模式运行、关闭 socket、`Nori_Xvision_UnInit()`。它实现三种已有模式：socket 模式等待 start 并在 `--single` 后退出；`--no-gpio` 直接运行 session 1；GPIO 模式在 falling edge 或 socket start 后运行下一 session，GPIO 初始化失败时仍直接运行 session 1。

`Runtime` 构造 `SessionRunner` 并传入一个 lambda。lambda 使用一次 `poll()` 监听 `GpioControl::event_fd()` 和 `SocketServer::fd()`，处理 socket 命令后停止 session 或请求预览；即使 `poll()` 超时，也检查 start 请求。socket 回包必须保持原 JSON 字段和错误文本：`not ready`、`already running`、`not running`、`unknown command`，以及相同的 `ready`、`running`、`session`、`elapsed_ms`、`cameras`、`imu`、`as5600`、`vive` 字段。`stop` 设置 `session_running()` 为 false；preview 调用 `SessionRunner::request_preview()`。

将 `app/runtime.cpp`、`app/socket_server.cpp`、`app/gpio_control.cpp` 追加到 Make 的 `CPP_SOURCES` 和 CMake 的 `add_executable` 源清单；不使用 glob，因此新源码缺失会使两个构建入口明确失败。

`app/main.cpp` 只保留 CLI 解析、line buffering、SIGINT/SIGTERM/SIGPIPE/SIGSEGV/SIGABRT 安装和：

```cpp
Runtime runtime(options);
return runtime.run();
```

信号处理器只将 Runtime 暴露的 keep-running/session-running atomic 设为停止状态；不得调用 socket、GPIO、Nori 或 Sensor API。

- [ ] **Step 5: 运行单元、结构和板端协议测试。**

Run: `make test_socket_command && make test`
Expected: 全部无硬件测试 exit 0。

Run: `sh tests/test_source_layout.sh && ! rg -n 'Nori_Xvision|gpiod_|socket\(|accept\(|poll\(|VideoSensor|SixCamSensor|ImuSensor' app/main.cpp`
Expected: exit 0。

Run on RK3588: `./tests/test_socket.sh`（以 `unified_capture --socket` 运行）
Expected: status/start/stop/重复 start-stop/unknown command 全部通过。

- [ ] **Step 6: 检查并提交。**

Run: `git diff --check && git status --short`
Expected: 只有 app 控制模块、构建目标和测试变更。

```bash
git add app tests Makefile CMakeLists.txt
git commit -m "refactor: split runtime control from main"
```

## Task 5: 更新部署、用户文档和仍有效的源码链接

**Files:**
- Modify: `README.md`, `CLAUDE.md`, `tests/README.md`, `deploy/unified_capture.service`, `docs/socket-control.md`, `docs/2026-07-27-hardware-migration-sampling-validation.md`, `docs/decisions/2026-07-23-use-mkv-for-capture-video.md`
- Test: `tests/test_source_layout.sh`

**Interfaces:**
- Consumes: Tasks 1–4 的最终路径、Make target 和 CLI/socket 行为。
- Produces: 与源码一致的构建、测试、部署、输出格式和源码导航文档。

- [ ] **Step 1: 更新 README 到实际运行接口。**

依赖和交叉编译变量统一为 `NORI_INC`、`NORI_LIB`；保留当前 `.y8` 摄像头灰度输出与 IMU/编码器/Tracker JSONL 描述；从参数表删除 `--no-vive`；文件说明替换为 `app/`、`core/`、`hardware/`、`tests/`、`deploy/` 职责表；服务复制命令改为 `cp deploy/unified_capture.service /etc/systemd/system/`。将 unit 的 `ExecStart` 设为 `/usr/local/bin/unified_capture --socket --single /media/usb0/capture`，去除无效的 `--no-vive` 并使用强制要求的 SD 卡采集根目录。socket 示例不变。用户于 2026-07-28 批准该部署行为修正。

- [ ] **Step 2: 更新项目指导和测试说明。**

`CLAUDE.md` 的“两个编译单元”改为 `app/*.cpp`、`hardware/common/sensor.cpp`、`hardware/video/device_discovery.cpp`、`hardware/as5600/as5600.c`。`tests/README.md` 声明所有单元测试都在 `tests/`、产物在 `build/tests/`，列出 `test_video_capture_control`、`test_socket_command`、`test_source_layout`，并以 `make test` 作为全量无硬件回归。

- [ ] **Step 3: 只修复仍作为导航使用的文档链接。**

`docs/socket-control.md` 源码链接改为 `../app/runtime.cpp`、`../app/socket_server.cpp`、`../deploy/unified_capture.service`。将 `docs/2026-07-27-hardware-migration-sampling-validation.md` 与 `docs/decisions/2026-07-23-use-mkv-for-capture-video.md` 的现行源码链接改为 `hardware/video/video_sensor.h` 与 `hardware/video/sixcam_sensor.h`。不得改写 `docs/bugs`、`docs/plans`、`docs/records` 对历史实现的正文。

- [ ] **Step 4: 验证文档和布局引用。**

Run: `sh tests/test_source_layout.sh`
Expected: exit 0。

Run: `! rg -n 'hardware/(IMU|VideoSensor)|\]\(\.\./main\.cpp\)|\]\(\.\./unified_capture\.service\)' README.md CLAUDE.md tests/README.md docs/socket-control.md docs/2026-07-27-hardware-migration-sampling-validation.md docs/decisions/2026-07-23-use-mkv-for-capture-video.md`
Expected: exit 0。

`tests/test_source_layout.sh` 有意包含旧硬件目录字面量，用来断言它们未被追踪；用户于 2026-07-28 批准将该脚本排除在文档残留扫描之外。

- [ ] **Step 5: 检查并提交。**

Run: `git diff --check && git status --short`
Expected: 只有部署和当前文档引用更新。

```bash
git add README.md CLAUDE.md tests/README.md docs deploy
git commit -m "docs: document unified capture source layout"
```

## Task 6: 完整验证和交付检查

**Files:**
- Modify: none expected
- Test: 全部 Make、CMake、目录、静态源码边界和 RK3588 板端回归

**Interfaces:**
- Consumes: Tasks 1–5 的完成状态。
- Produces: 可复现的验证记录；不引入额外功能或重构。

- [ ] **Step 1: 在干净构建产物状态运行无硬件验证。**

Run: `make clean && make test && sh tests/test_source_layout.sh && git diff --check`
Expected: 全部 exit 0；根目录不含项目 C/C++ 源、私有 header 或 service 文件。

- [ ] **Step 2: 验证构建清单与架构边界。**

Run: `make -n | rg 'app/main\.cpp|app/runtime\.cpp|app/session_runner\.cpp|app/socket_server\.cpp|app/gpio_control\.cpp|hardware/video/device_discovery\.cpp|hardware/as5600/as5600\.c'`
Expected: 所有生产源都出现。

Run: `cmake -S . -B build/cmake-configure && ! rg -n 'TSTC|USBCam_API' Makefile CMakeLists.txt README.md`
Expected: configure 成功，构建入口和 README 没有旧 SDK 名称。

Run: `find . -maxdepth 1 -type f \( -name '*.cpp' -o -name '*.c' -o -name '*.h' -o -name '*.service' \)`
Expected: 无输出。

- [ ] **Step 3: 在 RK3588 运行编译与行为回归。**

Run on RK3588:

```bash
make clean
make
./unified_capture --scan
./unified_capture --socket /media/usb0/capture
./tests/test_socket.sh
```

Expected: `make` 链接 Nori/MPP/libsurvive；`--scan` 列出设备；socket 脚本通过。单独 GPIO 验收时，按键完成一轮采集，session 目录和 `.y8`/JSONL 输出与重构前一致。

- [ ] **Step 4: 检查最终工作树和提交序列。**

Run: `git status --short && git log --oneline -6`
Expected: 工作树干净；可见布局、构建、session、runtime、文档五个单一目的提交，以及之前的设计/计划提交。
