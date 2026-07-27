# 多帧 PGM 灰度流输出 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 让四路摄像头的每个 session 灰度文件成为可由 FFmpeg 逐帧读取的固定尺寸多帧 PGM 流。

**Architecture:** 新增无硬件依赖的 header-only PGM 写入器，统一负责 P5 头、按行复制、补零、截取和写入检查。两种采集实现只负责打开 `.pgm` 文件，并将 NV12 的 Y 平面与实际尺寸交给该写入器；输出尺寸始终取各相机 `CameraConfig`。

**Tech Stack:** C++20、C stdio、Make、FFmpeg image2pipe、Nori Xvision / Rockchip MPP（仅完整采集构建与板端验收）。

## Global Constraints

- 每路摄像头每个 session 只输出一个连续的 `.pgm` 文件，不拆分单帧文件。
- 每帧写 `P5\n<configured-width> <configured-height>\n255\n`，其后严格写配置宽 × 配置高个 Y8 字节。
- Y8 像素仍取 BGR→NV12 后的 Y 平面；不引入颜色转换、压缩或新依赖。
- 源帧较小时，右侧和底部逐行补 `0`；源帧较大时截取左上角配置范围。
- 覆盖 `VideoSensor`（JHH2 左/右）和 `SixCamSensor`（JHH02/JHH04）的所有 Y8 输出。
- 不修改历史采集记录和旧 `.y8` 文件；仅更新当前使用说明。
- 单元测试必须不依赖 Nori、MPP、libsurvive 或采集硬件。

---

## 文件结构

| 文件 | 职责 |
| --- | --- |
| `hardware/VideoSensor/pgm_stream.h` | 无状态的 PGM 帧串写入函数；校验输入、写入固定头、逐行复制/补零/截取。 |
| `tests/test_pgm_stream.cpp` | 通过临时文件验证字节级 PGM 帧布局。 |
| `hardware/VideoSensor/VideoSensor.h` | 为两路独立 JHH2 打开 `.pgm`，调用共用写入器并在 I/O 失败后禁用该路输出。 |
| `hardware/VideoSensor/SixCamSensor.h` | 为 JHH02/JHH04 执行与独立 JHH2 相同的 PGM 写入规则。 |
| `camera_config.h` | 将 `output_y8` 的注释更新为多帧 PGM 流。 |
| `Makefile` | 增加 PGM 单元测试目标与主编译依赖。 |
| `README.md`、`CLAUDE.md`、`docs/01-check-data.md`、`tests/README.md` | 标明新扩展名、格式语义和 FFmpeg 读取命令。 |

### Task 1: 以测试驱动实现固定尺寸 PGM 帧写入器

**Files:**
- Create: `tests/test_pgm_stream.cpp`
- Create: `hardware/VideoSensor/pgm_stream.h`
- Modify: `Makefile:44, 71-79`

**Interfaces:**
- Produces: `pgm_stream::write_pgm_frame(FILE* fp, const uint8_t* source_y, int source_width, int source_height, int output_width, int output_height) -> bool`.
- Consumed by: `VideoSensor::collect()` and `SixCamSensor::collect_channel()` in Task 2.

- [ ] **Step 1: 写入会失败的字节级测试和构建目标**

创建 `tests/test_pgm_stream.cpp`，包含尚不存在的 `hardware/VideoSensor/pgm_stream.h`，并使用 `tmpfile()` 写入两帧。第一帧为 `2×1` 的 `{1, 2}`，输出为 `3×2`；第二帧为 `4×3`，输出也为 `3×2`。测试应构造以下期望字节并完全比较：

```cpp
const std::vector<uint8_t> expected = {
    'P','5','\n','3',' ','2','\n','2','5','5','\n',
    1, 2, 0,
    0, 0, 0,
    'P','5','\n','3',' ','2','\n','2','5','5','\n',
    10, 11, 12,
    20, 21, 22,
};

assert(pgm_stream::write_pgm_frame(fp, small, 2, 1, 3, 2));
assert(pgm_stream::write_pgm_frame(fp, large, 4, 3, 3, 2));
fflush(fp);
rewind(fp);
// fread 到 vector，assert(actual == expected)
```

其中 `large` 为三行 `{10,11,12,13}`、`{20,21,22,23}`、`{30,31,32,33}`。测试还必须断言空 `FILE*`、空像素指针、零或负尺寸都返回 `false`。

在 Makefile 的 `.PHONY` 加入 `test_pgm_stream`，并添加：

```make
test_pgm_stream: tests/test_pgm_stream.cpp hardware/VideoSensor/pgm_stream.h

	$(CXX) $(CXXFLAGS) -I. -o $@ tests/test_pgm_stream.cpp
	./$@
```

