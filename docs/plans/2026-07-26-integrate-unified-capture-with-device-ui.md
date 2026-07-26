# unified_capture ↔ device-ui 对接实现计划

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** 让 device-ui 屏幕界面通过 server.cjs 控制 unified_capture 的录制启停、实时预览、文件管理，替换旧的 guidaview/stereo daemon 后端。

**Architecture:** 改动集中在 server.cjs（新增 capture 适配层）和 unified_capture（新增 preview 帧导出），前端几乎不动。使用 Unix socket 短连接通信，与现有 stereo daemon 协议风格一致。unified_capture 不改线程模型，preview 复用现有的 collect() 循环。

**Tech Stack:** C++20 (unified_capture), Node.js 零依赖 (server.cjs), React/TypeScript (前端), libjpeg-turbo (帧导出), Unix Domain Socket

---

## 架构决策

### ADR: 适配 server.cjs，不改前端

**决策:** 所有协议适配在 server.cjs 中完成，前端 deviceApi.ts 的接口签名不变。

**理由:**
- 前端所有 API 调用通过 `deviceApi.ts`，接口 clean
- 前端 `RecordStatus.recording/cameraConnected/previewing` 这些字段已经够用
- 文件列表的 `Recording` 类型字段（hasColor/hasStereo/hasImu 等）可以映射
- 前端改动如果出问题，5.5" 触摸屏现场调试困难

### ADR: 预览帧用 downscale JPEG，不传全分辨率

**决策:** 3840x1200 BGR 先缩放到 960x300 再做 JPEG 编码，而非全分辨率。

**理由:**
- 5.5" 屏幕物理分辨率 1080x1920（竖屏），横屏模式下约 1920x1080
- 预览区域只占屏幕一小部分（CameraFeed 组件 ~400x300px）
- 全分辨率 JPEG 编码会显著增加 collect() 循环耗时，影响帧率
- libjpeg-turbo 已经链接，不需要新依赖

### ADR: 模式自动检测，不支持同时运行

**决策:** server.cjs 启动时自动检测 unified_capture socket 是否存在，存在则使用新协议，否则 fallback 到旧协议。不同时支持两套系统。

**理由:**
- 两套系统共享同一组 USB 设备 (1bcf:2d50/2d51)，物理上不能同时运行
- 自动检测避免需要用户配置

---

## Phase 1: 控制链路打通 (server.cjs)

### Task 1: 新增 captureCtl() 适配函数

**Files:**
- Modify: `device-ui/server.cjs`（文件头常量区 + 新增函数）

**背景:** server.cjs 已有 `stereoCtl()` 函数（第 975-986 行），用于跟 `/tmp/stereo_ctl.sock` 通信。需要新增一个几乎相同的 `captureCtl()`，但目标是 `/tmp/unified_capture.sock`。

**Step 1: 添加常量和适配函数**

在 server.cjs 第 969 行 `STEREO_CTL_SOCK` 定义附近，添加：

```js
// ── unified_capture integration ─────────────────────────────────────────
const CAPTURE_SOCK = '/tmp/unified_capture.sock';

function captureCtl(cmd, timeoutMs = 12000) {
  return new Promise((resolve) => {
    let done = false, buf = '';
    const finish = (o) => { if (!done) { done = true; resolve(o); } };
    const c = net.connect(CAPTURE_SOCK);
    const to = setTimeout(() => { try { c.destroy(); } catch {} finish({ ok: false, error: 'timeout' }); }, timeoutMs);
    c.on('connect', () => c.write(cmd + '\n'));
    c.on('data', (d) => { buf += d.toString(); });
    c.on('end', () => { clearTimeout(to); try { finish(JSON.parse(buf.trim())); } catch { finish({ ok: false, error: 'parse', raw: buf }); } });
    c.on('error', (e) => { clearTimeout(to); finish({ ok: false, error: 'unreachable' }); });
  });
}

async function captureActive() {
  // 检测 unified_capture socket 是否存在且响应
  try {
    const r = await captureCtl('status', 2000);
    return r && r.ok;
  } catch { return false; }
}
```

