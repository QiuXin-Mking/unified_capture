# MPP H.265 编码 SIGSEGV 深入诊断计划

> **For Claude:** 在 RK3588 板端逐 Task 执行，每步确认结果后再进行下一步。

**Goal:** 定位并修复 MPP H.265 编码在 `mpp_.put()` 中 SIGSEGV 的根因

**Architecture:** 渐进式诊断——先加错误检查定位精确崩溃点，再按可能性从高到低尝试修复方案

**Tech Stack:** Rockchip MPP, RK3588, DRM/ION buffer allocator

**已知前提：**
- `--no-h265` 模式 4 路全部正常（Y8 数据产出正常）
- CMA 已扩到 128M（修复了 `mpp_buffer_group_init:618` 断言）
- 崩溃发生在第一个 `mpp_.put()` 调用，3 个编码器 (jhh2_left/right + jhh02) 都是

---

### Task 1: 添加 MPP 调用返回值检查 + 崩溃点精确定位

**Files:**
- Modify: `mpp_encoder.h:77-105` (put 方法)
- Modify: `mpp_encoder.h:22-74` (init 方法)

**Step 1: 在 init() 中添加更多诊断日志**

在 `mpp_encoder.h` 的 `init()` 末尾，`return true` 之前，添加：

```cpp
// 诊断: 检查 buf_group 是否真的可用
fprintf(stderr, "[MPP] init OK: w=%u h=%u frame_size=%u buf_group=%p ctx=%p\n",
        w, h, frame_size, (void*)buf_group, (void*)ctx);
fflush(stderr);
```

**Step 2: 在 put() 中添加完整的错误检查**

将 `mpp_encoder.h:88-93` 替换为：

```cpp
MppBuffer buf = nullptr;
MPP_RET ret = mpp_buffer_get(buf_group, &buf, frame_size);
if (ret != MPP_OK || !buf) {
    fprintf(stderr, "[MPP] mpp_buffer_get FAILED: ret=%d buf=%p frame_size=%u\n",
            ret, (void*)buf, frame_size);
    fflush(stderr);
    mpp_frame_deinit(&frame);
    return 0;
}
fprintf(stderr, "[MPP] mpp_buffer_get OK: buf=%p\n", (void*)buf);
fflush(stderr);

void* ptr = mpp_buffer_get_ptr(buf);
if (!ptr) {
    fprintf(stderr, "[MPP] mpp_buffer_get_ptr returned NULL! buf=%p\n", (void*)buf);
    fflush(stderr);
    mpp_buffer_put(buf);
    mpp_frame_deinit(&frame);
    return 0;
}
fprintf(stderr, "[MPP] mpp_buffer_get_ptr OK: ptr=%p\n", ptr);
fflush(stderr);

memcpy(ptr, nv12, frame_size);
fprintf(stderr, "[MPP] memcpy done\n");
fflush(stderr);

mpp_frame_set_buffer(frame, buf);
fprintf(stderr, "[MPP] encode_put_frame calling...\n");
fflush(stderr);

ret = mpi->encode_put_frame(ctx, frame);
fprintf(stderr, "[MPP] encode_put_frame ret=%d\n", ret);
fflush(stderr);
mpp_frame_deinit(&frame);
```

**Step 3: 编译并测试**

在板端运行:
```bash
cd ~/unified_capture
make
# 只测 1 路 H.265 (最小复现)
./unified_capture --socket --no-vive --no-imu --no-as5600 /tmp/test_mpp1 2>&1 | grep -E '\[MPP\]|\[jhh2'
# 另开终端:
echo start | nc -U /tmp/unified_capture.sock
# 等 5 秒:
echo stop | nc -U /tmp/unified_capture.sock
```

**预期:** 日志会精确显示是在 `mpp_buffer_get`、`mpp_buffer_get_ptr`、还是 `encode_put_frame` 崩溃。

---

### Task 2: 修复尝试 #1 — MPP_BUFFER_INTERNAL → MPP_BUFFER_EXTERNAL

**Files:**
- Modify: `mpp_encoder.h:70-71`

**Step 1: 修改 buffer group 类型**

