# unified_capture ↔ device-ui 对接实现详细记录

> 日期: 2026-07-26
> 涉及仓库: unified_capture + device-ui
> 关联计划: docs/plans/2026-07-26-integrate-unified-capture-with-device-ui.md

---

## 一、架构决策

### 1.1 对接方向：server.cjs 适配 unified_capture，前端不动

**决策：** 所有协议适配在 device-ui/server.cjs 中完成，前端 deviceApi.ts 接口签名保持向后兼容。

**理由：**
- 前端所有 API 调用通过 `deviceApi.ts` 集中管理，接口干净
- 前端 `RecordStatus` 的 `recording/cameraConnected/previewing` 字段语义够用
- 文件列表的 `Recording` 类型字段（hasColor/hasStereo/hasImu）可以映射
- 前端改动如果出问题，5.5" 触摸屏现场调试困难，不如后端改动可控

### 1.2 预览方案：collect() 循环内 downscale JPEG，零新线程

**决策：** 3840x1200 BGR → 1/4 缩放 (960x300) → JPEG 压缩，在 VideoSensor/SixCamSensor 现有的 collect() 循环中完成。

**理由：**
- unified_capture 有硬约束：TSTC SDK + MPP 对额外 pthread_create 敏感。生产事故记录在 `BUG_PTHREAD_SOCKET.md`：在设备初始化附近创建任何新 pthread（即使只做 socket I/O）会导致 MPP DMA buffer 分配失败、TSTC SDK STREAM_STATUS 永久死锁、所有文件产出 0 字节
- 复用 libjpeg-turbo（已链接，无新依赖）
- 5.5" 屏幕物理分辨率 1080x1920（竖屏），横屏模式下约 1920x1080，预览区域只占小部分，全分辨率浪费

### 1.3 下采样方式：最近邻（Nearest Neighbor）

**决策：** 使用简单最近邻下采样（每 4x4 像素块取左上角），不做双线性插值。

**理由：**
- 预览用途是操作人员瞄准相机，不需要高质量缩放
- 最近邻比双线性快约 4x，不增加 collect() 循环耗时
- 3840x1200 → 960x300，缩小比 4:1，最近邻效果可接受

### 1.4 JPEG 参数选择

**决策：** TJSAMP_420, quality=85, TJFLAG_FASTDCT

**理由：**
- `TJSAMP_420`（4:2:0 色度子采样）：libjpeg-turbo 2.0.6 支持，文件小
- quality=85：预览用途，视觉质量够用，文件 ~30KB
- `TJFLAG_FASTDCT`：使用快速 DCT 算法，减少编码耗时
- 实测：960x300 JPEG 约 30KB，编码耗时 <5ms

### 1.5 模式检测：自动检测，3 秒缓存

**决策：** server.cjs 启动时自动检测 `/tmp/unified_capture.sock` 是否存在且响应 status 命令。结果缓存 3 秒。

**理由：**
- 两套系统（unified_capture vs 旧 stereo daemon + guidaview）共享同一组 USB 设备 (1bcf:2d50/2d51)，物理上不能同时运行
- 自动检测避免需要用户手动配置

### 1.6 文件结构映射

**决策：** unified_capture 的 session_NNN 目录树映射到前端 Recording 类型。

**映射关系：**

| Recording 字段 | unified_capture 判据 | 值 |
|---------------|---------------------|-----|
| hasColor | 任意 camera 子目录存在 .mkv 文件 | true/false |
| hasStereo | 同 hasColor（多路 = stereo-like） | hasColor |
| hasImu | 任意子目录存在 *_imu.jsonl | true/false |
| hasDepth | N/A（无深度相机） | false |
| hasGlove | N/A（手套独立管理） | false |
| hasAudio | N/A（无麦克风） | false |
| needsDecode | IMU 已是 JSONL 实时解码 | false |
| decoded | 始终为 true | true |

---

## 二、unified_capture 改动详情

### 2.1 main.cpp — Socket 协议扩展

**文件:** `unified_capture/main.cpp`

**新增 Include:**
```cpp
#include <mutex>  // std::mutex, std::lock_guard
```

**新增全局变量（第 47-50 行）:**
```cpp
// Preview JPEG export (no extra thread — flags are polled in sensor collect loops)
std::atomic<bool> g_preview_pending{false};
std::string g_preview_path;
std::mutex g_preview_mutex;
```

