# Mango / Mango Pro 双 Profile 拆分 Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** 把 `mango` profile 从「六目 + 双腕」拆成两个平级 profile —— `mango`（双目档：独立 JHH2 双目 + 双腕）与 `mango_pro`（六目档：六目 + 双腕），并让双目档真正能采集单块 JHH2。

**Architecture:** 复用 `banana` 现有 JHH2 独立双目的发现与 VideoSensor/ImuSensor 采集逻辑，不做重写。`mango_pro` 承接原 `mango` 的六目逻辑（原样迁移）。`banana` / `cherry` 不动。

**Tech Stack:** C++20, V4L2/UVC, Rockchip MPP, libturbojpeg, FFmpeg；host 测试用 `assert` 单文件二进制。

**设计依据:** `docs/design/mango-mango-pro-profile-split.md`

---

## 关键约定（实现者必读）

- **破坏性变更**：`ProductProfile::mango` 语义从「六目 + 双腕」改为「双目 + 双腕」。原六目逻辑全部落到新的 `ProductProfile::mango_pro`。
- **复用**：JHH2 独立双目发现 = `discover_banana_cameras()` 中「选不与 JHH04 同 bus 的 `1bcf:2d50`」那段；JHH2 采集 = `session_runner.cpp` banana 分支的 VideoSensor + ImuSensor 创建模式。
- **输出**：双目档头部目录 `head/`，H.265 MKV + IMU JSONL，**无 Y8**。
- **测试模式**：本项目 host 测试是「`assert` + `main()` 单文件」，每 test 一个 Makefile target，无 gtest。纯逻辑层走 TDD，硬件层（discovery / session_runner）靠编译 + 板端验收。

---

## Task 1: 扩展 ProductProfile 枚举与解析

**Files:**
- Modify: `core/product_config.h:5`（enum 声明处）
- Modify: `core/product_config.cpp`（`product_profile_name`、`parse_product_profile`、`load_product_configuration_for_profile`、`load_product_profile`）
- Test: `tests/test_product_config.cpp`

**Step 1: 写失败测试**

在 `tests/test_product_config.cpp` 的 `main()` 末尾（`std::filesystem::remove_all(directory);` 之前）追加：

```cpp
// mango_pro：parse / name / 六目配置加载
assert(parse_product_profile("mango_pro") == ProductProfile::mango_pro);
assert(product_profile_name(ProductProfile::mango_pro) == "mango_pro");
{
    std::ofstream output(product_config);
    output << "product=mango_pro\n";
}
{
    std::ofstream output(camera_map);
    output << "[mango_pro]\n"
           << "allow_missing_devices=true\n"
           << "wrist_left.product=SL\n"
           << "wrist_right.product=JHHSW\n"
           << "sixcam.enabled=true\n";
}
const ProductConfigResult mango_pro =
    load_product_configuration(product_config.string(), camera_map.string());
assert(mango_pro.configuration.has_value());
assert(mango_pro.configuration->profile == ProductProfile::mango_pro);
assert(mango_pro.configuration->sixcam_enabled);
assert(mango_pro.configuration->wrist.left_product == "SL");
assert(write_product_profile(product_config.string(), ProductProfile::mango_pro));

// mango 双目档：不再要求 sixcam.enabled，也不应把 sixcam_enabled 置真
{
    std::ofstream output(product_config);
    output << "product=mango\n";
}
{
    std::ofstream output(camera_map);
    output << "[mango]\n"
           << "allow_missing_devices=true\n"
           << "wrist_left.product=SL\n"
           << "wrist_right.product=JHHSW\n";
}
const ProductConfigResult mango_dual =
    load_product_configuration(product_config.string(), camera_map.string());
assert(mango_dual.configuration.has_value());
assert(mango_dual.configuration->profile == ProductProfile::mango);
assert(!mango_dual.configuration->sixcam_enabled);
```

**Step 2: 运行确认失败**

Run: `make test_product_config`
Expected: 编译错误（`ProductProfile::mango_pro` 未定义）。

**Step 3: 实现**