**Step 2: 验证**

在设备上运行（unified_capture 已启动时）：

```bash
node -e "
const net = require('net');
const c = net.connect('/tmp/unified_capture.sock');
c.on('connect', () => c.write('status\n'));
c.on('data', d => console.log('status:', d.toString()));
"
```

Expected: `status: {"ok":true,"ready":true,"running":false,...}`

**Step 3: 提交**

```bash
cd device-ui
git add server.cjs
git commit -m "feat(server): add captureCtl() adapter for unified_capture socket"
```

---

### Task 2: 改造 apiRecordToggle — 录制启停走 unified_capture

**Files:**
- Modify: `device-ui/server.cjs` — `apiRecordToggle()` 函数（第 1184-1210 行）

**Step 1: 在函数开头插入 unified_capture 分支**

在 `async function apiRecordToggle(req, res) {` 之后、`if (_recBusy)` 之前插入：

```js
  // unified_capture mode: delegate to its socket
  if (await captureActive()) {
    if (_recBusy) return json(res, { ok: false, busy: true });
    _recBusy = true;
    try {
      const st = await captureCtl('status', 5000);
      if (!st.ok) return json(res, { ok: false, error: 'capture unreachable' });
      if (st.running) {
        const r = await captureCtl('stop', 15000);
        return json(res, { ok: true, recording: false, elapsed_ms: r.elapsed_ms || 0 });
      } else {
        const r = await captureCtl('start', 15000);
        if (!r.ok) return json(res, { ok: false, error: r.error || 'start failed' });
        return json(res, { ok: true, recording: true });
      }
    } finally { _recBusy = false; }
  }
```

**Step 2: 保留旧代码不变**（作为 else 分支）

不删除现有的 guidaview 和 stereo 逻辑——它们作为 fallback。整个函数结构变为：

```
if (captureActive) → 新逻辑
else if (stereoActive) → 旧 stereo 逻辑
else → 旧 guidaview 逻辑
```

**Step 3: 验证**

```bash
# unified_capture 运行中
echo "status" | nc -U /tmp/unified_capture.sock   # 确认 idle
curl -X POST http://localhost:8080/api/record/toggle  # 应该返回 {"ok":true,"recording":true}
echo "status" | nc -U /tmp/unified_capture.sock   # 确认 running:true
curl -X POST http://localhost:8080/api/record/toggle  # 停止
```

**Step 4: 提交**

```bash
git add server.cjs
git commit -m "feat(server): route record toggle through unified_capture socket"
```

---

### Task 3: 改造 apiRecordStatus — 录制状态查询

**Files:**
- Modify: `device-ui/server.cjs` — `apiRecordStatus()` 函数（第 1138-1140 行）和 `getRecordStatus()` 函数（第 1062-1135 行）

**Step 1: 在 getRecordStatus() 开头插入**

```js
  // unified_capture mode
  if (await captureActive()) {
    const st = await captureCtl('status', 3000);
    if (!st.ok) {
      return { cameraConnected: false, cameraType: null, gloveConnected: false,
               micConnected: false, recording: false, previewing: false,
               guidaviewReady: false, currentDir: '', stereo: false };
    }
    return {
      cameraConnected: st.ready || false,
      cameraType: 'stereo',      // 前端用 stereo 类型显示多路标记
      gloveConnected: false,     // 手套由 server.cjs 自己的 BT 逻辑管
      micConnected: false,        // unified_capture 不管麦克风
      recording: st.running || false,
      previewing: false,          // preview 状态单独追踪（后续 task 加）
      guidaviewReady: false,      // 非 guidaview 模式
      currentDir: st.session || '', // 当前 session 名（如果有）
      stereo: true,
      // 扩展字段：多路摄像头信息
      cameras: st.cameras || {},
      imu: st.imu || false,
      as5600: st.as5600 || false,
      vive: st.vive || false,
    };
  }
```

**Step 2: 验证**

```bash
curl http://localhost:8080/api/record/status | jq .
```