**新增 Socket 命令处理（socket_handle_client 函数内）:**

在 `stop` 和 `status` 命令之间插入 `preview:<path>` 命令：

```cpp
} else if (!strncmp(buf, "preview:", 8)) {
    const char* path = buf + 8;
    if (!g_session_running) {
        resp = "{\"ok\":false,\"error\":\"not running\"}";
    } else {
        std::lock_guard<std::mutex> lock(g_preview_mutex);
        g_preview_path = path;
        g_preview_pending = true;
        resp = "{\"ok\":true}";
    }
```

**命令语义:** 设置 `g_preview_pending` 标志和 `g_preview_path` 目标路径。实际的 JPEG 生成由 VideoSensor/SixCamSensor 的 collect() 循环在下一次 BGR 解码成功时完成。命令立即返回 `{"ok":true}`，不等待 JPEG 写入完成。

**线程安全设计:**
- g_preview_mutex 保护 g_preview_path 的读写
- g_preview_pending 是 std::atomic<bool>，使用默认 seq_cst 内存序
- 主线程（socket handler）写入标志 → collect 线程读取标志 → 采集线程内完成 JPEG 编码
- 全程不创建新 pthread，不干扰 TSTC SDK / MPP 初始化

### 2.2 video_sensor.h — VideoSensor JPEG 预览导出

**文件:** `unified_capture/video_sensor.h`

**新增 Extern 声明（第 48-51 行）:**
```cpp
// Preview JPEG export globals (defined in main.cpp)
extern std::atomic<bool> g_preview_pending;
extern std::string g_preview_path;
extern std::mutex g_preview_mutex;
```

**新增预览导出代码（collect() 函数内，BGR 解码成功后）:**

插入位置：在 `bgr_to_nv12()` 调用和 Y8 写入之后，`dec_ret == 0` 检查块内部。

完整流程：
1. **检查标志:** `if (g_preview_pending.load())` — 无等待，快速路径
2. **获取锁:** `std::lock_guard<std::mutex> lock(g_preview_mutex)`
3. **双重检查:** `if (g_preview_pending.load())` — 防止多个 VideoSensor 竞争
4. **下采样:** 最近邻，宽高各 ÷4，`pw = w/4, ph = h/4`，最小 1px
5. **分配缓冲区:** `std::vector<uint8_t> scaled(pw * ph * 3)` — 约 864KB (960×300×3)
6. **逐像素拷贝:** 从 `bgr` 缓冲区取 4x4 块左上角，写入 `scaled`
7. **JPEG 压缩:** `tjCompress2(handle, scaled.data(), pw, 0, ph, TJPF_BGR, &jpg_buf, &jpg_size, TJSAMP_420, 85, TJFLAG_FASTDCT)`
8. **原子写入:** 先写 `.tmp` 临时文件，再 `rename()` 到目标路径（防止读半截文件）
9. **清理:** `tjFree(jpg_buf)`, `tjDestroy(handle)`
10. **清除标志:** `g_preview_pending = false`

**性能考量:**
- 下采样：960×300 = 288,000 次循环迭代，每次 3 字节拷贝，约 1-2ms
- JPEG 压缩：实测 tjCompress2 <5ms（960×300 BGR → JPEG）
- 总增加耗时：<10ms，30fps 帧间隔 33ms，不会导致丢帧

### 2.3 sixcam_sensor.h — SixCamSensor JPEG 预览导出

**文件:** `unified_capture/sixcam_sensor.h`

**新增 Extern 声明（第 43-46 行）:**

与 video_sensor.h 相同的三个 extern 声明。

**新增预览导出代码（collect_channel() 函数内）:**

与 VideoSensor 相同逻辑，区别：
- 仅彩色通道（`ch.output_h265 == true`）才导出预览帧
- jhh04（四目，仅 Y8）不导出，jhh02（双目，H.265+Y8）导出

### 2.4 unified_capture.service — 部署配置

**文件:** `unified_capture/unified_capture.service`

**变更:** `RestartSec=5` → `RestartSec=2`

**理由:** session 间进程退出重启窗口从 5 秒缩短到 2 秒，减少前端不可用时间。

---

## 三、device-ui 后端 (server.cjs) 改动详情