- `core/product_config.h`：`enum class ProductProfile { mango, mango_pro, banana, cherry };`
- `core/product_config.cpp` `load_product_profile`：`else if (value == "mango_pro") { *profile = ProductProfile::mango_pro; }`
- `parse_product_profile`：加 `if (value == "mango_pro") return ProductProfile::mango_pro;`
- `product_profile_name`：`case ProductProfile::mango_pro: return "mango_pro";`
- `load_product_configuration_for_profile`：把现有 `if (profile == ProductProfile::mango)` 分支改为 `if (profile == ProductProfile::mango || profile == ProductProfile::mango_pro)`，其内部：
  - 读 `map.find("mango")` 改为按 profile 名读：`map.find(std::string(product_profile_name(profile)))`（mango → `[mango]`，mango_pro → `[mango_pro]`）。
  - `sixcam.enabled` 只在 `profile == ProductProfile::mango_pro` 时解析（mango 双目档忽略该键）。

**Step 4: 运行确认通过**

Run: `make test_product_config`
Expected: 全部 assert 通过，无输出即成功（assert 静默）。

**Step 5: Commit**

```bash
git add core/product_config.h core/product_config.cpp tests/test_product_config.cpp
git commit -m "feat: 新增 mango_pro profile 枚举与解析，mango 语义改为双目档"
```

---

## Task 2: 输出策略区分 head / jhh02 / jhh04

**Files:**
- Modify: `app/capture_output_policy.h:38`（`mango_camera_output_policy`）
- Test: `tests/test_capture_output_policy.cpp`

**Step 1: 写失败测试**

在 `tests/test_capture_output_policy.cpp` 追加：

```cpp
// mango 双目档：head 输出 H.265，无 Y8
const CameraOutputPolicy head = mango_camera_output_policy("head");
assert(head.output_h265);
assert(!head.output_y8);

// mango_pro 六目档：复用同名策略，jhh02 无 Y8、jhh04 有 Y8
const CameraOutputPolicy pro_jhh02 = mango_pro_camera_output_policy("jhh02");
assert(pro_jhh02.output_h265);
assert(!pro_jhh02.output_y8);
const CameraOutputPolicy pro_jhh04 = mango_pro_camera_output_policy("jhh04");
assert(pro_jhh04.output_h265);
assert(pro_jhh04.output_y8);
const CameraOutputPolicy pro_wrist = mango_pro_camera_output_policy("wrist_left");
assert(pro_wrist.output_h265);
assert(!pro_wrist.output_y8);
```

**Step 2: 运行确认失败**

Run: `make test_capture_output_policy`
Expected: 编译错误（`mango_pro_camera_output_policy` 未定义）。

**Step 3: 实现**

`app/capture_output_policy.h` 替换 `mango_camera_output_policy` 并新增 `mango_pro_camera_output_policy`：

```cpp
inline CameraOutputPolicy mango_camera_output_policy(std::string_view name) {
    // mango 双目档：head + 双腕，均 H.265，无 Y8
    if (name == "head" || name == "wrist_left" || name == "wrist_right") {
        return {true, false};
    }
    return {false, false};
}

inline CameraOutputPolicy mango_pro_camera_output_policy(std::string_view name) {
    if (name == "wrist_left" || name == "wrist_right" || name == "jhh02") {
        return {true, false};
    }
    if (name == "jhh04") {
        return {true, true};
    }
    return {false, false};
}
```

**Step 4: 运行确认通过**

Run: `make test_capture_output_policy`
Expected: 通过。

**Step 5: Commit**

```bash
git add app/capture_output_policy.h tests/test_capture_output_policy.cpp
git commit -m "feat: 输出策略拆分 mango 双目档(head)与 mango_pro 六目档(jhh02/jhh04)"
```

---

## Task 3: session_profile 按 profile 分流

**Files:**
- Modify: `app/session_profile.cpp`
- Test: `tests/test_session_profile.cpp`

**Step 1: 写失败测试**

在 `tests/test_session_profile.cpp` 追加：

```cpp
// mango 双目档：active 只含腕部（head 由 session_runner 单独建），json 报 head
CameraDiscoveryResult mango_dual;
mango_dual.profile = ProductProfile::mango;
mango_dual.head = enabled_slot("head");
mango_dual.head.enabled = true;
mango_dual.wrist[0] = enabled_slot("wrist_left");
assert(active_profile_cameras(mango_dual).size() == 1);   // 仅腕部
assert(active_profile_cameras(mango_dual)[0].config.name ==
       std::string("wrist_left"));
assert(profile_cameras_json(mango_dual) ==
       "\"cameras\":{\"head\":true,\"wrist_left\":true,\"wrist_right\":false}");
assert(profile_session_directories(mango_dual) ==
       std::vector<std::string>({"wrist_left", "head"}));

// mango_pro 六目档：json 报 wrist + jhh04/jhh02，目录含 jhh04/jhh02
CameraDiscoveryResult mango_pro;
mango_pro.profile = ProductProfile::mango_pro;
mango_pro.wrist[0] = enabled_slot("wrist_left");
mango_pro.sixcam.enabled = true;
mango_pro.sixcam.jhh04_path = "/dev/video6";
mango_pro.sixcam.jhh02_path = "/dev/video4";
assert(profile_cameras_json(mango_pro) ==
       "\"cameras\":{\"wrist_left\":true,\"wrist_right\":false,\"jhh04\":true,\"jhh02\":true}");
assert(profile_session_directories(mango_pro) ==
       std::vector<std::string>({"wrist_left", "jhh04", "jhh02"}));
```