- [ ] **Step 2: 运行测试并确认它因缺失写入器而失败**

Run: `make test_pgm_stream`

Expected: FAIL；错误为 `hardware/VideoSensor/pgm_stream.h` 不存在或无法包含，而不是测试断言失败。

- [ ] **Step 3: 实现最小的 header-only 写入器**

创建 `hardware/VideoSensor/pgm_stream.h`。只包含 `<cstddef>`、`<cstdint>`、`<cstdio>`。实现以下接口：

```cpp
namespace pgm_stream {

inline bool write_pgm_frame(FILE* fp,
                            const uint8_t* source_y,
                            int source_width,
                            int source_height,
                            int output_width,
                            int output_height);

}  // namespace pgm_stream
```

实现要求：

1. `fp == nullptr`、`source_y == nullptr` 或四个尺寸任一不大于 0 时返回 `false`，且不得写入。
2. 先用 `fprintf(fp, "P5\\n%d %d\\n255\\n", output_width, output_height)` 写入头；返回值小于 0 时返回 `false`。
3. 对 `y` 从 `0` 到 `output_height - 1`：若 `y < source_height`，写 `min(source_width, output_width)` 字节 `source_y + y * source_width`；随后写足本行余下的零字节。若 `y >= source_height`，整行写零。
4. 使用固定的零缓冲区分块写入，不对每帧分配完整输出图像；每个 `fwrite` 必须比较返回字节数，不足即返回 `false`。
5. 只有头和全部像素都成功写入时返回 `true`。

- [ ] **Step 4: 运行 PGM 单元测试并确认通过**

Run: `make test_pgm_stream`

Expected: PASS（退出码 0）；测试证明第一帧按行补零、第二帧截取右侧/底部之外数据，且两帧之间无额外字节。

- [ ] **Step 5: 提交写入器与测试**

```bash
git add hardware/VideoSensor/pgm_stream.h tests/test_pgm_stream.cpp Makefile
git commit -m "feat: add fixed-size PGM stream writer"
```

### Task 2: 将四路 Y8 输出接入统一 PGM 流

**Files:**
- Modify: `hardware/VideoSensor/VideoSensor.h:27-39, 159-168, 284-294`
- Modify: `hardware/VideoSensor/SixCamSensor.h:27-38, 212-218, 392-395`
- Modify: `camera_config.h:21-22`
- Modify: `Makefile:51-56`

**Interfaces:**
- Consumes: Task 1 的 `pgm_stream::write_pgm_frame()`。
- Produces: 四个相机目录中的 `<camera>-<session-ts>.pgm` 多帧流，所有帧尺寸等于该相机的 `CameraConfig::width/height`。

- [ ] **Step 1: 先运行已通过的 PGM 格式测试作为集成基线**

Run: `make test_pgm_stream`

Expected: PASS；若失败，先修复 Task 1，不开始修改采集类。

- [ ] **Step 2: 接入独立 JHH2 的 `VideoSensor`**

在 `VideoSensor.h` 引入 `pgm_stream.h`。把输出路径从 `.y8` 改为 `.pgm`，并用二进制模式打开：

```cpp
snprintf(path, sizeof(path), "%s/%s-%s.pgm",
         out_dir_.c_str(), cfg_.name, session_ts_.c_str());
y8_fp_ = fopen(path, "wb");
```

保留已有的打开失败日志。将 NV12 Y 平面原始 `fwrite` 替换为：

```cpp
if (cfg_.output_y8 && y8_fp_ &&
    !pgm_stream::write_pgm_frame(y8_fp_, nv12, w, h,
                                 cfg_.width, cfg_.height)) {
    fprintf(stderr, "[%s] PGM write failed; disabling PGM output\\n", cfg_.name);
    fclose(y8_fp_);
    y8_fp_ = nullptr;
}
```

这覆盖 `jhh2_left` 和 `jhh2_right`，并且不改变 H.265、IMU 或 preview 分支。

- [ ] **Step 3: 接入 SixCam 的 JHH02 和 JHH04**

在 `SixCamSensor.h` 引入同一头文件。把两条通道的 `.y8` 路径替换为 `.pgm`，用 `fopen(path, "wb")` 打开，并在打开失败时输出 `[%s] cannot create PGM file %s`。

将 `collect_channel()` 中的 `fwrite(nv12, 1, w * h, ch.y8_fp)` 替换为：

