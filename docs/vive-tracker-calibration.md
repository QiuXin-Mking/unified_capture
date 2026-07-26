# VIVE Tracker 3.0 基站标定

> 2026-07-26

## 问题

两个 VIVE Tracker 3.0 + 两个 Gen 1 基站，libsurvive 只有 light 数据（激光扫到 sensor 的原始信号），**没有 pose（位姿）和 angle 数据**。

日志表现：
```
Info: Detected LH gen 1 system.
Info: Locked onto state 11(12, 1359583) for T20
[vive] collect done (2000 polls, 0 poses, 0 angles, 36466 lights)
```

## 原因

libsurvive 需要两类标定数据才能计算位姿：

| 数据类型 | 来源 | 耗时 | 状态 |
|----------|------|------|------|
| **OOTX**（转子相位/倾斜/曲线） | 基站激光中编码，tracker 接收解码 | ~15-20s 持续接收 | ❌ 未解码 |
| **基站 3D 位置**（x/y/z + 旋转四元数） | libsurvive solver 自动求解 | 需要 tracker 移动 | ❌ NaN |

旧 `/root/.config/libsurvive/config.json` 中：
- OOTX 数据有（`OOTXSet=1`，fcalphase/fcaltilt 等参数完整）
- 但基站位置全是 `NaN`（`pose: ["nan","nan","nan","nan","-nan","nan","-nan"]`）
- 同时 `PositionSet=1`，libsurvive 以为已标定，不会再求解

## 标定步骤

### 1. 清除旧配置，重新开始

```bash
systemctl stop unified_capture
rm -f /root/.config/libsurvive/config.json
```

### 2. 运行长 session（60-90 秒），缓慢移动 tracker

Tracker 需要被**手持缓慢移动**，让两个基站都能持续看到它。这样：
- OOTX 解码器在 ~15 秒内从激光信号中解出基站转子参数
- Solver 在 tracker 移动时求解基站 3D 位置

```bash
systemctl start unified_capture
sleep 4
echo "start" | nc -U -w2 /tmp/unified_capture.sock
# 此时手持任意一个 tracker，在空间里缓慢画圈、上下移动
sleep 90
echo "stop" | nc -U -w2 /tmp/unified_capture.sock
```

### 3. 检查标定结果

```bash
cat /root/.config/libsurvive/config.json | python3 -c "
import sys,json
d=json.load(sys.stdin)
for lh in ['lighthouse0','lighthouse1']:
    l = d[lh]
    nans = sum(1 for v in l.get('pose',[]) if 'nan' in str(v))
    print(f'{lh}: OOTXSet={l.get(\"OOTXSet\")} PositionSet={l.get(\"PositionSet\")} NaN count={nans}')
"
```

期望输出：
```
lighthouse0: OOTXSet=1 PositionSet=1 NaN count=0
lighthouse1: OOTXSet=1 PositionSet=1 NaN count=0
```

如果 `NaN count=0`，标定成功。后续 session 会立即输出 pose 数据。

### 4. 验证 pose 数据

标定完成后，跑一个短 session：

```bash
echo "start" | nc -U -w2 /tmp/unified_capture.sock
sleep 10
echo "stop" | nc -U -w2 /tmp/unified_capture.sock
# 检查 tracker 文件中有无 "x": 字段（pose 数据）
grep -c '"x":' /media/usb0/capture/session_XXX/tracker/*.jsonl
```

期望有 ~1000 条 pose 数据（100Hz × 10 秒）。

## 技术细节

### 数据流程

```
基站激光扫过 tracker sensor
    ↓
light_callback (高频, 不规则, ~500-800Hz/tracker)
    ↓ disambiguator 解析 sweep 来自哪个转子
angle_callback (需要 disambiguator 就绪)
    ↓ OOTX 解码 + solver 三角定位
pose_callback (100Hz, 需要 OOTX + 基站位置都就绪)
```

### 为什么之前没有 pose

1. **OOTX 未解码**: 配置文件中有旧的 OOTX 数据但可能格式不兼容，且 `OOTXSet=1` 阻止了重新解码
2. **基站位置 NaN**: `PositionSet=1` 让 solver 跳过求解，但位置实际是 NaN
3. **survive_init 参数**: 已改为传 `-l 2`（显式指定 2 个基站）

### 关键代码位置

- `vive_tracker.h`: `survive_init(3, dummy_argv)` — 传递 `-l 2` 参数
- `vive_tracker.h`: `pose_callback` / `angle_callback` / `light_callback` — 三种回调
- `vive_usb.h`: `unbind_all_vive_trackers()` — USB 设备 unbind
- `main.cpp`: `detect_vive_trackers()` — 自动检测 tracker 数量

### 注意事项

- Gen 1 基站建议使用 **B/C 模式**（非 A/B），用闪光灯同步
- OOTX 解码需要 tracker **持续**看到基站 ~15 秒不中断
- Solver 需要 tracker **移动**才能求解基站位置，静止不动无法收敛
- 标定数据保存在 `/root/.config/libsurvive/config.json`，删除后需重新标定