**Step 2: 运行确认失败**

Run: `make test_session_profile`
Expected: 断言失败 / 编译失败（`head` 字段、mango_pro 分支未实现）。

**Step 3: 实现**

`app/session_profile.cpp`：

- `active_profile_cameras`：mango 与 mango_pro 都返回腕部（`cameras.wrist`），不返回 head（head 由 session_runner 单独建）。即把现有 `cameras.profile == ProductProfile::mango ? cameras.wrist : cameras.jhh2` 扩展为 `cameras.profile == ProductProfile::mango || cameras.profile == ProductProfile::mango_pro`。
- `profile_cameras_json`：新增 mango 分支，报 `head` + `wrist_left` + `wrist_right`；mango_pro 分支复用现有 mango 逻辑（`wrist_left`/`wrist_right` + `jhh04`/`jhh02`）。
- `profile_session_directories`：mango 返回腕部目录 + `"head"`；mango_pro 返回腕部 + `"jhh04"` + `"jhh02"`。

**Step 4: 运行确认通过**

Run: `make test_session_profile`
Expected: 通过。

**Step 5: Commit**

```bash
git add app/session_profile.cpp tests/test_session_profile.cpp
git commit -m "feat: session_profile 区分 mango 双目档(head)与 mango_pro 六目档目录"
```

---

## Task 4: capture_sensor_status 处理 mango_pro

**Files:**
- Modify: `app/status_response.cpp:27`（`capture_sensor_status`）
- Test: `tests/test_status_response.cpp`

**Step 1: 写失败测试**

在 `tests/test_status_response.cpp` 追加：

```cpp
const CaptureSensorStatus mango_pro_sensors = capture_sensor_status(
    ProductProfile::mango_pro, true, true, true);
assert(mango_pro_sensors.imu);
assert(!mango_pro_sensors.as5600);
assert(!mango_pro_sensors.vive);
```

**Step 2: 运行确认失败**

Run: `make test_status_response`
Expected: 断言失败（当前 mango_pro 落进 else 分支返回 as5600/vive 为 true）。

**Step 3: 实现**

`app/status_response.cpp` 的 `capture_sensor_status`：把 `if (profile == ProductProfile::mango)` 改为 `if (profile == ProductProfile::mango || profile == ProductProfile::mango_pro)`。

**Step 4: 运行确认通过**

Run: `make test_status_response`
Expected: 通过。

**Step 5: Commit**

```bash
git add app/status_response.cpp tests/test_status_response.cpp
git commit -m "feat: mango_pro 禁用 AS5600/VIVE，与 mango 双目档一致"
```

---

## Task 5: 设备发现 —— mango 双目档 + mango_pro 六目档

**Files:**
- Modify: `hardware/video/device_discovery.h`
- Modify: `hardware/video/device_discovery.cpp`

> 本层依赖 `/sys` V4L2 枚举，无 host 单测。验证方式：编译通过 + 板端 `--scan`。

**Step 1: 结构体加 head 槽**

`hardware/video/device_discovery.h` 的 `CameraDiscoveryResult` 加一个 `CameraSlot head;`（放在 `wrist` 附近）。`CameraSlot` 已含 `config / enabled / device_path`。

**Step 2: 新增 discover_mango_cameras（双目档）**

`hardware/video/device_discovery.cpp` 新增（复用 banana 的 JHH2 分配思路，但只取 1 块且不与 JHH04 同 bus）：