Expected（unified_capture idle）:
```json
{
  "cameraConnected": true,
  "cameraType": "stereo",
  "gloveConnected": false,
  "micConnected": false,
  "recording": false,
  "previewing": false,
  "guidaviewReady": false,
  "currentDir": "",
  "stereo": true,
  "cameras": {"jhh2_left": true, "jhh2_right": true, "jhh04": true, "jhh02": true},
  "imu": true,
  "as5600": false,
  "vive": false
}
```

**Step 3: 提交**

```bash
git add server.cjs
git commit -m "feat(server): query unified_capture status for record status API"
```

---

### Task 4: 改造 apiLiveStart / apiLiveStop — 实时预览启停

**Files:**
- Modify: `device-ui/server.cjs` — `apiLiveStart()`（第 1144-1163 行）和 `apiLiveStop()`（第 1167-1181 行）

**Step 1: apiLiveStart 新增 unified_capture 分支**

```js
  // unified_capture mode: just arm the preview flag
  if (await captureActive()) {
    _stereoPreview = true;   // 复用 stereo preview 标志
    return json(res, { ok: true, capture: true });
  }
```

`_stereoPreview` 已经在 server.cjs 中定义（第 1020 行），可直接复用。前端 CameraScreen 启动预览时调用 `api.startLive()`，然后以 850ms 间隔轮询 `/api/camera/preview?t=xxx`。

**Step 2: apiLiveStop 新增 unified_capture 分支**

```js
  if (await captureActive()) {
    _stereoPreview = false;
    return json(res, { ok: true, capture: true });
  }
```

**Step 3: 验证**

```bash
curl -X POST http://localhost:8080/api/camera/live/start  # → {"ok":true,"capture":true}
curl -X POST http://localhost:8080/api/camera/live/stop   # → {"ok":true,"capture":true}
```

**Step 4: 提交**

```bash
git add server.cjs
git commit -m "feat(server): wire live start/stop to unified_capture preview flag"
```

---

## Phase 2: 摄像头预览帧导出 (unified_capture + server.cjs)

### Task 5: unified_capture 新增 preview 命令

**Files:**
- Modify: `unified_capture/main.cpp` — `socket_handle_client()` 函数

**背景:** 当前只有 start/stop/status 三个命令。需要新增 `preview:<path>` 命令，告诉采集程序把下一帧 JPEG 写到指定路径。

**Step 1: 新增全局预览标志**

在 main.cpp 全局变量区（约第 37 行附近）：

```cpp
// Preview JPEG export
static std::atomic<bool> g_preview_pending{false};
static std::string g_preview_path;
static std::mutex g_preview_mutex;
```

**Step 2: 在 socket_handle_client() 中新增 preview 命令处理**

在 `socket_handle_client()` 函数中（第 222 行），在 `} else if (!strcmp(buf, "status")) {` 之前插入：

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

需要添加 `#include <mutex>` 到头文件区。

**Step 3: 在 run_session() 的主循环中处理预览请求**

在 run_session() 的 `while (g_session_running)` 循环内（第 323 行），在 poll 返回后检查预览标志。但 preview 的实际 JPEG 生成在 VideoSensor 中做（Task 6），这里只需要确认标志被正确设置。

**Step 4: 验证**

```bash
# unified_capture 录制中
echo "preview:/tmp/test_preview.jpg" | nc -U /tmp/unified_capture.sock
# → {"ok":true}
```

**Step 5: 提交**

```bash
cd unified_capture
git add main.cpp
git commit -m "feat: add preview:<path> command to socket protocol"
```

---

### Task 6: VideoSensor 新增 JPEG 预览帧导出

**Files:**
- Modify: `unified_capture/video_sensor.h` — `collect()` 函数
- Modify: `unified_capture/sixcam_sensor.h` — `collect_channel()` 函数

**背景:** 每个 VideoSensor 的 collect() 循环中已经做了 BGR 解码（用于 MPP 编码和 Y8 导出）。只需要在 BGR 解码成功后增加一步：检查是否 pending preview，如果是则 downscale + JPEG 编码 + 写盘。

**Step 1: 在 video_sensor.h 的 collect() 中添加预览导出**

