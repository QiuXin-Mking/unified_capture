# 测试文档

## 测试分类

| 类别 | 位置 | 运行环境 | 说明 |
|------|------|---------|------|
| **单元测试** | `test_*.cpp` (项目根目录) | 任何 Linux | 纯逻辑验证，无硬件依赖 |
| **布局检查** | `tests/test_hardware_header_layout.sh` | 任何 Linux | 验证头文件路径与 include 一致性 |
| **系统测试** | `tests/test_*.sh` | 仅 RK3588 板端 | 需要硬件 + 程序运行中 |

## 运行测试

```bash
# 所有测试
make test_output_path
make test_time_utils
make test_hardware_header_layout

# 系统测试（需板端且程序运行中）
./tests/test_socket.sh
```

## 单元测试

### 要求

- 单文件 `.cpp`，`#include` 被测 header，`main()` 中用 `assert()` 验证
- 放在项目根目录，Makefile 提供对应 target：`make test_<name>`
- **不依赖** TSTC SDK、MPP、libsurvive 等板端库

### 现有测试

| 文件 | 覆盖 |
|------|------|
| `test_output_path.cpp` | SD 卡路径校验、前缀生成、挂载检测 |
| `test_time_utils.cpp` | `mkdir_p` 递归目录创建 |

### 命名约定

```
test_<module>.cpp          # 源文件
make test_<module>         # Makefile target
```

## 系统测试

系统测试在 **RK3588 板端**运行，需要 `unified_capture` 程序已在运行（socket 模式），通过 `/tmp/unified_capture.sock` 发送命令并验证响应。

### 参考实现

`test_socket.sh` — Socket 协议的完整验收脚本，覆盖以下场景：

| 序号 | 场景 | 预期结果 |
|------|------|---------|
| 1 | 启动阶段 status | 返回 JSON（可能 ready:false） |
| 2 | 等待设备就绪 | `"ready":true`，30 次 × 1s 轮询 |
| 3 | start 命令 | `"ok":true` |
| 4 | 采集中 status | `"running":true` + elapsed_ms |
| 5 | 重复 start | `"ok":false,"error":"already running"` |
| 6 | stop 命令 | `"ok":true,"elapsed_ms":NNN` |
| 7 | 重复 stop | `"ok":false,"error":"not running"` |
| 8 | 未知命令 | `"ok":false,"error":"unknown command"` |
| 9 | 停止后 status | `"running":false` |

### 编写新的系统测试

1. 新建 `tests/test_<feature>.sh`
2. 首行 `#!/bin/bash`，使用 `nc -U /tmp/unified_capture.sock` 与程序通信
3. 每个用例按 `序号. 场景描述 → 预期结果` 结构组织
4. 响应为单行 JSON，用 `grep` 验证关键字段
5. 完成后更新本文档的"系统测试"表格

### 系统测试约定

- 脚本自身不启停 `unified_capture`，由测试者手动启停或 systemd 管理
- round-trip 验证：`start` → `status`(running) → `stop` → `status`(idle)
- 边界条件必须覆盖：重复 start/stop、未知命令
