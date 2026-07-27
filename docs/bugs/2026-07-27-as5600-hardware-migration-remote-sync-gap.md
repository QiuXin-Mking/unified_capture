# BUG：AS5600 驱动迁移 — 板端同步不完整导致构建状态混合

## 元数据

| 字段 | 内容 |
|------|------|
| 状态 | 已定位 |
| 严重级别 | High |
| 首次发现 | 2026-07-27 00:27 CST |
| 最后更新 | 2026-07-27 00:27 CST |
| 负责人 | unified_capture 团队 |
| 影响版本 | 未提交的 AS5600 路径迁移工作区 |
| 关联任务 | 将 `as5600.c/.h` 迁至 `hardware/as5600/` 并在 RK3588 编译验证 |

> **目标：** 记录 AS5600 驱动迁移时本地与 RK3588 板端状态不一致的事实、恢复步骤和后续硬件验证待办。

---

## 现象

本地工作区已将根目录的 `as5600.c`、`as5600.h` 迁至 `hardware/as5600/`，并更新了 Makefile、CMake、调用头文件和目录布局测试。

在 RK3588 板端仅删除了根目录旧文件后执行构建，板端仍使用旧 Makefile，且没有同步新目录和测试脚本，导致目录测试文件缺失、`main.o` 继续依赖不存在的根目录 `as5600.h`。

## 影响

- 影响功能：RK3588 板端无法编译 `unified_capture`，因此无法验证 AS5600 路径迁移。
- 影响设备或数据：尚未启动采集，无采集数据影响。
- 是否阻塞发布或交付：是，迁移未完成目标硬件验证。
- 临时恢复方式：将本地迁移文件完整同步到板端后，再删除板端根目录旧文件并构建。

## 环境

| 项目 | 值 |
|------|----|
| 硬件 | RK3588；板端 shell 提示符为 `root@lubancat` |
| 操作系统 / 内核 | 未确认 |
| 软件提交 | 本地为未提交迁移；板端为旧 Makefile 和不完整文件集 |
| SDK / 驱动 | TSTC SDK、Rockchip MPP、Linux I2C；版本未确认 |
| 设备连接 | SSH 目标 `root@192.168.100.200`，板端项目目录 `/root/pr-file/01-统一采集方案/unified_capture` |
| 启动参数 | 不适用（构建阶段） |

## 复现步骤

### Step 1：只删除板端旧路径文件

**操作：**

```bash
cd /root/pr-file/01-统一采集方案/unified_capture
rm -f as5600.c as5600.h as5600.o
```

**预期：** 删除旧文件前，应已同步使用 `hardware/as5600/` 路径的 Makefile、源文件、头文件和测试脚本。

### Step 2：在未完整同步的板端构建

**操作：**

```bash
make clean
make test_hardware_header_layout test_time_utils test_output_path
make
```

**预期：** `clean` 应删除 `hardware/as5600/as5600.o`，目录布局测试可运行，`main.o` 只依赖 `hardware/as5600/as5600.h`。

### Step 3：从当前 agent 环境访问板端

**操作：**

```bash
ssh root@192.168.100.200
```

**预期：** 成功建立 SSH 连接，以便同步和执行板端验证。

## 预期结果

板端目录应包含 `hardware/as5600/as5600.c`、`hardware/as5600/as5600.h` 和 `tests/test_hardware_header_layout.sh`；根目录不再存在 `as5600.c/.h`。`make` 应以 `hardware/as5600/as5600.o` 为对象文件完成全量构建。

## 实际结果

- 板端 `clean` 仍输出 `rm -f unified_capture *.o`，表明 Makefile 未迁移。
- 板端没有 `tests/test_hardware_header_layout.sh`。
- 删除根目录 `as5600.h` 后，旧 `main.o` 依赖规则立即构建失败。
- 当前 agent 的执行环境在 TCP 连接阶段拦截 SSH，无法自行补齐板端文件或确认板端状态。

## 证据

### 关键日志

```text
rm -f unified_capture *.o
sh tests/test_hardware_header_layout.sh
sh: 0: cannot open tests/test_hardware_header_layout.sh: No such file
make: *** [Makefile:71：test_hardware_header_layout] 错误 2
make: *** 没有规则可制作目标“as5600.h”，由“main.o” 需求。 停止。

ssh: connect to host 192.168.100.200 port 22: Operation not permitted
```

### 实验结果