```cpp
CameraDiscoveryResult discover_mango_cameras(
    const ProductConfiguration& configuration) {
    CameraDiscoveryResult result;
    result.profile = ProductProfile::mango;

    std::vector<DiscoveredDevice> devices = scan_v4l2_devices();
    printf("V4L2: found %zu device(s)\n", devices.size());
    if (devices.empty()) {
        result.degraded = configuration.wrist.allow_missing_devices;
        return result;
    }

    // ── 头部独立 JHH2 双目：取一块不与 JHH04(2d51) 同 bus 的 JHH2(2d50) ──
    uint32_t sixcam_bus = 0;
    for (const auto& d : devices) {
        if (d.vid == kSixVid && d.pid == kSixPid) { sixcam_bus = d.bus; break; }
    }
    for (const auto& d : devices) {
        if (d.vid == kJhh2Vid && d.pid == kJhh2Pid && d.bus != sixcam_bus) {
            result.head.config = {"head", kJhh2Vid, kJhh2Pid, 0, 3840, 1200, 30,
                                  16000000, 30, true,
                                  ImuOrientation::HORIZONTAL_TOP, true, false};
            result.head.enabled = true;
            result.head.device_path = d.path;
            result.active_count++;
            printf("  %-12s -> %s bus=%u  3840x1200@30 (dual-eye head)\n",
                   "head", d.path.c_str(), d.bus);
            break;
        }
    }
    if (!result.head.enabled) {
        if (configuration.wrist.allow_missing_devices) {
            result.degraded = true;
        }
        result.camera_errors.emplace_back("head jhh2 not found");
    }

    // ── 腕部：复用现有 wrist 匹配 ──
    std::vector<WristDeviceInfo> inventory;
    inventory.reserve(devices.size());
    for (const auto& d : devices) {
        WristDeviceInfo device;
        device.device_path = d.path;
        device.vid = d.vid;
        device.pid = d.pid;
        device.product = d.product;
        enumerate_mjpeg_formats(d.path, device.formats);
        inventory.push_back(std::move(device));
    }
    WristDiscoveryResult wrist =
        match_wrist_cameras(configuration.wrist, inventory);
    for (std::size_t i = 0; i < result.wrist.size(); ++i) {
        result.wrist[i].config = wrist.cameras[i].config;
        result.wrist[i].enabled = wrist.cameras[i].available;
        result.wrist[i].device_path = wrist.cameras[i].device_path;
    }
    result.degraded = result.degraded || wrist.degraded;
    result.camera_errors.insert(result.camera_errors.end(),
                                wrist.errors.begin(), wrist.errors.end());
    result.active_count += wrist.active_count;
    return result;
}
```

**Step 3: 原 discover_mango_cameras 改名为 discover_mango_pro_cameras**

把现有函数（六目 + 腕部逻辑，`device_discovery.cpp:292` 起）改名为 `discover_mango_pro_cameras`，`result.profile = ProductProfile::mango_pro`，其余逻辑不变。

**Step 4: 派发**

`discover_cameras`（`device_discovery.cpp:433`）改为：

```cpp
CameraDiscoveryResult discover_cameras(const ProductConfiguration& configuration) {
    if (configuration.profile == ProductProfile::cherry) {
        return discover_cherry_cameras(configuration);
    }
    if (configuration.profile == ProductProfile::mango) {
        return discover_mango_cameras(configuration);
    }
    if (configuration.profile == ProductProfile::mango_pro) {
        return discover_mango_pro_cameras(configuration);
    }
    return discover_banana_cameras();
}
```

**Step 5: 编译验证**

Run: `make -j` （需在 RK3588 板端或具备 MPP 交叉编译环境；host 上可用 `make CXX=g++ ...` 只验证 `device_discovery.cpp` 语法需连带依赖，若 host 缺 MPP 头则跳过编译、以板端为准）
Expected: 生产构建通过，无 `discover_mango_cameras` 未定义/重复定义错误。

**Step 6: Commit**

```bash
git add hardware/video/device_discovery.h hardware/video/device_discovery.cpp
git commit -m "feat: 设备发现拆分 mango 双目档(head)与 mango_pro 六目档"
```

---

## Task 6: SessionRunner 双目档采集分支

**Files:**
- Modify: `app/session_runner.cpp:97-147`（mango 分支）

**Step 1: 改造 mango 分支**

把 `session_runner.cpp` 中 `else if (cameras_.profile == ProductProfile::mango)` 的整个分支（含六目 SixCamSensor 创建 + 腕部循环）替换为双目档逻辑：

