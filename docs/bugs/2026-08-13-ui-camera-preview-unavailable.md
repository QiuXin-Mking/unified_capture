# BUG：UI 展示功能 — 头部双目/四目预览通道无数据（unified_capture 服务未运行）

## 元数据

| 字段 | 内容 |
|------|------|
| 状态 | 已规避 |
| 严重级别 | High |
| 首次发现 | 2026-08-13 10:2x |
| 最后更新 | 2026-08-13 |
| 负责人 | unified_capture 团队 |
| 影响版本 | `device-ui.service`（`server.cjs` 08-12 18:14）+ `unified_capture.service`（binary 08-12 18:17，`/usr/local/bin/unified_capture`） |
| 关联任务 | 无 |

> **目标：** 记录 UI「展示功能」中头部双目（jhh02）、头部四目（jhh04）预览通道看不到数据的根因、证据与恢复方式。

---

## 现象

- UI「展示功能 / 相机」页里，头部双目、头部四目预览通道看不到数据，表现为「无法使用」。
- 恢复运行后曾出现短暂「一闪一闪」，随后稳定显示正常。

## 影响

- 影响功能：UI 相机实时预览（`/api/camera/preview/head-stereo`、`/api/camera/preview/head-four`）。
- 影响设备或数据：无数据丢失（采集链路本身未受影响，仅预览接口不可用）。
- 是否阻塞发布或交付：是（展示功能为交付验收项）。
- 临时恢复方式：`systemctl start unified_capture.service`。

## 环境

| 项目 | 值 |
|------|----|
| 硬件 | RK3588（hostname `rk`） |
| 操作系统 / 内核 | Debian（`/sbin/init`，未注明内核版本） |
| 软件提交 | `server.cjs` 08-12 18:14；`unified_capture` binary 08-12 18:17 |
| SDK / 驱动 | MPP（`e34f0dd1`）；Node `v12.22.12` |
| 设备连接 | `1bcf:2d50`(jhh02)、`1bcf:2d51`(jhh04)、`1bcf:2d52`×2(SL/JHHSW) 共 4 个 UVC |
| 启动参数 | `/usr/local/bin/unified_capture --socket --single /media/usb0/capture` |

## 复现步骤

### Step 1：重启板子后检查采集服务状态

**操作：**

```bash
systemctl is-enabled unified_capture.service
systemctl is-active  unified_capture.service
ls -l /tmp/unified_capture.sock
ps aux | grep -E 'unified_capture' | grep -v grep
```

**预期：** `enabled`、`active`，socket 存在，守护进程在运行。

**实际：** `disabled`、`inactive (dead)`，socket 不存在，无进程。

### Step 2：通过 UI 或 curl 访问预览通道

**操作：**

```bash
curl -s -X POST http://localhost:8080/api/camera/live/start
curl -i http://localhost:8080/api/camera/preview/head-stereo
```

**预期：** 返回 HTTP 200 与 JPEG 帧。

**实际：** 返回 HTTP 503 `preview not started`。

## 预期结果

守护进程开机自启；UI 展示功能里头部双目/四目持续返回有效 JPEG 预览帧。

## 实际结果

守护进程未运行，预览接口全部 503；`systemctl start` 后恢复为 HTTP 200 + 有效 JPEG。

## 证据

### 关键日志

```text
# 服务状态
$ systemctl is-enabled unified_capture.service
disabled
$ systemctl is-active unified_capture.service
inactive

# socket 缺失
$ ls -l /tmp/unified_capture.sock
ls: 无法访问 '/tmp/unified_capture.sock': 没有那个文件或目录

# 板子当天早上重启（守护进程未随开机拉起）
$ uptime
 10:21:13 up 19 min, 0 users, load average: 0.78, 0.30, 0.16

# journal 显示最后一次运行停留在 08-12 18:19（session 35）
$ journalctl -u unified_capture.service -n 3
8月 12 18:19:33 rk unified_capture[148111]: >>> Session 35 START <<<
8月 12 18:19:33 rk unified_capture[148111]: [jhh02] setup OK (4000x1200@30fps, H265=Y, Y8=N)
8月 12 18:19:33 rk unified_capture[148111]: [jhh04] setup OK (3104x480@30fps, H265=N, Y8=N)
```

