# 执行计划 — 前端产品选择同步后端并持久化

> 跨两个仓库。建议顺序：先守护进程（后端事实源），再 server.cjs 映射，最后前端接线。每个仓库独立提交。

## 阶段 1：unified_capture 配置层（无行为变化，纯重构）

1. `core/product_config.h`：增 `ProductProfile parse_product_profile(std::string_view)`、`ProductConfigResult load_product_configuration_for_profile(ProductProfile, const std::string& camera_map_path)`、`bool write_product_profile(const std::string& path, ProductProfile)` 声明。
2. `core/product_config.cpp`：
   - 抽出「按 profile 填 wrist/cherry/sixcam」为独立 body，供 `load_product_configuration_for_profile` 复用；`load_product_configuration` 改为读 product.conf 后调用它。
   - 实现 `parse_product_profile`（复用现有 `value=="mango"` 判断）；实现 `write_product_profile`（tmp+rename 原子写）。
3. 补测试 `tests/test_product_config.cpp`：`write_product_profile` 后读回一致、`parse_product_profile` 非法输入返回失败、`load_product_configuration_for_profile` 三种 profile 均可构建。
4. `make test` 通过（含现有 `test_cherry_product_config.cpp` 回归）。

## 阶段 2：unified_capture 命令字 + 热重载

5. `app/socket_server.h`：`SocketCommandKind` 增 `set_product`；`SocketCommand` 增 `std::string product`。
6. `app/socket_server.cpp`：`parse_socket_command` 增 `set_product:<name>` 解析（name ∈ mango/banana/cherry，复用 `parse_product_profile`/`product_profile_name` 校验，否则 unknown）。
7. `app/runtime.cpp`（核心重构）：
   - 匿名 namespace 内新增 `CaptureState` 结构 + `initialize()` / `reload()` 帮助函数（见 design A3）。
   - `run()` 顶部改用 `initialize()` 建 state；`SessionRunner` 用 `std::unique_ptr` 持有。
   - `handle_socket_command` 增 `set_product` 分支；`status` 分支读 `state->…`。
8. 补测试：
   - `tests/test_socket_command.cpp`：`set_product:banana` 解析正确、`set_product:kiwi`/`set_product:` 为 unknown。
   - 视硬件可否，`tests/test_socket.sh` 增一条 `set_product` 冒烟（无硬件环境至少覆盖解析层）。
9. 更新 `docs/socket-control.md`（命令表 + set_product 语义）与 `app/commands/README.md`（命令列表）。
10. `make test` 通过。

## 阶段 3：device-ui server.cjs 映射与路由

11. `server.cjs`：顶部增 `PRODUCT_PROFILE_MAP` / `PROFILE_PRODUCT_MAP`（唯一权威，附注释引用 CLAUDE.md 错位说明）。
12. 新增 `apiSetProduct(req, res)`：解析 body → 映射 profile → `captureCtl('set_product:'+profile)` → 回前端名或错误。
13. router 区新增 `POST /api/product`。
14. （建议）`apiStatus`/`getRecordStatus` 的 `captureStatus.product` 用 `PROFILE_PRODUCT_MAP` 映射回前端名。
15. `node --check server.cjs` 语法校验；有条件则起服务手动 curl 验证。

## 阶段 4：device-ui 前端接线

16. `frontend/src/services/deviceApi.ts`：`api` 增 `setProduct`。
17. `frontend/src/App.tsx`：`selectProduct` 改 async——成功后 `setProduct`+`saveSelectedProduct`+`go`，失败 `notify` 不切换。
18. 补前端测试（`frontend/src/` 现有 Vitest）：`setProduct` 成功/失败两分支；`product.test.ts` 映射关系断言（`Mango→banana`、`Banana→mango`）。
19. `pnpm --dir frontend test`（或项目既有 test 命令）通过。

## 验证命令

```bash
# 守护进程
cd unified_capture && make test
# 手动热重载冒烟（板端）
echo 'set_product:banana' | nc -U /tmp/unified_capture.sock   # {"ok":true,"product":"banana"}
echo 'status' | nc -U /tmp/unified_capture.sock               # product=banana, cameras 含 wrist_left/right
# device-ui
cd device-ui && node --check server.cjs
pnpm --dir frontend test
```

## 风险文件 / 回滚点

- `app/runtime.cpp`：`run()` 结构改动最大，是回滚与 review 重点；保持 `initialize()`/`reload()` 行为与原一次性初始化等价。
- `app/session_runner.{h,cpp}`：仅由「栈对象」变「unique_ptr 持有」，成员不变。
- `core/product_config.cpp`：抽出函数不得改变现有 profile 语义（`test_cherry_product_config.cpp` 是回归护栏）。
- `server.cjs`：映射表放顶部显眼处，防止未来有人在别处手写错位。

## 实现期需验证的技术未知

- `discover_cameras()` 二次调用是否残留 V4L2 全局状态（若无清理，需在 reload 前释放）。
- 板端 `nc -U` 冒烟确认 `--single` 与 socket 模式下热重载均正常（热重载不退出进程）。