```cpp
} else if (cameras_.profile == ProductProfile::mango) {
    // ── 双目档：head 先启流，双腕随后 ──
    const bool has_head = cameras_.head.enabled &&
                          !cameras_.head.device_path.empty();
    capture_control_.reset_stream_start(
        static_cast<int>(active_profile_cameras(cameras_).size()), has_head);

    if (has_head) {
        CameraConfig config = cameras_.head.config;
        const CameraOutputPolicy policy =
            mango_camera_output_policy(config.name);
        config.output_h265 = policy.output_h265;
        config.output_y8 = policy.output_y8;
        auto video = std::make_unique<VideoSensor>(
            config, session_dir, cameras_.head.device_path,
            session_number, session_timestamp, session_running_,
            capture_control_);
        VideoSensor* video_ptr = video.get();
        sensors_.push_back(std::move(video));
        if (options_.use_imu && config.has_imu) {
            sensors_.push_back(std::make_unique<ImuSensor>(
                config.name, session_dir, video_ptr->imu_queue(),
                session_number, session_timestamp, config.imu_orientation,
                session_running_));
        }
    }

    for (const CameraSlot& camera : active_profile_cameras(cameras_)) {
        CameraConfig config = camera.config;
        const CameraOutputPolicy policy =
            mango_camera_output_policy(config.name);
        config.output_h265 = policy.output_h265;
        config.output_y8 = policy.output_y8;
        auto video = std::make_unique<VideoSensor>(
            config, session_dir, camera.device_path,
            session_number, session_timestamp, session_running_,
            capture_control_);
        VideoSensor* video_ptr = video.get();
        sensors_.push_back(std::move(video));
        if (options_.use_imu && config.has_imu) {
            sensors_.push_back(std::make_unique<ImuSensor>(
                config.name, session_dir, video_ptr->imu_queue(),
                session_number, session_timestamp, config.imu_orientation,
                session_running_));
        }
    }
}
```

> 注意：原 mango 分支的 SixCamSensor 创建逻辑（六目）要**保留**，但归属改为新的 `mango_pro` 分支。

**Step 2: 新增 mango_pro 分支**

在 `session_runner.cpp` 中，将原 mango 分支的六目逻辑整体放到新分支 `else if (cameras_.profile == ProductProfile::mango_pro)`，内部：
- `SixCamSensor` 创建部分原样保留。
- 腕部循环里的 `mango_camera_output_policy(config.name)` 改为 `mango_pro_camera_output_policy(config.name)`。

**Step 3: 编译验证**

Run: `make -j`
Expected: 生产构建通过。重点确认：`VideoSensor`/`ImuSensor` 构造签名与 banana 分支一致（`imu_queue()` 返回类型正确）。

**Step 4: Commit**

```bash
git add app/session_runner.cpp
git commit -m "feat: SessionRunner 双目档 head 采集与 mango_pro 六目分支拆分"
```

---

## Task 7: runtime 状态上报与严格模式期望数

**Files:**
- Modify: `app/runtime.cpp`

**Step 1: `status_cameras` 区分 profile**

`app/runtime.cpp:24` 的 `status_cameras`：mango 分支报 `head` + `wrist_left` + `wrist_right`；mango_pro 分支报 `wrist_left`/`wrist_right` + `jhh04`/`jhh02`。

```cpp
if (cameras.profile == ProductProfile::mango) {
    result.emplace_back("head", cameras.head.enabled);
    result.emplace_back("wrist_left", cameras.wrist[0].enabled);
    result.emplace_back("wrist_right", cameras.wrist[1].enabled);
    return result;
}
if (cameras.profile == ProductProfile::mango_pro) {
    result.emplace_back("wrist_left", cameras.wrist[0].enabled);
    result.emplace_back("wrist_right", cameras.wrist[1].enabled);
    if (cameras.sixcam.enabled) {
        result.emplace_back("jhh04", !cameras.sixcam.jhh04_path.empty());
        result.emplace_back("jhh02", !cameras.sixcam.jhh02_path.empty());
    }
    return result;
}
```

**Step 2: `build_profile_state` 严格模式**

`app/runtime.cpp:102` 的 `const bool is_mango = ...` 需同时覆盖 `mango` 与 `mango_pro`（两者都禁 VIVE、都走腕部 IMU 推导）。严格模式期望数：

- mango：`expected = 腕部启用数 + 1`（head）；错误文案 `"mango requires all devices (wrist x2 + head)"`。
- mango_pro：`expected = 腕部启用数 + 2`（六目）；文案沿用 `"mango_pro requires all devices (wrist x2 + sixcam)"`。

> 当前代码 `if (configuration.sixcam_enabled) expected += 2;` 仅对 mango_pro 生效；mango 双目档改为 `expected += 1`（head）。

**Step 3: 编译验证**

Run: `make -j`
Expected: 通过。

**Step 4: Commit**

