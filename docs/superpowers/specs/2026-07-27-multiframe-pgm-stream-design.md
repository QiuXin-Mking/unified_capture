# 多帧 PGM 灰度流输出设计

## 目标

将 unified_capture 的所有 Y8 灰度输出改为单文件、连续追加的多帧 PGM 流。每路摄像头每个 session 仍只生成一个灰度文件；FFmpeg 可按 PGM 图像流逐帧读取。

## 范围

- 覆盖 `VideoSensor` 的两路独立 JHH2 输出。
- 覆盖 `SixCamSensor` 的 JHH02 与 JHH04 两路输出。
- 保持 Y8 像素取自现有 NV12 的 Y 平面，不做颜色转换。
- 不修改 H.265/MKV、IMU、预览或历史采集文件。

## 文件格式与命名

每个输出文件扩展名改为 `.pgm`，命名保持现有摄像头名和 session 时间戳：

```text
<camera>/<camera>-<session-ts>.pgm
```

文件是二进制 PGM（P5）图像的无间隔串联。每帧精确写入：

```text
P5\n<configured-width> <configured-height>\n255\n<configured-width × configured-height 个字节>
```

因此每帧都是独立、可解析的 PGM 图像，整个文件是标准的多图像 PGM 流。读取示例：

```bash
ffmpeg -f image2pipe -c:v pgm -framerate 30 -i input.pgm output.mkv
```

普通静态图片查看器可能只显示第一帧；应使用 FFmpeg、Netpbm 或可读取 PNM 图像流的工具处理整段采集。

## 固定尺寸与补零规则

PGM 头使用相机配置的宽高，而不是 JPEG 帧实际解码尺寸，以便同一路文件的所有帧保持相同大小。

写入以行为单位进行：

1. 对每个目标行，复制源行中 `min(source_width, configured_width)` 个 Y8 字节。
2. 目标行的剩余字节补 `0`。
3. 源帧缺少的目标行全部补 `0`。
4. 若源帧超出配置宽高，只保留左上角配置范围，防止破坏固定帧边界。

正常情况下 SDK 已按配置请求分辨率，补零或截取仅用于处理异常尺寸帧。补零不会改变已存在的 Y8 像素。

## 实现结构

新增无硬件依赖的 header-only PGM 流写入辅助函数，负责写 P5 头、逐行复制、补零与完整写入检查。`VideoSensor` 和 `SixCamSensor` 都调用该函数，避免四路输出出现格式差异。

`output_y8` 配置开关继续保留，语义更新为“输出 Y8 像素的多帧 PGM 流”。输出文件以二进制模式打开；任一路打开或写入失败时记录摄像头和路径，并停止该路 PGM 写入，其他采集输出不受影响。磁盘 I/O 失败无法通过补零恢复。

## 验证

新增纯 C++ 单元测试，不依赖 Nori、MPP 或硬件。测试在临时文件写入多帧并验证：

- 每帧有正确的 P5、固定宽高和 `255` 头；
- 小尺寸源帧按行补零，不产生行错位；
- 超尺寸源帧按规则截取；
- 连续两帧之间没有填充或额外字节。

Makefile 新增独立测试目标。目标板端还应执行一次短采集，并使用 `ffprobe -f image2pipe -c:v pgm -count_frames` 确认帧数和配置分辨率。

## 文档更新

更新当前 README、项目指导说明和数据检查命令中的 `.y8` 描述及 rawvideo 用法。历史计划、记录和既有采集结果保留原样，避免把历史事实改写成新格式。