找到 collect() 中 BGR 解码成功后的位置（约在 tjDecompress2 调用后），添加：

```cpp
// Preview JPEG export (if requested by socket command)
if (g_preview_pending.load(std::memory_order_acquire)) {
    std::lock_guard<std::mutex> lock(g_preview_mutex);
    if (g_preview_pending.load()) {
        // Downscale to 1/4 resolution for preview
        int pw = width_ / 4, ph = height_ / 4;
        std::vector<uint8_t> scaled(pw * ph * 3);
        // Simple nearest-neighbor downscale
        for (int y = 0; y < ph; y++) {
            for (int x = 0; x < pw; x++) {
                int sx = x * 4, sy = y * 4;
                uint8_t* src = bgr_buf_.data() + (sy * width_ + sx) * 3;
                uint8_t* dst = scaled.data() + (y * pw + x) * 3;
                dst[0] = src[0]; dst[1] = src[1]; dst[2] = src[2];
            }
        }
        // JPEG compress
        unsigned long jpg_size = 0;
        uint8_t* jpg_buf = nullptr;
        tjhandle jpg = tjInitCompress();
        tjCompress2(jpg, scaled.data(), pw, ph, 0, TJPF_BGR,
                    &jpg_buf, &jpg_size, TJSAMP_444, 85,
                    TJFLAG_FASTDCT);
        // Write atomically: write to temp, then rename
        std::string tmp_path = g_preview_path + ".tmp";
        FILE* f = fopen(tmp_path.c_str(), "wb");
        if (f) { fwrite(jpg_buf, 1, jpg_size, f); fclose(f); }
        rename(tmp_path.c_str(), g_preview_path.c_str());
        tjFree(jpg_buf);
        tjDestroy(jpg);
        g_preview_pending = false;
    }
}
```

**Step 2: sixcam_sensor.h 同样添加**

SixCamSensor 有两个通道（jhh04 和 jhh02），只需要其中一个做预览。选 jhh02（它是彩色通道），在 `collect_channel(1)` 的 BGR 解码后添加相同逻辑。

**Step 3: 验证**

```bash
# unified_capture 录制中
echo "preview:/tmp/test_preview.jpg" | nc -U /tmp/unified_capture.sock
sleep 0.2
file /tmp/test_preview.jpg   # → JPEG image data
identify /tmp/test_preview.jpg  # → 应该显示缩小后的分辨率
```

**Step 4: 提交**

```bash
git add video_sensor.h sixcam_sensor.h
git commit -m "feat: export downscaled JPEG preview frame on demand"
```

---

### Task 7: server.cjs 改造 apiCameraPreview 走 unified_capture

**Files:**
- Modify: `device-ui/server.cjs` — `apiCameraPreview()` 函数（第 1231-1256 行）

**Step 1: 在函数开头插入 unified_capture 分支**

```js
  // unified_capture mode: request a fresh frame from the capture daemon
  if (await captureActive()) {
    if (!_stereoPreview) {
      // Preview not armed — return a static "waiting" image or 503
      res.writeHead(503);
      return res.end('preview not started');
    }
    const r = await captureCtl(`preview:${PREVIEW_FILE}`, 5000);
    if (r && r.ok && fs.existsSync(PREVIEW_FILE)) {
      const data = fs.readFileSync(PREVIEW_FILE);
      res.writeHead(200, {
        'Content-Type': 'image/jpeg',
        'Content-Length': data.length,
        'Cache-Control': 'no-store'
      });
      return res.end(data);
    }
    res.writeHead(503);
    return res.end('no preview available');
  }
```

**Step 2: 验证**

```bash
# unified_capture 录制中，先开 preview
curl -X POST http://localhost:8080/api/camera/live/start
# 等 1 秒
curl http://localhost:8080/api/camera/preview -o /tmp/frame.jpg
file /tmp/frame.jpg  # → JPEG image data
```

**Step 3: 提交**

```bash
git add server.cjs
git commit -m "feat(server): serve camera preview frames from unified_capture"
```

---

## Phase 3: 文件管理适配 (server.cjs)

### Task 8: 改造 apiFiles — 扫描 session_NNN 目录