```bash
git add app/runtime.cpp
git commit -m "feat: runtime status 与严格模式期望数区分 mango/mango_pro"
```

---

## Task 8: preview 通道加 head

**Files:**
- Modify: `app/socket_server.cpp:15`（`is_preview_channel`）
- Test: `tests/test_socket_command.cpp`

**Step 1: 写失败测试**

在 `tests/test_socket_command.cpp` 追加（若已有 preview 解析用例，参照补 head 断言）：

```cpp
// preview:head:<path> 解析出 channel=head
```

**Step 2: 实现**

`app/socket_server.cpp` 的 `is_preview_channel` 加 `channel == "head"`。

**Step 3: 验证**

Run: `make test_socket_command`
Expected: 通过。

**Step 4: Commit**

```bash
git add app/socket_server.cpp tests/test_socket_command.cpp
git commit -m "feat: preview 通道新增 head"
```

---

## Task 9: 示例配置与文档同步

**Files:**
- Modify: `deploy/camera-map.conf.example`
- Modify: `deploy/product.conf.example`（确认示例仍为 `product=mango`，含义已变，注释说明）
- Modify: `docs/unified_capture-overview.md`、`README.md`、`CLAUDE.md`（profile 表与命名说明）

**Step 1: camera-map.conf.example**

```ini
# 双目档（mango / mango plus）：head 硬编码为独立 JHH2 双目
[mango]
allow_missing_devices=true
wrist_left.product=SL
wrist_right.product=JHHSW

# 六目档（mango pro / mango pro plus）
[mango_pro]
allow_missing_devices=true
wrist_left.product=SL
wrist_right.product=JHHSW
sixcam.enabled=true

[cherry]
...（原样）
```

**Step 2: 文档**

- `CLAUDE.md` 项目概述 + 架构里「mango 采集六目…」改为「mango 采集双目(独立 JHH2) + 双腕；mango_pro 采集六目 + 双腕」。
- `README.md` / `docs/unified_capture-overview.md` 的 profile 表同步（4 个 profile）。

**Step 3: 验证**

Run: `make test_product_config`（确认 example 配置仍能解析）
Expected: 通过。

**Step 4: Commit**

```bash
git add deploy/camera-map.conf.example deploy/product.conf.example docs/ CLAUDE.md README.md
git commit -m "docs: 同步 mango/mango_pro 双 profile 示例配置与文档"
```

---

## Task 10: 板端验收（手动门禁，不可 host 单测）

> 前提：RK3588 板端（`rk.local` / `192.168.100.200`），具备 MPP + 全部依赖。

**Step 1: 双目档 60 秒验收**

```bash
# 板端接 1 块独立 JHH2(1bcf:2d50) + 左腕 SL + 右腕 JHHSW
make
echo 'product=mango' > /etc/unified_capture/product.conf
./unified_capture --no-gpio validation_dual_$(date +%Y%m%d)
```

Expected:
- `--scan` 打印 `head -> /dev/videoX bus=Y 3840x1200@30` + 双腕。
- `session_001/head/head-*.mkv`（HEVC 3840×1200）、`head-*.jsonl`、`wrist_left/`、`wrist_right/`。
- 60 秒日志 `queue_overflows=0 decode_failures=0 encoder_failures=0`，三路 ~30fps。
- `ffprobe` 确认 HEVC 30fps。

**Step 2: 六目档回归**

```bash
echo 'product=mango_pro' > /etc/unified_capture/product.conf
./unified_capture --no-gpio validation_pro_$(date +%Y%m%d)
```

Expected: 对照 `docs/records/v4l2-30fps-jhh02-h265-status.md`，`jhh02`/`jhh04`/双腕 四路 30fps、0 丢帧。

**Step 3: set_product 热切换**

```bash
printf 'set_product:mango_pro\n' | nc -U /tmp/unified_capture.sock
printf 'status\n' | nc -U /tmp/unified_capture.sock
```

Expected: `status.product` 正确切到 `mango_pro`，`cameras` 字段随之变化。

---

## 完成标准

- `make test` 全绿（host）。
- 板端双目档 60 秒验收通过（Task 10 Step 1）。
- 板端六目档回归通过（Task 10 Step 2）。
- `set_product` 在 `mango` / `mango_pro` 间热切换正常。

## 回滚点

- 每个 Task 独立 commit；若双目档板端验收失败，回滚 Task 6（session_runner）与 Task 5（discovery）即可恢复原六目 `mango` 行为，不影响 `banana`/`cherry`。
