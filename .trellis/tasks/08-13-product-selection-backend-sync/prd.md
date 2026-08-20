# 前端产品选择同步后端并持久化

## Goal

用户在 device-ui 前端选择产品（Mango / Banana）后，选择结果要传递到后端并持久化，让后端（unified_capture 守护进程）记住该产品变更并让新产品立即生效，消除「前端 localStorage 一套、后端 product.conf 一套且互不同步」的现状。

## Background

### 现状：前后端产品概念各自独立、命名错位

**前端（device-ui）**
- 产品选择只写浏览器 `localStorage`（`frontend/src/app/product.ts`：`SELECTED_PRODUCT_KEY = 'sensorhub-product'`），`saveSelectedProduct()` / `loadSelectedProduct()` 纯前端，不调任何后端接口。
- 选择入口 `frontend/src/App.tsx:135-139` 的 `selectProduct()`：只做 `setProduct` + `saveSelectedProduct` + 跳转，无 API 调用。
- 可选类型 `SelectableProduct = 'Banana' | 'Mango'`（`product.ts:1`）；`Cherry` 在 `HomeScreen.tsx:59` 图标分支与测试中出现，但 UI 标记「不可用」。
- 产品含义（`HomeScreen.tsx:25-43`）：`Banana` = 指尖夹爪、板机夹爪、头部 Ego 与手套；`Mango` = 头部 Ego + 左/右腕部 Ego。

**后端（unified_capture 守护进程）**
- 产品由 `/etc/unified_capture/product.conf` 决定，profile ∈ `mango`/`banana`/`cherry`（`core/product_config.h:6`），**仅启动时读一次**（`app/runtime.cpp:88-94`），之后相机发现、SessionRunner、`status.cameras` 全围绕启动时配置构建，**运行时无切产品/重载机制**。
- Socket 协议只有 `start`/`stop`/`status`/`preview:<channel>:<path>`（`app/socket_server.cpp` `parse_socket_command`、`app/runtime.cpp:203`），无 set-product 命令。
- `status` 响应已含 `product` 字段（`app/status_response.cpp:40`）。

**命名错位（核心坑，`unified_capture/CLAUDE.md:11`）**

| 前端产品名 | 守护进程 profile | 采集内容 |
|---|---|---|
| `Mango` | **`banana`** | 头部 Ego + 左腕 + 右腕（腕部 SL/JHHSW + 六目 jhh02/jhh04） |
| `Banana` | **`mango`**（legacy_head） | 仅头部六目，不采集腕部 |
| `Cherry` | `cherry` | YCTC SC233HGS 双目 |

### 通信与部署

- 守护进程 systemd：`ExecStart=/usr/local/bin/unified_capture --socket --single /media/usb0/capture`，`Restart=always`（`deploy/unified_capture.service`）。`--single` 每次 session 结束退出、systemd 拉起。
- device-ui `server.cjs`（Node）通过 `CAPTURE_SOCK='/tmp/unified_capture.sock'` 的 `captureCtl()` 短连接通信，已有 `start/stop/status/preview` 映射，无产品路由（`server.cjs:1123-1156`）。
- 前端 `frontend/src/services/deviceApi.ts` 的 `request()` 封装 `fetch`，`api` 对象集中定义全部接口。
- 项目已有命令字扩展约定：`app/commands/README.md`（新增枚举→解析→分发→更新 `docs/socket-control.md`）。

## Requirements

- **R1** 前端选择产品时，除写 `localStorage`（保留为离线/启动默认）外，向后端发起产品变更请求。
- **R2** device-ui `server.cjs` 新增产品接口，把前端产品名映射到守护进程 profile 后转发给守护进程；映射表是唯一权威来源，双向映射（请求前端名→profile、`status.product` profile→前端名）。
- **R3** unified_capture 守护进程新增 `set_product:<profile>` socket 命令，空闲态下校验目标 profile、重读配置、重枚举相机、原子替换运行态，并持久化到 `/etc/unified_capture/product.conf`（原子写）。
- **R4** 切换失败（正在采集 / 未知 profile / 目标产品无相机 / 配置错误）时，守护进程保留旧产品运行态并返回明确错误，不进入坏状态。
- **R5** 命名错位（Mango↔banana）由显式映射层处理，不靠人脑记，并有测试锁定。

## Acceptance Criteria

- [ ] **AC1** 前端在「Mango」产品卡片点击选择后，`server.cjs` 收到 `POST /api/product`，映射为 `banana` 并下发 `set_product:banana`；守护进程 `status.product` 变为 `banana`，`status.cameras` 出现 `wrist_left`/`wrist_right`（`banana` profile 的 key 集合）。
- [ ] **AC2** 前端选择「Banana」后，守护进程 `status.product` 变为 `mango`，`status.cameras` 出现 `jhh2_left`/`jhh2_right`（legacy_head）。
- [ ] **AC3** 切换成功后被持久化：`/etc/unified_capture/product.conf` 内容为 `product=banana`（或对应值），守护进程**不重启**即生效（进程 PID 不变）。
- [ ] **AC4** 采集进行中（`running:true`）下发 `set_product`，守护进程返回 `{"ok":false,"error":"..."}`，`status.product` 与运行态不变。
- [ ] **AC5** 目标产品相机发现失败时，守护进程返回错误、`status.product` 保持旧值、`product.conf` 未被改写。
- [ ] **AC6** 前端选择成功后有成功反馈（toast），失败时显示错误且 UI 产品停留在原值；`localStorage` 与后端状态一致。
- [ ] **AC7** 映射表有单元测试：`Mango→banana`、`Banana→mango` 双向一致，杜绝错位回归。
- [ ] **AC8** 守护进程新增命令有回归测试（`tests/test_socket_command.cpp`、`tests/test_socket.sh`）覆盖解析与 set_product 行为；`make test` 通过。

## Key Decisions

- **D1 生效机制 = 运行时热重载（不重启）**：`set_product:<profile>` 在空闲态重读配置、重枚举相机、原子替换运行态，进程不退出。
- **D2 命名错位 = 保留 + 显式映射层**：映射表唯一权威放在 device-ui `server.cjs`，守护进程只认自己的 `mango`/`banana`/`cherry`。
- **D3 范围 = 前端仅 Mango/Banana 可选**（对齐现有 `SelectableProduct`）；守护进程 `set_product` 命令接受全部三个 profile（`load_product_configuration` 已支持 cherry，成本为零）。Cherry 前端选不在此期（仍置灰）。

## Out of Scope

- 任务平台/领取、云端上报、设备商城、精选内容（见 `device-ui/docs/前端已实现但后台未支持的功能.md`，与本需求无关）。
- 音频采集（腕部麦克风）——`unified_capture/CLAUDE.md` 明确未批准前不增加。
- 前端「启动时反向读取后端 product 并对账」的完整双向同步（见 Deferred）。

## Risks / Deferred

- **R-热重载重构**：`SessionRunner` 含引用成员不可赋值，需改为 `std::unique_ptr`/`std::optional` 持有；`runtime.cpp` 初始化逻辑抽成可重入。属本需求核心，已纳入 implement。
- **R-V4L2 重复枚举**：`discover_cameras()` 热重载时二次调用是否残留全局状态，需实现期验证（若无，需加清理）。
- **R-IMU/VIVE 副作用**：现 `run()` 会就地改 `options_.use_imu`，热重载需改为局部变量，避免切换后状态粘连。
- **Deferred 启动对账**：前端启动时以 localStorage 为默认、推送后持久化，暂不做「读后端 product 覆盖本地」的双向对账（除非外部手动改 product.conf 才可能漂移）。可后续补充。