**Files:**
- Modify: `device-ui/server.cjs` — `apiFiles()` 函数（约第 520-599 行）

**背景:** 旧系统扫描 `RECORD_DIR/recording_*`，新系统需要扫描 `/data/capture/session_NNN`。需要新增一个 `scanCaptureSessions()` 函数，在 capture 模式下替代旧的文件扫描。

**Step 1: 新增 scanCaptureSessions() 函数**

在 server.cjs 中添加（放在 apiFiles 函数之前）：

```js
const CAPTURE_DATA_DIR = process.env.CAPTURE_DATA_DIR || '/data/capture';

function scanCaptureSessions() {
  const files = [];
  let entries = [];
  try { entries = fs.readdirSync(CAPTURE_DATA_DIR); } catch { return files; }

  for (const name of entries) {
    if (!name.startsWith('session_')) continue;
    const sp = path.join(CAPTURE_DATA_DIR, name);
    let st;
    try { st = fs.statSync(sp); } catch { continue; }
    if (!st.isDirectory()) continue;

    // Calculate total size
    let totalSize = 0;
    function du(dir) {
      try {
        for (const f of fs.readdirSync(dir)) {
          const fp = path.join(dir, f);
          const s = fs.statSync(fp);
          if (s.isDirectory()) du(fp);
          else totalSize += s.size;
        }
      } catch {}
    }
    du(sp);

    // Detect content types from subdirectories
    const subDirs = fs.readdirSync(sp).filter(f => {
      try { return fs.statSync(path.join(sp, f)).isDirectory(); } catch { return false; }
    });

    const hasColor = subDirs.some(d => {
      const sub = fs.readdirSync(path.join(sp, d));
      return sub.some(f => f.endsWith('.mkv'));
    });
    const hasImu = subDirs.some(d => {
      try {
        const sub = fs.readdirSync(path.join(sp, d));
        return sub.some(f => f.endsWith('_imu.jsonl') || f.endsWith('imu.jsonl'));
      } catch { return false; }
    });
    // Non-video sensors
    const topFiles = fs.readdirSync(sp).filter(f => {
      try { return fs.statSync(path.join(sp, f)).isFile(); } catch { return false; }
    });
    const hasEncoder = topFiles.some(f => f === 'encoder.jsonl');
    const hasTracker = topFiles.some(f => f === 'tracker.jsonl');

    // Mapping: unified_capture flags → frontend Recording type
    files.push({
      name,
      size: totalSize,
      mtime: st.mtimeMs,
      hasColor,
      hasDepth: false,        // no depth camera in unified_capture
      hasGlove: false,        // gloves managed separately
      hasImu,
      hasStereo: hasColor,    // unified_capture multi-cam = stereo-like
      hasAudio: false,        // no mic in unified_capture
      decoded: true,          // IMU is always decoded (JSONL inline)
      decoding: false,
      needsDecode: false,
      transferring: false,
      transferred: false,
      transferPct: 0,
      // Extended info (non-standard, for future use)
      hasEncoder,
      hasTracker,
      cameraCount: subDirs.length,
    });
  }

  // Sort newest first
  files.sort((a, b) => b.mtime - a.mtime);
  return files;
}
```

**Step 2: 在 apiFiles() 开头插入 unified_capture 分支**

```js
  // unified_capture mode: scan session dirs
  if (await captureActive()) {
    const files = scanCaptureSessions();
    const externalDisk = await detectExternalDisk();
    return json(res, { files, root: CAPTURE_DATA_DIR, externalDisk });
  }
```

**Step 3: 验证**

```bash
# 创建测试数据
mkdir -p /data/capture/session_001/jhh2_left
touch /data/capture/session_001/jhh2_left/001.mkv
touch /data/capture/session_001/jhh2_left/001_imu.jsonl

curl http://localhost:8080/api/files | jq .
```

Expected:
```json
{
  "files": [{
    "name": "session_001",
    "size": 0,
    "hasColor": true,
    "hasImu": true,
    "hasStereo": true,
    "decoded": true,
    "needsDecode": false,
    ...
  }],
  "root": "/data/capture",
  "externalDisk": null
}
```