### 实验结果

| 条件 | 结果 | 结论 |
|------|------|------|
| 服务未启动时 `GET /api/camera/preview/head-stereo` | 503 `preview not started` | 预览不可用 |
| `systemctl start` 后 `status` | `{"ok":true,"ready":true,"jhh02":true,"jhh04":true}` | 设备就绪 |
| 恢复后单通道轮询 12 次（500ms） | 全部 HTTP 200，JPEG 316K–491K | 后端稳定 |
| 恢复后双通道同时轮询 15 轮（850ms） | 全部 HTTP 200，0 失败 | 后端稳定 |
| `file` 校验预览帧 | `JPEG image data, baseline, 4000x1200` / `3104x480` | 帧内容有效 |

## 根因分析

**结论状态：** 已确认

`unified_capture.service` 处于 `disabled`（未 `WantedBy` 开机自启）状态。当天早上板子重启后，守护进程未被 systemd 拉起，`/tmp/unified_capture.sock` 因此不存在。`server.cjs` 的 `captureActive()` 依赖该 socket 返回 `ok`，socket 缺失时返回 false，导致 `apiCaptureChannelPreview` 直接返回 503，前端呈现「无信号」。

## 解决或规避方案

### 当前方案

```bash
systemctl start unified_capture.service   # 临时恢复
systemctl enable unified_capture.service  # 持久化（待确认是否已执行）
```

### 风险与限制

- 若只 `start` 不 `enable`，下次重启会再次复现。
- 顺带发现（不同根因，建议独立建档）：
  1. `preview-control.cjs` 的 `stop()` 使用 `fs.rmSync`，Node `v12.22.12` 无此 API（≥14.14），导致 `live/stop` 报 `fs.rmSync is not a function`，遗留 `.device-ui-preview` 孤儿 session（每个约几十 MB，本次遗留 59 MB，已手动删除）。修法：改用 `fs.rmdirSync(dir, { recursive: true })`。
  2. `/etc/unified_capture/product.conf` 为 `product=mango`（legacy_head），腕部相机（SL/JHHSW）不被采集；如需「左手/右手」预览需改回 `banana`。

## 验证结果

**验证状态：** 已通过

| 验证项 | 操作 | 预期 | 实际 | 结论 |
|--------|------|------|------|------|
| 头部双目预览 | `curl /api/camera/preview/head-stereo` | 200 + JPEG | 200，4000×1200 JPEG | 通过 |
| 头部四目预览 | `curl /api/camera/preview/head-four` | 200 + JPEG | 200，3104×480 JPEG | 通过 |
| 连续轮询稳定性 | 双通道 ×15 轮 @850ms | 无 503 | 0 失败 | 通过 |

## 相关文件

- `/etc/systemd/system/unified_capture.service` — 服务单元，`ExecStart=/usr/local/bin/unified_capture --socket --single /media/usb0/capture`
- `/root/ui/server.cjs` — `captureActive()` / `apiCaptureChannelPreview` 预览后端
- `/root/ui/preview-control.cjs` — `createPreviewController` 与 `fs.rmSync` 清理逻辑
- `/root/unified_capture/app/runtime.cpp` — 守护进程 socket 控制与预览命令分发

## 经验教训

1. 依赖 `unified_capture` socket 的 UI 功能，其可用性直接取决于 systemd 服务是否 `enabled`；排查预览「无信号」时应先确认 `systemctl is-active unified_capture.service` 与 socket 是否存在。
2. 板端 Node 仍为 v12，使用 Node 新增 API（如 `fs.rmSync`）前需确认运行时版本，避免在停止预览等边界路径上静默失败。

## 变更记录

| 日期 | 修改人 | 内容 |
|------|--------|------|
| 2026-08-13 | unified_capture 团队 | 首次记录 |
