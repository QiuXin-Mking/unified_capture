# 设计 — 前端产品选择同步后端并持久化

## 架构总览

```
device-ui 前端 (React)
   selectProduct('Mango')
     ├─ localStorage 缓存 (product.ts 不变)
     └─ api.setProduct('Mango')  ──POST /api/product──▶  server.cjs
                                                            │ 映射: Mango→banana
                                                            ▼
                                              captureCtl('set_product:banana')
                                                            │ AF_UNIX 短连接
                                                            ▼
                              unified_capture 守护进程 (Runtime 单线程 poll)
                                 空闲态: 校验→重读配置→重枚举→原子替换→写 product.conf
```

映射唯一权威在 `server.cjs`；守护进程只理解 `mango`/`banana`/`cherry`，不感知前端命名。

## 数据流契约

### 1. 前端 → server.cjs

`POST /api/product`，body `{"product":"Mango"|"Banana"}`。

响应：`{"ok":true,"product":"Mango"}`（成功，回传前端名）；`{"ok":false,"error":"..."}`（失败）。

### 2. server.cjs → 守护进程（新增 socket 命令）

`set_product:<profile>\n`，profile ∈ `mango|banana|cherry`。

| 条件 | 响应 |
|---|---|
| 成功 | `{"ok":true,"product":"banana"}` |
| 采集中 | `{"ok":false,"error":"busy"}` |
| 未知 profile | `{"ok":false,"error":"unknown product"}` |
| 目标无相机/配置错 | `{"ok":false,"error":"<具体原因>"}` |

## 改动设计（按仓库）

### A. unified_capture 守护进程（C++）

#### A1. 命令字扩展（`app/socket_server.{h,cpp}`，遵循 `app/commands/README.md`）

- `SocketCommandKind` 增 `set_product`。
- `SocketCommand` 增 `std::string product` 字段。
- `parse_socket_command`：解析 `set_product:<name>`（name 非空且 ∈ mango/banana/cherry，否则 unknown）。
- 提取 `is_valid_product_profile(string_view)` 供解析与运行时复用。

#### A2. 配置层（`core/product_config.{h,cpp}`）

- 抽出 `load_product_configuration_for_profile(ProductProfile, camera_map_path)`：只按给定 profile 读 camera-map 并构建 `ProductConfiguration`（把现有 `load_product_configuration` 里「按 profile 填 wrist/cherry/sixcam」的 body 抽出复用）。
- `load_product_configuration` 改为：读 product.conf 得到 profile → 调上面的 for_profile 函数。
- 新增 `write_product_profile(path, profile)`：原子写 `product=<name>\n`（写 `<path>.tmp` 再 `rename`），复用 `product_profile_name()`。
- 新增 `parse_product_profile(string_view)`：字符串 → `ProductProfile`（供 socket 解析与写文件复用），替代散落的 `if value=="mango"...` 判断。

#### A3. 运行态重构（`app/runtime.cpp`，核心）

现状 `run()` 顶部一次性 `load → discover → validate → build SessionRunner`，且 `SessionRunner sessions` 是栈对象、`configuration` 是 `const`。改为：

- 新增 `CaptureState` 结构（匿名 namespace 内）：
  ```cpp
  struct CaptureState {
    ProductConfiguration configuration;
    CameraDiscoveryResult cameras;
    CaptureSensorStatus sensor_status;
    SessionOptions session_options;
    std::unique_ptr<SessionRunner> sessions;
  };
  ```
- 抽出 `bool initialize(CaptureState*, const RuntimeOptions&, std::string* err)`：执行 load + discover + 各 profile 相机校验 + IMU/VIVE 推导 + 建 `sessions`。IMU 禁用用局部 `bool use_imu = options_.use_imu` 而非就地改 `options_`（消除 R-副作用）。
- 抽出 `bool reload(CaptureState*, const RuntimeOptions&, ProductProfile target, std::string* err)`：
  1. `load_product_configuration_for_profile(target, camera_map_path)` 得到新 config（先不写 product.conf）。
  2. `discover_cameras(new_config)`。
  3. 复用 initialize 的相机校验（cherry/banana/mango 要求）。
  4. 全部通过后：`write_product_profile(product_config_path, target)` 再交换 state（`*state = std::move(new_state)`）。
  5. 任一步失败：返回 false + 原因，不改 product.conf、不交换 state。
- `handle_socket_command` 增 `set_product` 分支：
  - `session_running_` 为 true → `{"ok":false,"error":"busy"}`。
  - `parse_product_profile(command.product)` 失败 → `{"ok":false,"error":"unknown product"}`。
  - `reload(...)` 失败 → `{"ok":false,"error":"<err>"}`。
  - 成功 → `{"ok":true,"product":"<name>"}`。
- `status` 分支改用 `state->configuration.profile` / `state->cameras` / `state->sensor_status`，使切换后 `status.product`/`cameras` 立即反映新 profile。

> 说明：`SessionRunner` 含 `std::atomic<bool>&` 引用成员，不可拷贝/赋值，故用 `std::unique_ptr<SessionRunner>` 持有以支持整体替换。空闲时其 `sensors_` 已空、析构安全。

### B. device-ui `server.cjs`（Node）

- 顶部新增映射常量（唯一权威来源）：
  ```js
  const PRODUCT_PROFILE_MAP = { Banana: 'mango', Mango: 'banana' };
  const PROFILE_PRODUCT_MAP = { mango: 'Banana', banana: 'Mango' };
  ```
  （Cherry 仅预留，不暴露。）
- 新增 `apiSetProduct(req, res)`：读 body `product`，查表映射为 profile；`captureCtl('set_product:' + profile)`；成功回前端名、失败透传 `error`。
- 路由表新增 `POST /api/product`（`server.cjs` router 区，与 `POST /api/settings` 相邻）。
- `apiStatus`/`getRecordStatus` 中把 `captureStatus` 的 `product`（守护进程 profile 名）经 `PROFILE_PRODUCT_MAP` 映射回前端名，前端看到的 `status.product` 与自己的命名一致（可选但建议，避免又一处错位）。

### C. device-ui 前端（React）

- `frontend/src/services/deviceApi.ts`：`api` 增 `setProduct: (product) => request('POST', '/api/product', { product })`。
- `frontend/src/App.tsx` `selectProduct`：改为 async——先 `await api.setProduct(next)`，成功再 `setProduct` + `saveSelectedProduct` + `go`；失败 `notify(error)` 且不切换。
- `frontend/src/app/product.ts`：`SelectableProduct` 不变（仍 `'Banana'|'Mango'`）。

## 兼容性与回滚

- 新增 socket 命令与 `/api/product` 均为新增接口，不改变现有 `start/stop/status/preview` 语义；旧前端（不发 /api/product）行为不变。
- 守护进程 `product.conf` 仍是唯一持久化事实源，运维可手动编辑，热重载命令只是程序化写入入口。
- 回滚：`git revert` 即回到「前端只写 localStorage、后端只读启动配置」的旧行为，无数据迁移。

## 关键取舍

- **热重载 vs 自重启**：选热重载（用户已定），零停机，但引入 `CaptureState` 重构与 V4L2 重复枚举验证成本。产品切换是低频显式操作，本可以自重启省事，但用户要求不重启。
- **映射放 server.cjs 而非守护进程**：守护进程保持领域纯粹（只认硬件 profile 名），映射是「展示/适配」职责，归属 device-ui；避免把前端命名泄漏进 C++ 采集核心。
- **校验先行、后写文件**：先 load+discover+validate 目标 profile，成功才写 product.conf 并交换，保证失败不破坏旧态（AC5）。