| 条件 | 结果 | 结论 |
|------|------|------|
| 本地目录布局测试 | `make test_hardware_header_layout` 退出码 0 | 本地迁移结构和引用一致 |
| 本地纯工具测试 | `test_time_utils`、`test_output_path` 退出码 0 | 与迁移无关的可执行回归项通过 |
| 本地 Linux I2C 驱动对象编译 | macOS 缺少 `linux/i2c-dev.h` | 不能替代 RK3588/Linux 编译验证 |
| 板端测试 | 缺少测试脚本，旧 Makefile 仍依赖根目录头文件 | 板端没有收到完整迁移文件 |
| 当前 agent SSH | TCP `connect()` 返回 `Operation not permitted` | 网络沙箱阻断，不是认证或板端命令失败 |

## 根因分析

**结论状态：** 已确认

板端只执行了旧根目录驱动文件的删除，但本地迁移涉及的 Makefile、CMakeLists、`encoder_sensor.h`、测试脚本和 `hardware/as5600/` 文件未完整同步。因此板端落入“旧构建规则 + 新文件缺失”的不一致状态。

当前 agent 无法直接修复板端，是因为执行环境在 SSH 的 TCP `connect()` 阶段返回 `Operation not permitted`；该限制发生在认证之前。

## 解决或规避方案

### 当前方案

由可访问局域网板端的终端执行以下顺序。同步成功前不要删除板端根目录驱动文件。

```bash
# 在本机 unified_capture 目录执行；-R 保留 hardware/ 与 tests/ 相对目录。
rsync -avR \
  CMakeLists.txt Makefile README.md encoder_sensor.h \
  tests/test_hardware_header_layout.sh \
  hardware/as5600/as5600.c hardware/as5600/as5600.h \
  root@192.168.100.200:/root/pr-file/01-统一采集方案/unified_capture/

# 在 RK3588 板端执行。
cd /root/pr-file/01-统一采集方案/unified_capture
rm -f as5600.c as5600.h as5600.o
make clean
make test_hardware_header_layout test_time_utils test_output_path
./test_time_utils
./test_output_path
make
```

若首条 `rsync` 的本地工作目录不是 `unified_capture`，必须改为正确目录或使用完整路径，确保远端的相对路径仍为 `tests/...` 与 `hardware/as5600/...`。

### 风险与限制

- `rsync` 默认不删除远端旧文件；根目录的两个旧源文件必须在确认新文件到位后显式删除。
- `make clean` 的新规则只清理 `main.o` 和 `hardware/as5600/as5600.o`；遗留的根目录 `as5600.o` 需单独删除一次。
- 本地 macOS 缺少 Linux I2C 头文件，不能用本机 `make` 判断驱动对象是否可编译。
- 当前 agent 无法 SSH 到板端；下一位 agent 需在可访问 `192.168.100.200:22` 的环境中执行板端步骤。

## 验证结果

**验证状态：** 部分通过

| 验证项 | 操作 | 预期 | 实际 | 结论 |
|--------|------|------|------|------|
| 本地路径和引用 | `make test_hardware_header_layout` | 新文件存在、旧路径不存在、引用更新 | 通过 | 通过 |
| 本地工具回归 | `test_time_utils`、`test_output_path` | 退出码 0 | 通过 | 通过 |
| RK3588 同步 | 完整 `rsync` | 板端接收全部迁移文件 | 未执行 | 未验证 |
| RK3588 全量构建 | `make` | 成功产生 `unified_capture` | 未执行 | 未验证 |

## 相关文件

- `Makefile` — 新对象文件路径、依赖和 clean 规则。
- `CMakeLists.txt` — AS5600 源文件路径。
- `encoder_sensor.h` — AS5600 公共头文件的 include 路径。
- `hardware/as5600/as5600.c`、`hardware/as5600/as5600.h` — 迁移后的驱动文件。
- `tests/test_hardware_header_layout.sh` — 迁移路径和引用约束。
- `docs/bugs/2026-07-27-as5600-hardware-migration-remote-sync-gap.md` — 本记录。

## 经验教训

1. 文件移动必须作为一个同步原子单元处理：先同步构建规则、引用、源文件和测试，再删除旧路径。
2. `rsync` 不带删除参数不会传播重命名后的旧文件删除；应对每个需删除的远端文件列出精确操作。
3. 未完成板端验证时，不能将本地目录测试或 `make -n` 当作 RK3588 全量构建成功。
4. 远程操作受执行环境网络策略限制时，应记录精确报错和交接命令，避免把连接限制误判为设备或认证故障。

## 变更记录

| 日期 | 修改人 | 内容 |
|------|--------|------|
| 2026-07-27 | Codex | 首次记录本地/板端迁移状态不一致、SSH 沙箱限制与交接步骤 |