### 3.1 新增 captureCtl() 和 captureActive() — Socket 适配层

**文件:** `device-ui/server.cjs`（第 988-1018 行）

**captureCtl(cmd, timeoutMs=12000):**

与现有 `stereoCtl()` 函数完全相同的短连接模式：
1. `net.connect(CAPTURE_SOCK)` — 连接 `/tmp/unified_capture.sock`
2. 写入 `cmd + '\n'`
3. 等待 `end` 事件
4. `JSON.parse(buf.trim())` 解析响应
5. 超时/错误时返回 `{ok: false, error: '...'}`

**关键差异:** 不暴露底层错误消息（`error: 'unreachable'` 而不是 `'unreachable:' + e.message`），因为 unified_capture socket 错误不需要透传给前端。

**captureActive():**

带 3 秒缓存的模式检测：
```js
let _captureCache = { ts: 0, active: false };
async function captureActive() {
  const now = Date.now();
  if (now - _captureCache.ts < 3000) return _captureCache.active;
  try {
    const r = await captureCtl('status', 2000);
    _captureCache = { ts: now, active: !!(r && r.ok) };
  } catch { _captureCache = { ts: now, active: false }; }
  return _captureCache.active;
}
```

**缓存理由:** API 轮询频繁（前端每 3 秒调 `/api/status` 和 `/api/record/status`），避免每次请求都走 Unix socket。

### 3.2 apiRecordToggle — 录制启停

**文件:** `device-ui/server.cjs` — `apiRecordToggle()` 函数

**新增 unified_capture 分支（函数最顶部）:**

```js
if (await captureActive()) {
    if (_recBusy) return json(res, { ok: false, busy: true });
    _recBusy = true;
    try {
      const st = await captureCtl('status', 5000);
      if (!st.ok) return json(res, { ok: false, error: 'capture unreachable' });
      if (st.running) {
        const r = await captureCtl('stop', 15000);
        _maybeSkipPostCapture();
        return json(res, { ok: !!r.ok, recording: false, elapsed_ms: r.elapsed_ms || 0 });
      } else {
        const r = await captureCtl('start', 15000);
        if (!r.ok) return json(res, { ok: false, error: r.error || 'start failed' });
        return json(res, { ok: true, recording: true });
      }
    } finally { _recBusy = false; }
}
```

**流程:**
1. 检测 captureActive → true 则走新分支
2. 先查状态 → running? 调 stop : 调 start
3. stop 时调用 `_maybeSkipPostCapture()` 写 `.skip_postprocess` 标记
4. 返回 `recording: true/false`，前端据此切换 UI 状态
5. `_recBusy` 串行化防止并发 toggle 请求

**保留旧代码:** 原有的 stereoActive() 和 guidaview/pressRecordButton 逻辑完整保留作为 else 分支。

### 3.3 getRecordStatus — 录制状态查询

**文件:** `device-ui/server.cjs` — `getRecordStatus()` 函数

**新增 unified_capture 分支（stereoActive 检查之前）:**

```js
if (await captureActive()) {
    const st = await captureCtl('status', 3000);
    const sCam = !!(st && st.ok && st.ready);
    const sRec = !!(st && st.ok && st.running);
    return {
      cameraConnected: sCam, cameraType: 'stereo', gloveConnected, gloveSides,
      micConnected: mic.connected, micName: mic.name,
      recording: sRec, previewing: sCam && !sRec && _stereoPreview,
      guidaviewReady: sCam && !sRec,
      currentDir: (st && st.session) || '', stereo: true,
      cameras: (st && st.cameras) || {},
      imu: !!(st && st.imu), as5600: !!(st && st.as5600), vive: !!(st && st.vive),
    };
}
```

**字段映射:**
- `cameraConnected` ← `st.ready`（设备扫描完成）
- `recording` ← `st.running`
- `cameraType` ← 固定 `'stereo'`（前端用此显示多路标记）
- `cameras` ← `st.cameras`（如 `{jhh2_left: true, jhh2_right: false}`）
- `previewing` ← `sCam && !sRec && _stereoPreview`（相机就绪 + 未录制 + 预览已 armed）

### 3.4 apiLiveStart / apiLiveStop — 实时预览

**文件:** `device-ui/server.cjs`