**Step 4: 提交**

```bash
git add server.cjs
git commit -m "feat(server): scan unified_capture session dirs for file list"
```

---

### Task 9: 适配文件操作 API — decode / transfer / preview / delete

**Files:**
- Modify: `device-ui/server.cjs` — 多个函数

**Step 1: apiDecode（IMU 解码）— 改为 no-op**

unified_capture 的 IMU 数据已经是 JSONL（实时解码），不再需要后处理解码。修改 `apiDecode` 函数（约第 621 行）：

```js
  // unified_capture: IMU is already decoded (JSONL), no post-processing needed
  if (await captureActive()) {
    return json(res, { ok: true, alreadyDecoded: true });
  }
```

**Step 2: apiTransfer（传输到 USB）— 适配源路径**

修改 `apiTransfer` 函数（约第 728 行），将源路径从 `RECORD_DIR/recName` 改为 `CAPTURE_DATA_DIR/recName`：

```js
  const srcDir = (await captureActive())
    ? path.join(CAPTURE_DATA_DIR, recName)
    : path.join(RECORD_DIR, recName);
```

**Step 3: recording preview（H.264 预览视频）— 适配 MKV 路径**

修改 recording preview 处理（约第 767 行），找到 MKV 文件的新位置：

```js
  // unified_capture: MKVs are in subdirectories
  if (await captureActive()) {
    let mkvPath = null;
    for (const sub of fs.readdirSync(srcDir)) {
      const subp = path.join(srcDir, sub);
      if (!fs.statSync(subp).isDirectory()) continue;
      const mkvs = fs.readdirSync(subp).filter(f => f.endsWith('.mkv'));
      if (mkvs.length > 0) {
        mkvPath = path.join(subp, mkvs[0]);
        break;
      }
    }
    if (!mkvPath) return json(res, { ok: false, error: 'no mkv found' }, 404);
    // ... 后续的 FFmpeg 转码逻辑不变，只是输入路径变了
  }
```

**Step 4: apiFilesDelete — 适配删除路径**

```js
  const fp = (await captureActive())
    ? path.join(CAPTURE_DATA_DIR, name)
    : path.join(RECORD_DIR, name);
```

**Step 5: 验证**

```bash
# 创建测试 session
mkdir -p /data/capture/session_test/jhh2_left
touch /data/capture/session_test/jhh2_left/001.mkv

# 测试删除
curl -X DELETE http://localhost:8080/api/files/session_test
# → {"ok":true}

# 确认已删除
ls /data/capture/session_test 2>&1  # → No such file or directory
```

**Step 6: 提交**

```bash
git add server.cjs
git commit -m "feat(server): adapt decode/transfer/preview/delete for unified_capture paths"
```

---

### Task 10: 改造 apiStatus — 设备综合状态

**Files:**
- Modify: `device-ui/server.cjs` — `apiStatus()` 函数（第 255 行附近）

**Step 1: 在 apiStatus 中混入 unified_capture 摄像头信息**

```js
  // If unified_capture is active, include its camera status
  let captureInfo = null;
  if (await captureActive()) {
    try {
      const st = await captureCtl('status', 2000);
      if (st && st.ok) {
        captureInfo = {
          cameras: st.cameras || {},
          recording: st.running || false,
          imu: st.imu || false,
          as5600: st.as5600 || false,
          vive: st.vive || false,
        };
      }
    } catch {}
  }
  // ... merge captureInfo into the status response
```

**Step 2: 验证**

```bash
curl http://localhost:8080/api/status | jq .
```

**Step 3: 提交**

```bash
git add server.cjs
git commit -m "feat(server): include unified_capture status in device status API"
```

---

## Phase 4: 前端微调 (frontend)

### Task 11: 前端类型对齐 — deviceApi.ts

**Files:**
- Modify: `device-ui/frontend/src/services/deviceApi.ts`
- Modify: `device-ui/frontend/src/app/model.ts`