```cpp
if (ch.output_y8 && ch.y8_fp &&
    !pgm_stream::write_pgm_frame(ch.y8_fp, nv12, w, h,
                                 ch.width, ch.height)) {
    fprintf(stderr, "[%s] PGM write failed; disabling PGM output\\n", ch.name);
    fclose(ch.y8_fp);
    ch.y8_fp = nullptr;
}
```

这覆盖 `jhh02` 和仅输出灰度的 `jhh04`。保留现有 teardown 的 `fclose`，因为它已对空指针安全。

- [ ] **Step 4: 更新配置说明与编译依赖**

将 `camera_config.h` 的 `output_y8` 注释改为“是否写 Y8 像素的多帧 PGM 流”。在 Makefile 的 `main.o` 依赖列表加入 `hardware/VideoSensor/pgm_stream.h`，使该头变更触发主程序重编译。

- [ ] **Step 5: 运行格式回归和可用的编译检查**

Run: `make test_pgm_stream && git diff --check`

Expected: PASS、无空白错误。

在具备 Nori Xvision、MPP、TurboJPEG、gpiod 与 libsurvive 的 RK3588 构建环境运行：

```bash
make clean && make
```

Expected: `unified_capture` 编译成功，无新增警告。不要将本机缺少板端 SDK 导致的编译失败误报为本改动失败。

- [ ] **Step 6: 提交四路采集接入**

```bash
git add hardware/VideoSensor/VideoSensor.h hardware/VideoSensor/SixCamSensor.h \
        camera_config.h Makefile
git commit -m "feat: store camera grayscale as PGM streams"
```

### Task 3: 更新运行文档并执行板端格式验收

**Files:**
- Modify: `README.md:90-112`
- Modify: `CLAUDE.md:72-76, 123-130`
- Modify: `docs/01-check-data.md:1-4`
- Modify: `tests/README.md:13-17, 32-37`

**Interfaces:**
- Consumes: Task 2 生成的 `<camera>-<session-ts>.pgm`。
- Produces: 与运行时真实格式一致的用户/维护者说明和可重复的验收命令。

- [ ] **Step 1: 更新当前输出说明和读取示例**

在 README 和 CLAUDE 的输出树中把 `.y8` 改为 `.pgm`，并注明“连续多帧 P5 PGM 流，像素来自 NV12 Y 平面；帧尺寸固定为相机配置宽高，异常小帧补零”。在 README 的输出结构下添加：

```bash
ffplay -f image2pipe -c:v pgm -framerate 30 -i jhh04-<session-ts>.pgm
ffmpeg -f image2pipe -c:v pgm -framerate 30 -i jhh04-<session-ts>.pgm output.mkv
```

将 `docs/01-check-data.md` 的 rawvideo/`.y8` 示例替换为相同的 `.pgm` `image2pipe` 示例，并保留 MKV 查看命令。

- [ ] **Step 2: 记录新增单元测试入口**

在 `tests/README.md` 的“所有测试”命令和单元测试表中加入 `make test_pgm_stream` 与 `test_pgm_stream.cpp`，说明它覆盖多帧 PGM 头、逐行补零和截取。

- [ ] **Step 3: 运行文档和格式回归**

Run: `make test_pgm_stream && git diff --check && rg -n '\\.y8|rawvideo' README.md CLAUDE.md docs/01-check-data.md`

Expected: PGM 单元测试通过、`git diff --check` 无输出、当前文档不再把采集输出描述为 `.y8` 或 rawvideo。

- [ ] **Step 4: 在 RK3588 执行短采集验收**

在目标板运行一次短 session 后，对每个实际启用相机的 `.pgm` 执行：

```bash
ffprobe -v error -f image2pipe -c:v pgm -count_frames \
  -show_entries stream=codec_name,width,height,nb_read_frames \
  -of default=noprint_wrappers=1 <camera>-<session-ts>.pgm
```

Expected: `codec_name=pgm`；宽高分别等于该相机配置；`nb_read_frames` 大于 0。用相同文件运行 `ffmpeg -f image2pipe -c:v pgm -framerate 30 -i ... output.mkv`，Expected: 输出 MKV 可由 `ffprobe` 识别为视频流。

- [ ] **Step 5: 提交文档**

```bash
git add README.md CLAUDE.md docs/01-check-data.md tests/README.md
git commit -m "docs: document multi-frame PGM capture output"
```

## 最终验证清单

- [ ] `make test_pgm_stream` 退出码为 0。
- [ ] `git diff --check` 无输出。
- [ ] 在 RK3588 完整构建成功。
- [ ] 每个启用相机的短采集 `.pgm` 可被 `ffprobe -f image2pipe -c:v pgm` 读出一帧以上，且宽高等于配置。
- [ ] 未修改历史记录、历史计划或已存在 `.y8` 采集数据。