**apiLiveStart 新增:**
```js
if (await captureActive()) { _stereoPreview = true; return json(res, { ok: true, capture: true }); }
```

**apiLiveStop 新增:**
```js
if (await captureActive()) { _stereoPreview = false; return json(res, { ok: true, capture: true }); }
```

**设计:** 复用现有的 `_stereoPreview` 布尔标志。unified_capture 不需要 throwaway recording（与 guidaview 不同），只需要 arm/disarm 标志即可。

### 3.5 apiCameraPreview — 摄像头 JPEG 帧

**文件:** `device-ui/server.cjs`

**新增 unified_capture 分支:**

```js
if (await captureActive()) {
    if (!_stereoPreview) {
      res.writeHead(503);
      return res.end('preview not started');
    }
    const r = await captureCtl(`preview:${PREVIEW_FILE}`, 5000);
    if (r && r.ok) {
      // Wait up to 1s for the JPEG to be written (async: flag set now, JPEG on next frame)
      for (let i = 0; i < 20; i++) {
        if (fs.existsSync(PREVIEW_FILE)) {
          const data = fs.readFileSync(PREVIEW_FILE);
          res.writeHead(200, {
            'Content-Type': 'image/jpeg',
            'Content-Length': data.length,
            'Cache-Control': 'no-store'
          });
          return res.end(data);
        }
        await new Promise(r => setTimeout(r, 50));
      }
    }
    res.writeHead(503);
    return res.end('no preview available');
}
```

**重试机制:** unified_capture 的 socket handler 设标志后立即返回，实际 JPEG 在下一个 collect() 帧写入（最迟 ~33ms@30fps）。server.cjs 轮询 20 次 × 50ms = 最多等 1 秒。

### 3.6 apiFiles — 文件列表

**文件:** `device-ui/server.cjs`

**新增 scanCaptureSessions() 函数（~90 行）:**

扫描 `/data/capture/session_NNN/` 目录树：
1. `fs.readdirSync(CAPTURE_DATA_DIR)` → 过滤 `session_` 前缀
2. 递归 `du()` 计算总大小
3. 检测 `hasColor`：任意子目录存在 `.mkv`
4. 检测 `hasImu`：任意子目录存在 `*_imu.jsonl`
5. 检测 `hasEncoder`：顶层存在 `encoder.jsonl`
6. 检测 `hasTracker`：顶层存在 `tracker.jsonl`
7. 映射到前端 `Recording` 类型（`decoded: true, needsDecode: false`）
8. 排序：`files.sort((a, b) => b.mtime - a.mtime)`

**apiFiles 新增 unified_capture 分支:**
```js
if (await captureActive()) {
    const files = scanCaptureSessions(getExternalDisk());
    return json(res, { files, root: CAPTURE_DATA_DIR, externalDisk });
}
```

### 3.7 apiFilesDelete / apiDecode / apiTransfer / apiRecordingFile — 路径适配

**所有文件操作 API 新增统一模式:**
```js
const baseDir = (await captureActive()) ? CAPTURE_DATA_DIR : RECORD_DIR;
const fp = path.join(baseDir, name);
```

**apiDecode 特殊处理:**
```js
if (await captureActive()) {
    return json(res, { ok: true, alreadyDecoded: true });
}
```
unified_capture 的 IMU 数据已是 JSONL 实时解码，不需要后处理。

### 3.8 apiStatus — 设备综合状态

**文件:** `device-ui/server.cjs`

**新增 captureStatus 字段:**
```js
let captureStatus = null;
if (await captureActive()) {
    const st = await captureCtl('status', 2000);
    if (st && st.ok) {
      captureStatus = {
        ready: !!st.ready, recording: !!st.running,
        cameras: st.cameras || {}, imu: !!st.imu,
        as5600: !!st.as5600, vive: !!st.vive,
      };
    }
}
json(res, { battery, storage, ..., captureStatus, ts: Date.now() });
```

---

## 四、device-ui 前端改动详情

### 4.1 DeviceStatus 类型扩展

**文件:** `frontend/src/services/deviceApi.ts`

```typescript
export type DeviceStatus = {
  // ... 原有字段不变 ...
  captureStatus?: {
    ready: boolean
    recording: boolean
    cameras: Record<string, boolean>
    imu: boolean
    as5600: boolean
    vive: boolean
  }
}
```