**背景:** 后端 RecordStatus 新增了 `cameras`/`imu`/`as5600`/`vive` 字段，前端类型需要同步。但这些都是可选扩展，不改也不影响编译。

**Step 1: 扩展 RecordStatus 类型**

```typescript
export type RecordStatus = {
  cameraConnected: boolean
  cameraType?: 'stereo' | 'depth' | null
  gloveConnected: boolean
  gloveSides?: { left?: boolean; right?: boolean }
  micConnected?: boolean
  micName?: string
  recording: boolean
  previewing: boolean
  guidaviewReady: boolean
  currentDir?: string
  stereo?: boolean
  // unified_capture extensions
  cameras?: Record<string, boolean>
  imu?: boolean
  as5600?: boolean
  vive?: boolean
}
```

**Step 2: 扩展 Recording 类型**

```typescript
export type Recording = {
  // ... existing fields
  // unified_capture extensions
  hasEncoder?: boolean
  hasTracker?: boolean
  cameraCount?: number
}
```

**Step 3: 更新 model.ts 中的 fallback 值**

```typescript
export const FALLBACK_RECORD: RecordStatus = {
  // ... existing fields
  cameras: {},
  imu: false,
  as5600: false,
  vive: false,
};
```

**Step 4: 构建验证**

```bash
cd frontend
pnpm build
```

Expected: TypeScript 编译无错误，Vite 构建成功。

**Step 5: 提交**

```bash
git add frontend/src/services/deviceApi.ts frontend/src/app/model.ts
pnpm build
git add static/
git commit -m "feat(frontend): add unified_capture type extensions to RecordStatus and Recording"
```

---

## Phase 5: 部署配置

### Task 12: unified_capture systemd unit

**Files:**
- Create: `unified_capture/unified_capture.service`（已存在，确认配置）

**Step 1: 确认 service 文件配置**

当前 `unified_capture.service` 应该使用 `--socket --single` 模式：

```ini
[Unit]
Description=Unified Capture Service (Multi-Camera)
After=multi-user.target

[Service]
Type=simple
ExecStart=/usr/local/bin/unified_capture --socket --single --no-vive /data/capture
ExecStop=/bin/sh -c 'echo stop | nc -U /tmp/unified_capture.sock'
Restart=always
RestartSec=2
User=root
StandardOutput=journal
StandardError=journal

[Install]
WantedBy=multi-user.target
```

关键：`--single` 确保每次 session 后进程退出，`Restart=always` + `RestartSec=2` 让 systemd 自动重启。

**Step 2: 验证**

```bash
systemctl start unified_capture
systemctl status unified_capture
echo "status" | nc -U /tmp/unified_capture.sock  # → {"ok":true,...}
echo "start" | nc -U /tmp/unified_capture.sock   # → 开始录制
echo "stop" | nc -U /tmp/unified_capture.sock    # → 停止，进程退出
sleep 3
echo "status" | nc -U /tmp/unified_capture.sock  # → 自动重启成功
```

**Step 3: 提交**

```bash
git add unified_capture.service
git commit -m "feat: systemd unit with --single mode for reliable restart"
```

---

### Task 13: device-ui 启动脚本更新

**Files:**
- Modify: `device-ui/01-operation/device-ui-operate.sh`

**Step 1: 添加 unified_capture 依赖检查**

在启动 server.cjs 之前，检查 unified_capture socket 是否就绪：

```bash
# Wait for unified_capture to be ready
echo "Waiting for unified_capture..."
for i in $(seq 1 20); do
    if echo "status" | nc -U /tmp/unified_capture.sock 2>/dev/null | grep -q '"ok":true'; then
        echo "unified_capture ready"
        break
    fi
    sleep 1
done
```

**Step 2: 提交**

```bash
git add 01-operation/device-ui-operate.sh
git commit -m "feat: wait for unified_capture socket before starting UI"
```

---

### Task 14: 端到端集成测试

**Step 1: 测试准备**

```bash
# 确保两服务都运行
systemctl start unified_capture
PORT=8080 RECORD_DIR=/mnt/ums/records CAPTURE_DATA_DIR=/data/capture node server.cjs &
sleep 2
```

