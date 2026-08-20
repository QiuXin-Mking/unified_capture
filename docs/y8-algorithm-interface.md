# JHH04 Y8 算法侧接口

## 1. 输出内容

统一采集程序为六目四目侧 `jhh04` 提供两种输出：

1. 持久化文件：用于离线处理和故障追溯。
2. 共享内存实时帧：用于同一块 RK3588 上的算法进程实时消费。

两者来自同一次 MJPEG→YUV 解码，实时推送失败不会阻塞 `.y8` 文件写入和视频采集。

## 2. Y8 文件

文件路径：


```text
/media/usb0/capture/<prefix>/session_001/jhh04/
└── jhh04-<timestamp>.y8
```

文件是无头、无行填充的连续灰度帧流：

| 属性 | 值 |
|---|---:|
| 宽 | 3104 |
| 高 | 480 |
| 像素格式 | gray8 |
| 单帧字节数 | 1,489,920 |
| 目标帧率 | 30 fps |

完整帧数为：

```text
file_size / 1,489,920
```

## 3. 实时接口

### 3.1 Unix socket

算法进程连接：

```text
/tmp/unified_capture_jhh04_y8.sock
```

连接成功后，采集进程发送一行握手信息：

```text
Y8_SHM 1 /unified_capture_jhh04_y8 width=3104 height=480 bytes=1489920 slots=8
```

随后每发布一帧发送一行通知：

```text
FRAME 123 slot=3 pts_us=456789 bytes=1489920
```

其中 `FRAME` 的数字是采集帧序号，`slot` 是共享内存中的槽位。

Socket 只传递控制信息和帧通知，不传递 Y8 图像本身。

### 3.2 共享内存

算法进程使用握手中的名称调用：

```cpp
int fd = shm_open("/unified_capture_jhh04_y8", O_RDONLY, 0);
void* mapping = mmap(nullptr, mapping_bytes, PROT_READ, MAP_SHARED, fd, 0);
```

共享内存包含 8 个槽位。槽位地址计算为：

```cpp
slot_offset = sizeof(Y8SharedMemoryHeader) +
              slot * (sizeof(Y8SharedMemorySlotHeader) + 1489920);
payload = slot_offset + sizeof(Y8SharedMemorySlotHeader);
```

发布者先写 payload，最后以 release 语义写入槽位 sequence；消费者以 acquire 语义读取 sequence 后再读取 payload。消费者必须在处理期间保存并校验该 sequence，因为环形队列可能在处理期间覆盖旧槽位。

### 3.3 丢帧策略

共享内存是“最新帧优先”的非阻塞环形队列：

- 算法进程不连接时，采集继续运行；
- 算法进程处理速度不足时，旧槽位会被新帧覆盖；
- 通过通知中的 frame 序号判断是否丢帧；
- 不允许为了等待算法进程而阻塞 V4L2 采集线程。

如果算法必须处理每一帧，应直接读取 `.y8` 文件或另行增加专用磁盘队列。

## 4. 最小消费流程

1. 连接 `/tmp/unified_capture_jhh04_y8.sock`。
2. 读取并解析 `Y8_SHM` 握手行。
3. `shm_open` 并 `mmap` 共享内存。
4. 循环读取 `FRAME` 通知，根据 `slot` 定位 Y8 payload。
5. 校验槽位 sequence、`bytes == 1489920`，再交给算法。
6. Socket 断开后关闭 mmap 和 fd；下一次采集重新连接。

## 5. 生命周期与故障

共享内存发布器随 `jhh04` 采集 session 创建和销毁。采集停止后 socket 关闭，算法进程应回到连接等待状态。若 socket 或共享内存创建失败，程序会记录告警并继续写 `.y8` 文件；算法侧可降级为 session 文件读取。