### 4.2 RecordStatus 类型扩展

```typescript
export type RecordStatus = {
  // ... 原有字段不变 ...
  cameras?: Record<string, boolean>
  imu?: boolean
  as5600?: boolean
  vive?: boolean
}
```

所有新增字段均为可选（`?:`），确保向后兼容。

### 4.3 FALLBACK_RECORD 更新

**文件:** `frontend/src/app/model.ts`

```typescript
export const FALLBACK_RECORD: RecordStatus = {
  // ... 原有字段不变 ...
  cameras: {},
  imu: false,
  as5600: false,
  vive: false,
}
```

---

## 五、部署配置改动

### 5.1 device-ui-operate.sh

**文件:** `device-ui/01-operation/device-ui-operate.sh`

**新增 unified_capture 等待逻辑:**

在启动 Node.js 后端之前：
```bash
if systemctl is-active --quiet unified_capture 2>/dev/null; then
    log_info "等待 unified_capture 就绪..."
    for i in $(seq 1 20); do
        if echo "status" | nc -U "$CAPTURE_SOCK" 2>/dev/null | grep -q '"ok":true'; then
            log_info "unified_capture 已就绪"
            break
        fi
        sleep 1
    done
fi
```

---

## 六、修 bug 记录

| # | 现象 | 根因 | 修复 | 文件 |
|---|------|------|------|------|
| 1 | tjCompress2 ret=-1 "Invalid argument" | 参数顺序错误：`tjCompress2(h, buf, pw, ph, 0, ...)` → width=pw, pitch=ph, height=0，height 为 0 非法 | 改为 `tjCompress2(h, buf, pw, 0, ph, ...)` | video_sensor.h, sixcam_sensor.h |
| 2 | tjCompress2 ret=-1 "Invalid argument" | TJSAMP_444 不被 libjpeg-turbo 2.0.6 支持 | 改为 TJSAMP_420 | video_sensor.h, sixcam_sensor.h |
| 3 | 编译错误 "declared extern and later static" | main.cpp 中 g_preview_* 声明为 static，但 header 中声明为 extern | 去掉 static | main.cpp |
| 4 | 编译警告 -Wmisleading-indentation | `if (pw < 1) pw = 1; if (ph < 1) ph = 1;` 同一行两个 if，缩进误导 | 加花括号：`if (pw < 1) { pw = 1; }` | video_sensor.h, sixcam_sensor.h |
| 5 | Preview API 首帧 503 | captureCtl 返回 ok 时 JPEG 尚未写入（下一帧才写） | 加 20×50ms 重试循环 | server.cjs |
| 6 | 磁盘满导致 rsync 失败 | 测试录制累积 34GB（24 个 session） | rm -rf /data/capture/session_* | 设备运维 |

---

## 七、测试结果（RK3588 实机）

测试环境：LubanCat-4 V1 (RK3588), Linux 5.10.160, 仅 jhh2_left 摄像头连接

| # | API | 方法 | 结果 | 关键数据 |
|---|-----|------|------|---------|
| 1 | /api/record/status | GET | ✅ | cameraConnected=true, cameras={jhh2_left:true} |
| 2 | /api/record/toggle | POST | ✅ | recording=true |
| 3 | /api/camera/live/start | POST | ✅ | capture=true |
| 4 | /api/camera/preview | GET | ✅ | HTTP 200, JPEG 960×300, ~30KB |
| 5 | /api/files | GET | ✅ | session_001, 189MB, hasColor+hasImu |
| 6 | /api/status | GET | ✅ | captureStatus 字段存在 |
| 7 | /api/record/toggle | POST | ✅ | stop, elapsed_ms=5278 |

---

## 八、待后续完善

1. **多路预览切换:** 当前预览固定用 jhh2_left。后续可通过 `preview:<camera_name>:<path>` 命令支持切换预览源
2. **前端多路显示:** 前端 CameraFeed 目前只显示单路。DataScreens.tsx 中右路预留位标注"独立视频通道待接入"
3. **AS5600 / VIVE 状态上报:** main.cpp 中 `status` 命令的 as5600/vive 字段当前硬编码 false，需接入实际传感器探测
4. **session 字段:** `status` 命令的 session 字段当前硬编码 null，可改为返回当前 session 名（如 "session_025"）