**Step 2: 测试控制链路**

```bash
# 1. 状态检查
curl -s http://localhost:8080/api/record/status | jq .cameraConnected  # → true
curl -s http://localhost:8080/api/status | jq .recordings

# 2. 开始录制
curl -s -X POST http://localhost:8080/api/record/toggle | jq .recording  # → true
sleep 2
curl -s http://localhost:8080/api/record/status | jq .recording  # → true

# 3. 停止录制
curl -s -X POST http://localhost:8080/api/record/toggle | jq .recording  # → false

# 4. 文件列表
curl -s http://localhost:8080/api/files | jq '.files | length'  # → ≥1
```

**Step 3: 测试预览链路**

```bash
# 开始预览
curl -s -X POST http://localhost:8080/api/camera/live/start
# 等 1 秒让 unified_capture 产生一帧
sleep 1
# 拉预览帧
curl -s http://localhost:8080/api/camera/preview -o /tmp/preview_test.jpg
file /tmp/preview_test.jpg  # → JPEG image data
# 停止预览
curl -s -X POST http://localhost:8080/api/camera/live/stop
```

**Step 4: 测试 session 重启循环**

```bash
# 记录当前 session 号
echo "status" | nc -U /tmp/unified_capture.sock | jq .
# 开始 → 停止（触发进程退出）
echo "start" | nc -U /tmp/unified_capture.sock
sleep 2
echo "stop" | nc -U /tmp/unified_capture.sock
# 等 3 秒重启
sleep 3
echo "status" | nc -U /tmp/unified_capture.sock | jq .ready  # → true
```

**Step 5: 浏览器端测试**

在设备 Chromium Kiosk 模式下打开 `http://localhost:8080`：
- 主页加载正常
- 切换到"数据"tab → 相机状态显示"已连接"
- 点击"开始预览" → CameraFeed 显示实时画面
- 点击"开始录制" → 红色录制状态灯亮起，计时器运行
- 点击"停止录制" → 状态恢复
- 切换到"记录"tab → 显示刚才的 session
- 点击记录 → 详情 overlay 显示标签（彩色/IMU）

**Step 6: 提交（如果有修复）**

```bash
git add -A
git commit -m "test: end-to-end integration tests pass"
```

---

## 风险与缓解

| 风险 | 影响 | 缓解措施 |
|------|------|---------|
| 3840x1200 downscale+JPEG 编码耗时影响帧率 | 丢帧 | 用 TJFLAG_FASTDCT + 降低 JPEG quality，监控 collect() 循环耗时 |
| session 间 3-5s 不可用 → API 返回错误 | 前端显示离线 | server.cjs 在 capture 不可用时返回友好状态 + 前端已有离线处理 |
| server.cjs 同时有人改（git 冲突） | 合并冲突 | Task 1-10 的改动集中在独立的 capture 分支区域，不改变旧代码行 |

## 不在此计划范围内的功能

以下功能保留为旧系统能力（手套/音频/深度相机），unified_capture 不接管：

- 蓝牙手套连接/校准（server.cjs 继续管）
- 麦克风采集（server.cjs + guidaview）
- Orbbec 深度相机（guidaview 模式）
- WiFi 管理（server.cjs 独立处理）
- 设备商城/任务平台（前端占位，后端都未接入）

---

## 文件改动汇总

| 文件 | 改动类型 | Phase |
|------|---------|-------|
| `device-ui/server.cjs` | 新增 ~200 行，修改 ~10 处 | 1, 2, 3 |
| `unified_capture/main.cpp` | 新增 ~30 行 | 2 |
| `unified_capture/video_sensor.h` | 新增 ~40 行 | 2 |
| `unified_capture/sixcam_sensor.h` | 新增 ~40 行 | 2 |
| `device-ui/frontend/src/services/deviceApi.ts` | 新增 8 行 | 4 |
| `device-ui/frontend/src/app/model.ts` | 新增 5 行 | 4 |
| `unified_capture/unified_capture.service` | 确认/修改 | 5 |
| `device-ui/01-operation/device-ui-operate.sh` | 新增 8 行 | 5 |
