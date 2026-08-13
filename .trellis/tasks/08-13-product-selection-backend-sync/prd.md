# 前端产品选择同步后端并持久化

## Goal

用户在 device-ui 前端选择产品（Mango / Banana / Cherry）后，选择结果要传递到后端并持久化，让后端（unified_capture 守护进程）记住该产品变更，而不是前后端各自维护一套互不同步的产品概念。

## Background（已确认事实）

### 现状：前后端产品概念各自独立、命名错位

**前端（device-ui）**
- 产品选择只存在浏览器 `localStorage`（`device-ui/frontend/src/app/product.ts`：`SELECTED_PRODUCT_KEY = 'sensorhub-product'`），`saveSelectedProduct()` / `loadSelectedProduct()` 都是纯前端、不调任何后端接口。
- 选择入口在 `device-ui/frontend/src/App.tsx:135-139` 的 `selectProduct()`：只做 `setProduct` + `saveSelectedProduct` + 跳转，没有 API 调用。
- 可选产品类型 `SelectableProduct = 'Banana' | 'Mango'`（`product.ts:1`）；`Cherry` 出现在 `HomeScreen.tsx:59` 的图标分支和测试里，但被标记为「不可用」。
- 产品含义（`HomeScreen.tsx:25-43`）：
  - `Banana` = 指尖夹爪、板机夹爪、头部 Ego 与手套
  - `Mango` = 头部 Ego、左腕部 Ego 与右腕部 Ego

**后端（unified_capture 守护进程）**
- 产品由 `/etc/unified_capture/product.conf` 决定，profile ∈ `mango` / `banana` / `cherry`（`core/product_config.h:6`），**仅在启动时读取一次**（`app/runtime.cpp:88-94`），之后 `discover_cameras()`、`SessionRunner`、`status.cameras` key 集合都围绕这份启动时配置构建，**运行时无任何切产品/重载机制**。
- Socket 协议目前只有 `start` / `stop` / `status` / `preview:<channel>:<path>`（`app/socket_server.cpp` `parse_socket_command`；`app/runtime.cpp:203` `handle_socket_command`），**没有 set-product / reload 命令**。
- `status` 响应已含 `"product"` 字段（`app/status_response.cpp:40`），会返回守护进程当前的 profile 名。

### 命名错位（核心坑，来自 `unified_capture/CLAUDE.md:11`）

| 前端产品名 | 守护进程 profile | 采集内容 |
|---|---|---|
| `Mango` | **`banana`** | 头部 Ego + 左腕 + 右腕（腕部 SL/JHHSW + 六目 jhh02/jhh04） |
| `Banana` | **`mango`**（legacy_head） | 只有头部六目，不采集腕部 |
| `Cherry` | `cherry` | YCTC SC233HGS 双目（UVC H.264 + CDC ACM） |

即：前端选「Mango」时 `product.conf` 必须配 `banana`，而不是 `mango`；名字正好错位。

### 部署/运行方式

- 守护进程 systemd 服务：`ExecStart=/usr/local/bin/unified_capture --socket --single /media/usb0/capture`，`Restart=always`、`RestartSec=2`（`deploy/unified_capture.service`）。`--single` 模式下每次 session 结束守护进程即退出，由 systemd 拉起，读取新的 `product.conf`。
- device-ui 的 `server.cjs`（Node）通过 `CAPTURE_SOCK = '/tmp/unified_capture.sock'` 的 `captureCtl()` 短连接与守护进程通信，已有 `start`/`stop`/`status`/`preview` 映射，但**没有产品相关路由**。
- device-ui 前端通过 `frontend/src/services/deviceApi.ts` 调 `server.cjs` 的 `/api/*`。

## Requirements

1. 前端选择产品时，除写入 `localStorage`（保留为离线/启动默认值）外，还要把选择发往后端。
2. 后端（device-ui server.cjs）新增产品相关 API，把前端产品名映射到守护进程 profile，并转发给 unified_capture 守护进程。
3. unified_capture 守护进程收到切产品指令后，把新 profile 持久化到 `/etc/unified_capture/product.conf`，并让新 profile 生效。
4. 命名错位（Mango↔banana）必须显式处理：映射关系有唯一权威来源，不靠人脑记。

## Key Decisions（已定）

- **D1 — 生效机制：运行时热重载（不重启）**。新增 `set_product:<profile>` socket 命令，守护进程在**空闲态**（`!session_running_`）重读配置、重枚举相机、原子替换运行态，不退出进程。代价：需把 `SessionRunner` 从栈上对象改为可替换持有（`std::unique_ptr`/`std::optional`，因其含引用成员不可赋值），并把 `runtime.cpp` 中「加载配置→发现→校验→建 SessionRunner」抽成可重入初始化。
- 持久化仍写 `/etc/unified_capture/product.conf`（原子写 tmp+rename），由守护进程完成（它已拥有该文件、以 root 运行）。

## Technical Notes

- `SessionRunner` 空闲时无活动线程（`sensors_` 每次 session 前 `clear()`、结束 `wait_teardown()` join），产品相关状态只有 `cameras_` 一份发现结果拷贝，热重载可在空闲态安全替换。
- `load_product_configuration()`（`core/product_config.cpp`）、`discover_cameras()`（`hardware/video/device_discovery.cpp`）均为纯读、无副作用；`discover_cameras` 重复枚举的 V4L2 全局状态残留需在实现期验证。
- 切换校验策略（待定，倾向）：先对目标 profile 完整 load+discover+校验成功，再写 product.conf 并交换运行态；失败则保留旧 profile 并返回错误，避免守护进程进入坏状态。
- `status` 响应的 `product` 字段（`app/status_response.cpp:40`）会随热重载切换为新 profile，前端可据此回读后端当前生效产品。

## Acceptance Criteria

- [ ] TBD（待产品决策确定后补全）

## Out of Scope（暂不纳入，除非另有决策）

- 任务平台/任务领取、云端上报、设备商城、精选内容等（见 `device-ui/docs/前端已实现但后台未支持的功能.md`，与本需求无关）。
- 音频采集（腕部麦克风）——`unified_capture/CLAUDE.md` 明确「未另行批准前不增加音频」。

## Open Questions（阻塞规划的产品决策）

1. **命名错位的处理方式**：保留错位 + 显式映射层（唯一权威来源），还是重命名一侧消除错位。
2. **MVP 范围**：是否纳入 `Cherry`，还是只做 `Mango`/`Banana`。