```cpp
// 改前:
ret = mpp_buffer_group_get(&buf_group, MPP_BUFFER_TYPE_DRM,
                           MPP_BUFFER_INTERNAL, "he", NULL);
// 改后:
ret = mpp_buffer_group_get(&buf_group, MPP_BUFFER_TYPE_DRM,
                           MPP_BUFFER_EXTERNAL, "he", NULL);
```

**Step 2: 编译并测试**

只测 1 路 H.265:
```bash
make
./unified_capture --socket --no-vive --no-imu --no-as5600 /tmp/test_mpp2 2>&1 | grep -E '\[MPP\]|\[jhh2'
# 另开终端:
echo start | nc -U /tmp/unified_capture.sock
sleep 5
echo stop | nc -U /tmp/unified_capture.sock
```

**预期:** `mpp_buffer_get` 成功返回，`encode_put_frame` 正常工作，产出 .mkv 文件。

---

### Task 3: 修复尝试 #2 — 添加 MPP_ENC_SET_CFG 最终化步骤

> 如果 Task 2 修复已生效，跳过此 Task。

**Files:**
- Modify: `mpp_encoder.h:68-72` (在 buffer_group_get 之前插入)

**Step 1: 添加配置最终化**

在 `mpp_encoder.h:68` (buffer_group_get 之前) 插入：

```cpp
// ★ 最终化编码器配置 (触发内部资源分配)
MppEncCfg cfg;
ret = mpp_enc_cfg_init(&cfg);
if (ret != MPP_OK) { fprintf(stderr, "mpp_enc_cfg_init failed\n"); return false; }
ret = mpi->control(ctx, MPP_ENC_GET_CFG, cfg);
if (ret != MPP_OK) { fprintf(stderr, "MPP_ENC_GET_CFG failed\n"); mpp_enc_cfg_deinit(cfg); return false; }
ret = mpi->control(ctx, MPP_ENC_SET_CFG, cfg);
if (ret != MPP_OK) { fprintf(stderr, "MPP_ENC_SET_CFG failed\n"); mpp_enc_cfg_deinit(cfg); return false; }
mpp_enc_cfg_deinit(cfg);
```

**Step 2: 编译并测试**
```bash
make
./unified_capture --socket --no-vive --no-imu --no-as5600 /tmp/test_mpp3 2>&1 | grep -E '\[MPP\]|\[jhh2'
echo start | nc -U /tmp/unified_capture.sock
sleep 5
echo stop | nc -U /tmp/unified_capture.sock
```

---

### Task 4: 修复尝试 #3 — MPP_BUFFER_TYPE_ION

> 如果 Task 2 或 3 已生效，跳过此 Task。

**Files:**
- Modify: `mpp_encoder.h:70`

**Step 1: 切换为 ION 分配器**

```cpp
// 改前:
ret = mpp_buffer_group_get(&buf_group, MPP_BUFFER_TYPE_DRM, ...
// 改后:
ret = mpp_buffer_group_get(&buf_group, MPP_BUFFER_TYPE_ION, ...
```

**Step 2: 编译并测试**
```bash
make
./unified_capture --socket --no-vive --no-imu --no-as5600 /tmp/test_mpp4 2>&1 | grep -E '\[MPP\]|\[jhh2'
echo start | nc -U /tmp/unified_capture.sock
sleep 5
echo stop | nc -U /tmp/unified_capture.sock
```

---

### Task 5: 多路全开验证

> 单路修复确认生效后再执行。

**Step 1: 3 路 H.265 同时开启**
```bash
make
./unified_capture --socket --no-vive --no-imu --no-as5600 /tmp/test_mpp_full 2>&1
echo start | nc -U /tmp/unified_capture.sock
sleep 10
echo stop | nc -U /tmp/unified_capture.sock
```

**验证标准:**
- 无 SIGSEGV / 无 journalctl 报 fatal
- jhh2_left 目录有 001.mkv（非空）
- jhh2_right 目录有 001.mkv（非空）
- jhh02 目录有 001.mkv（非空）
- 所有 .y8 文件也存在且非空

---

### Task 6: 清理诊断日志 + 提交

确认修复后，把 Task 1 添加的大量诊断 `fprintf` 精简，只保留必要的错误检查（`mpp_buffer_get` 返回值检查 + NULL 保护），删除逐行的调试日志。

```bash
git add mpp_encoder.h
git commit -m "fix: MPP H.265 SIGSEGV in put() - fix buffer allocation strategy"
```
