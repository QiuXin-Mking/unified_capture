# unified_capture 核心概念讲解与练习

基于 RK3588 多摄像头统一采集程序，涵盖 C++ 多线程、Linux 系统编程、Unix Domain Socket 三大专题，共 17 道简答题。

---

# 专题一：C++ 多线程（5 题）

## 1.1 std::thread —— 线程创建与 join

### 是什么

`std::thread` 是 C++11 标准库提供的线程抽象。构造一个 `std::thread` 对象时，传入一个可调用对象（函数指针、lambda、函数对象），操作系统就会创建一个新线程来执行它。`join()` 阻塞当前线程，直到目标线程执行完毕退出。

核心语义：**一个 `std::thread` 对象代表一个执行线程的所有权**。它不可拷贝（没有两个对象能拥有同一个线程），只可移动。

### 为什么需要

没有线程，程序就在一个执行流里串行运行。多摄像头采集如果串行，等于先等摄像头 A 采完再采摄像头 B，帧率完全不可接受。需要的是：每个摄像头独立采集、编码、写盘，主线程统一管理生命周期。

`join()` 的必要性：线程对象析构时，如果线程还是 `joinable` 状态（既没有被 join 也没有被 detach），程序会直接 `std::terminate`。所以 `join()` 是线程生命周期管理的"句号"——你必须等它结束，或者显式放弃它（detach）。

### 项目中的使用

**位置 1：Sensor 基类的 `launch()` 方法**（`sensor.h` 第 32-44 行）

```cpp
void launch(SimpleBarrier& gate) {
    thread_ = std::thread([this, &gate]() {
        setup();
        gate.arrive_and_wait();  // 同步点
        collect();
        teardown();
    });
}
```

每个 Sensor（VideoSensor、ImuSensor、EncoderSensor 等）调用 `launch()` 时，就在新线程中跑"setup 初始化 → barrier 同步 → collect 采集 → teardown 清理"的完整生命周期。

**位置 2：SixCamSensor 的 `collect()` 方法**（`sixcam_sensor.h` 第 280-283 行）

```cpp
void collect() override {
    std::thread t0([this]() { collect_channel(0); });
    std::thread t1([this]() { collect_channel(1); });
    t0.join();
    t1.join();
}
```

六目模组的两个通道（jhh04 和 jhh02）需要同时采集，所以六目 Sensor 自己的 collect 线程里，再创建两个子线程分别拉帧，然后 join 等两者都结束。

**位置 3：`join()`**（`sensor.h` 第 46-50 行，`main.cpp` 第 344 行）

```cpp
void join() {
    if (thread_.joinable()) thread_.join();
}
```

`main.cpp` 的 `run_session()` 结束时遍历所有 sensor 调用 `s->join()`，保证所有线程都干净退出后再释放资源。

### 简答题 1.1

**问题**：在 `Sensor::launch()` 中，lambda 通过 `[this, &gate]` 捕获了 `this` 和 `gate` 的引用。如果 `main` 线程在 sensor 线程结束之前就让 `SimpleBarrier gate` 对象析构了（比如 gate 是局部变量，函数提前返回），会发生什么？为什么这在本项目中不会发生？

**参考答案**：会发生未定义行为（悬挂引用）。gate 通过引用捕获，一旦 gate 对象被销毁，sensor 线程内部调用 `gate.arrive_and_wait()` 就会访问已释放的内存，大概率 segfault。

本项目不会发生的原因：`gate` 在 `run_session()` 中定义为局部变量（`main.cpp` 第 307 行），`main` 线程在 `while (!gate.wait_all_arrived(100))` 循环中阻塞等待所有 sensor 到达 barrier，然后进入 `while (g_session_running)` 轮询等待采集结束，最后才遍历 `sensors` 调用 `join()`。此时所有线程必然已经结束（因为 `g_session_running = false` 会让所有 collect 循环退出），`gate` 在整个 sensor 线程的生命周期内始终存活。

---

## 1.2 std::mutex + std::lock_guard —— 互斥锁

### 是什么

`std::mutex` 是互斥量，保证同一时刻只有一个线程能访问被保护的临界区。`std::lock_guard` 是 RAII 风格的锁包装器：构造时 `lock()`，析构时自动 `unlock()`，无论临界区是正常返回还是抛异常退出，锁一定会被释放。

### 为什么需要

多个线程同时访问共享资源时，没有互斥就会产生数据竞争（data race），导致数据损坏或崩溃。数据竞争在 C++ 标准中是**未定义行为**，编译器可以做任何优化假设，导致极其诡异的 bug。

本项目更需要 mutex 的另一个原因：**第三方 SDK 的线程安全性限制**。TSTC SDK 的 `STREAM_STATUS` 接口不支持多路并发调用，同时调用会死锁在 SDK 内部。必须用锁来串行化。

### 项目中的使用

**位置 1：全局互斥锁保护 TSTC SDK 启流**（`video_sensor.h` 第 45 行，第 162-183 行）

```cpp
static std::mutex g_stream_start_mutex;

// 在 VideoSensor::setup() 中:
{
    std::lock_guard<std::mutex> lock(g_stream_start_mutex);
    TST_USBCam_Video_DEAL_WITH_INIT(tstc_handle_, dev_fd_);
    pthread_create(&stream_thread_, nullptr, VideoSensor::stream_thread_func, this);
    usleep(200000);
    TST_USBCam_Video_STREAM_STATUS(tstc_handle_, 1);  // 阻塞式, SDK 内部有锁
}
```

这里锁的范围包含了整个 `DEAL_WITH_INIT → 创建流线程 → STREAM_STATUS` 三段操作。原因是相同 VID/PID 的设备（比如多个 JHH2 双目相机）共享同一套 USB 协议栈，如果两个设备同时进入 `STREAM_STATUS`，SDK 内部的状态机冲突会导致死锁。

**位置 2：FrameQueue 内部的 `mtx_`**（`frame_queue.h` 第 34、43、52、60 行）

`try_push`、`pop_wait`、`try_pop`、`empty` 四个方法都先加锁再操作 `std::queue`，因为 `std::queue` 不是线程安全的，生产者（Video 线程）和消费者（IMU 线程）同时 push/pop 会导致内部指针损坏。

### 简答题 1.2

**问题**：在 `VideoSensor::setup()` 中，`std::lock_guard` 的作用域用了一对额外的大括号 `{ ... }` 包裹（第 162-183 行），而不是让锁持有到函数结束。为什么这么设计？如果把大括号去掉，让 `lock_guard` 的生命周期延长到整个 `setup()` 结束，会有什么后果？

**参考答案**：大括号的作用是**缩小临界区**，让锁在启流完成后尽早释放。TSTC SDK 的 `STREAM_STATUS` 调用是阻塞式的（要等固件初始化完成），如果不缩小临界区，后续的 FIFO 创建、MPP 初始化、Y8 文件打开等操作（都在锁外面，不需要互斥保护）也要在持锁状态下执行，会阻塞其他摄像头。效果就是：本来 4 个摄像头可以依次拿到锁、快速完成启流、释放锁给下一个，变成了每个摄像头独占锁直到 setup 完全结束，总启动时间被拉长。更要命的是，如果 FIFO 的 `open(O_WRONLY)` 阻塞等待 ffmpeg 打开读端，而锁还没释放，那其他所有摄像头都被卡住无法启流，造成死锁。

---

## 1.3 std::condition_variable —— 条件变量（wait/notify）

### 是什么

条件变量解决的问题是：**一个线程需要等待某个条件成立才能继续，但轮询太浪费 CPU**。

- `wait(lock, predicate)`：释放锁并阻塞，直到被唤醒且 predicate 返回 true。醒来时自动重新获取锁。
- `notify_one()`：唤醒一个等待的线程。
- `notify_all()`：唤醒所有等待的线程。

本质是一个"等待通知"机制，让线程在条件不满足时休眠，条件满足时被唤醒，零 CPU 开销。

### 为什么需要

假设没有条件变量，ImuSensor 要等 VideoSensor 产出 BGR 帧，只能写一个死循环：

```cpp
while (queue.empty()) { /* 忙等, CPU 100% */ }
```

这在嵌入式设备（RK3588）上是不可接受的——CPU 被白白烧掉，影响编码、写盘等真正需要算力的任务。

### 项目中的使用

**位置 1：FrameQueue 的生产者-消费者**（`frame_queue.h`）

```cpp
// 生产者 (VideoSensor::collect):
bool try_push(BGRFrame&& frame) {
    std::lock_guard<std::mutex> lock(mtx_);
    if (q_.size() >= max_size_) return false;
    q_.push(std::move(frame));
    cv_.notify_one();  // ★ 唤醒消费者
    return true;
}

// 消费者 (ImuSensor 实际上用 try_pop 轮询 + usleep, 但 pop_wait 提供了阻塞等待接口):
BGRFrame pop_wait() {
    std::unique_lock<std::mutex> lock(mtx_);
    cv_.wait(lock, [this] { return !q_.empty(); });  // ★ 等帧
    BGRFrame f = std::move(q_.front());
    q_.pop();
    return f;
}
```

VideoSensor 每解码一帧 BGR，就 `try_push` 到队列并 `notify_one()`。如果 ImuSensor 在 `pop_wait()` 中阻塞等待，就会被唤醒取帧。这里用了 `std::unique_lock` 而非 `lock_guard`，因为 `wait` 内部需要 unlock/lock 锁，`lock_guard` 不提供手动 unlock 的能力。

**位置 2：SimpleBarrier 的同步点**（`barrier.h`）

```cpp
void arrive_and_wait() {
    std::unique_lock<std::mutex> lock(mtx_);
    size_t gen = generation_;
    if (++arrived_ == count_) {
        arrived_ = 0;
        generation_++;
        cv_.notify_all();  // ★ 最后一个到达的线程唤醒所有人
    } else {
        cv_.wait(lock, [this, gen] { return gen != generation_; });  // ★ 先到的等
    }
}
```

这是一个自实现的 barrier（因为项目用 GCC 10，不支持 C++20 的 `std::barrier`）。所有 sensor 线程在 `setup()` 完成后调用 `arrive_and_wait()`：先到的线程阻塞等待，最后一个到达的线程翻转 generation 并 `notify_all()`，所有线程同时被唤醒，一起进入 `collect()`。

### 简答题 1.3

**问题**：`cv_.wait(lock, [this] { return !q_.empty(); })` 这行代码中，为什么 predicate（`!q_.empty()`）是必要的？如果不带 predicate，直接写 `cv_.wait(lock)`，会有什么风险？

**参考答案**：有两个风险：

**（1）虚假唤醒（spurious wakeup）**：POSIX 标准和 C++ 标准都明确允许条件变量在没有 `notify` 的情况下醒来。如果只用 `wait(lock)`，线程可能被虚假唤醒后，在队列仍为空的情况下就去 `q_.front()`，访问空队列是未定义行为。

**（2）丢失通知（lost wakeup）**：如果生产者在消费者进入 `wait` 之前就 push 了数据并 `notify_one`，那个通知就丢失了（因为还没有等待者）。带 predicate 的 `wait` 会先检查条件——如果队列已经有数据，就根本不进入等待，直接拿帧走人。保证"先产后消"也不丢数据。

---

## 1.4 std::atomic —— 原子变量

### 是什么

`std::atomic<T>` 保证对变量的读取和写入是**不可分割的原子操作**。一个线程写，另一个线程读，不会读到"写到一半"的中间状态。同时它还提供内存序（memory order）语义，保证多线程间的可见性。

### 为什么需要

普通 `bool` 变量在多线程环境下有两个问题：

1. **撕裂读/写**：虽然 `bool` 在现代平台通常是一个字节，看似天然原子，但编译器可能做各种优化（寄存器缓存、指令重排），导致一个线程写入 `true` 后，另一个线程可能永远看不到。
2. **数据竞争**：C++ 标准规定，两个线程同时访问同一内存位置，且至少一个是写操作，如果没有同步机制就是数据竞争，属于未定义行为。编译器可以据此做任何激进的优化假设，比如把 `while (running)` 优化成 `if (running) while (true)`。

`std::atomic` 同时解决了原子性和可见性，且明确告诉编译器"这个变量会被其他线程修改，不要优化掉对它的读写"。

### 项目中的使用

**位置 1：全局运行标志**（`main.cpp` 第 35-37 行）

```cpp
static std::atomic<bool> g_running{true};         // 整个程序是否继续运行
static std::atomic<bool> g_session_running{false}; // 当前采集 session 是否活跃
std::atomic<int> g_jhh2_remaining{0};              // JHH2 设备剩余未启流数
```

`g_running` 被信号处理函数（SIGINT）写为 `false`，被主循环的 `while (g_running)` 读取。`g_session_running` 被 socket / GPIO 处理代码写入，被所有 sensor 线程的 `while (running_)` 读取。`g_jhh2_remaining` 被每个 JHH2 VideoSensor 递减，被 jhh04 的 SixCamSensor 等待归零。

**位置 2：Sensor 内部的 running_ 引用**（`sensor.h` 第 28、56 行）

```cpp
Sensor(std::string name, std::atomic<bool>& running)
    : name_(std::move(name)), running_(running) {}
```

每个 Sensor 持有 `g_session_running` 的引用，在自己的 `collect()` 循环中 `while (running_)` 判断是否继续。当主线程将 `g_session_running` 设为 false 时，所有线程的循环同时退出，实现了**跨线程的一键停止**。

**位置 3：socket 请求标志**（`main.cpp` 第 43 行）

```cpp
static std::atomic<bool> g_socket_start_request{false};
```

socket 处理线程写入 `true`，主循环用 `exchange(false)` 原子地读取并清零，避免请求丢失。

### 简答题 1.4

**问题**：在 `VideoSensor::collect()` 的帧循环中（第 214 行），条件是 `while (running_)`。假设有人"优化"成 `while (running_.load(std::memory_order_relaxed))`，使用 relaxed 内存序而非默认的 `seq_cst`。在这个项目中，这会导致什么实际的问题？（提示：关注 `g_session_running` 被信号处理函数修改的场景）

**参考答案**：`memory_order_relaxed` 只保证原子性，不保证不同线程间对多个变量的修改顺序可见。具体场景：

主线程的信号处理函数执行 `g_session_running = false`，随后 `run_session()` 中的 `for (auto* s : sensors) { s->join(); delete s; }` 开始清理资源。但是 sensor 线程用 `relaxed` 读 `running_`，可能看到 `g_session_running` 的旧值（true）的同时，也看到了其他内存操作的"未来"状态。更糟糕的是，relaxed 不建立 happens-before 关系，sensor 线程可能看到 `running_ == true`，继续调用 `TST_USBCam_GET_FRAME_BUFF`，而此时 TSTC 句柄可能已经被 teardown（在 `pthread_join(stream_thread_)` 后）释放了。用默认的 `seq_cst`（或至少 `acquire`），读操作会与主线程的写操作建立同步关系，确保看到 `running_ == false` 的同时，也看到了之前全部清理操作的效果。

---

## 1.5 pthread_create / pthread_join —— POSIX 线程

### 是什么

`pthread_create` 是 POSIX 标准的 C 接口线程创建函数，`pthread_join` 等待线程结束。与 `std::thread` 是对同一操作系统能力（pthread）的不同抽象层级。`std::thread` 在 Linux 上底层就是调用 `pthread_create`。

**关键区别**：

| 特性 | `std::thread` | `pthread_create` |
|------|--------------|-----------------|
| 语言层级 | C++ 标准库（RAII, 类型安全） | POSIX C 接口 |
| 线程函数签名 | 任意可调用对象 (lambda, bind, function) | `void* (*)(void*)` |
| 参数传递 | 类型安全，自动推导 | `void*` 裸指针，无类型检查 |
| 生命周期 | 析构时若 joinable 则 `std::terminate` | 无所谓，但泄漏资源 |
| 返回值 | 通过 `std::promise`/`std::future` 或引用捕获 | `void*` 返回值 + `pthread_join` 第二个参数 |

### 为什么需要（为什么项目中混用）

**TSTC SDK 是 C 接口的闭源库，它的内部流线程生命周期由 SDK 自己管理，要求调用方用 pthread 原生接口与其对接。**

具体来说，TSTC SDK 的内部实现使用了 pthread 的底层特性（比如特定的线程属性、取消点等），它期望外部传入的线程也是 pthread 原生句柄（`pthread_t`），以便在 SDK 内部做 `pthread_kill` 或信号控制。如果用 `std::thread`，虽然可以通过 `native_handle()` 拿到底层 `pthread_t`，但 std::thread 的 RAII 析构语义会与 SDK 的内部管理产生冲突——SDK 内部可能在你不期望的时候结束线程，而你持有的 `std::thread` 对象析构时会调用 `std::terminate`。

### 项目中的使用

**位置：VideoSensor 的 TSTC 内部流线程**（`video_sensor.h` 第 174 行，第 349 行，第 405-416 行）

```cpp
// 创建:
pthread_create(&stream_thread_, nullptr, VideoSensor::stream_thread_func, this);

// 等待:
pthread_join(stream_thread_, nullptr);

// 线程函数:
static void* stream_thread_func(void* arg) {
    auto* self = (VideoSensor*)arg;
    Pix_Format fmt;
    fmt.u_PixFormat = 0;
    fmt.u_Width  = (uint32_t)self->cfg_.width;
    fmt.u_Height = (uint32_t)self->cfg_.height;
    fmt.u_Fps    = (uint32_t)self->cfg_.fps;
    TST_USBCam_Video_DEAL_WITH(self->tstc_handle_, fmt);       // SDK 内部事件循环
    TST_USBCam_Video_DEAL_WITH_UNINIT(self->tstc_handle_);
    TST_USBCam_DELETE_DEVICE_POINT(self->tstc_handle_);
    return nullptr;
}
```

**线程层次**：

```
VideoSensor (C++ 层面)
  └── Sensor::launch() 用 std::thread 创建 → setup → collect → teardown  (C++ 线程)
        └── setup() 内用 pthread_create → stream_thread_func                (POSIX 线程, 跑 SDK 事件循环)
              └── TST_USBCam_Video_DEAL_WITH → SDK 内部阻塞式事件轮询
```

为什么 setup 线程（std::thread 层）不能直接调用 `TST_USBCam_Video_DEAL_WITH`？因为这是一个阻塞调用，会整个吞掉 setup 线程，collect 就无法执行了。必须另起一个线程给它跑 SDK 事件循环，然后 setup 线程继续启动 `STREAM_STATUS` 和后续的 collect 拉帧循环。

### 简答题 1.5

**问题**：在 `VideoSensor::setup()` 中，`pthread_create` 创建流线程后，紧接着就调用了 `usleep(200000)` 等待 200ms（`video_sensor.h` 第 175 行）。为什么要加这个 sleep？如果去掉它，直接调用 `TST_USBCam_Video_STREAM_STATUS`，可能会出现什么问题？

**参考答案**：这是一个**竞态条件的时间缓冲**。`pthread_create` 只是发出了创建请求，线程被调度执行的时间是不确定的。`TST_USBCam_Video_STREAM_STATUS(handle, 1)` 这个调用需要 SDK 内部的流线程已经初始化完成（比如 `DEAL_WITH` 中打开了视频管道、分配了 buffer 等）。如果创建线程后立即调用 `STREAM_STATUS`，流线程可能还没来得及执行 `TST_USBCam_Video_DEAL_WITH`，导致 SDK 内部状态不正确，最坏情况下 STREAM_STATUS 返回错误或死锁。

200ms 是一个经验值，在 RK3588 上足够流线程启动并进入 DEAL_WITH。严格来说这是一种不优雅的同步方式（更好的做法是用条件变量或信号量），但在嵌入式+闭源 SDK 的场景下，这往往是工程上最务实的解决方案——你不能修改 SDK 内部代码，只能通过经验调参保证时序正确。

---

## 专题一总结：五个概念在项目中的协同关系

```
main 线程 ─── signal(SIGINT) → g_session_running = false (atomic)
  │
  ├─ for each Sensor: s->launch(gate)  →  std::thread 创建 sensor 线程
  │     │
  │     ├─ VideoSensor::setup()
  │     │     ├─ lock_guard<mutex> 保护 TSTC SDK 串行化启流
  │     │     └─ pthread_create → TSTC 内部流线程 (SDK 事件循环)
  │     │
  │     ├─ gate.arrive_and_wait()  →  condition_variable 同步所有线程
  │     │
  │     ├─ VideoSensor::collect()
  │     │     └─ try_push → FrameQueue  { mutex + cv_.notify_one }
  │     │
  │     └─ teardown → pthread_join(stream_thread_)
  │
  └─ for each Sensor: s->join()  →  std::thread::join 等待退出

IMU 线程 ─── FrameQueue::try_pop()  { mutex } 消费 BGR 帧
```

---

# 专题二：Linux 系统编程（6 题）

## 2.1 poll() —— I/O 多路复用

### 是什么

`poll()` 是一个系统调用，允许一个线程同时监听多个文件描述符（fd）上的事件。原型：

```c
int poll(struct pollfd *fds, nfds_t nfds, int timeout_ms);
```

- `fds` 是一个数组，每个元素描述一个要监听的 fd 和感兴趣的事件（`POLLIN` 可读、`POLLOUT` 可写等）
- `nfds` 是数组长度
- `timeout_ms` 是超时毫秒数：-1 表示永久阻塞，0 表示立即返回，正数表示最多等 N 毫秒
- 返回值：就绪的 fd 数量，超时返回 0，出错返回 -1

内核会把每个 `pollfd` 的 `revents` 字段填上实际发生的事件，调用者遍历数组处理。

### 为什么需要

在嵌入式/服务端程序中，经常需要同时处理多个 I/O 源——比如 GPIO 按键事件、网络 socket 连接、串口数据。不用 `poll()` 的话，要么开多线程各自阻塞读（线程开销大、同步复杂），要么忙轮询（CPU 空转）。`poll()` 让一个线程高效地"哪个先来就先处理哪个"，CPU 在没事件时休眠。

### 项目中的使用

在 `main.cpp` 中有三处典型用法：

**(a) 主空闲循环：同时监听 GPIO 按键和 Unix Socket**（第 455-459 行）

```c
while (g_running) {
    struct pollfd pfds[2];
    pfds[0].fd = gpiod_line_event_get_fd(btn); pfds[0].events = POLLIN;
    pfds[1].fd = g_sock_fd; pfds[1].events = POLLIN;
    int ret = poll(pfds, 2, 200);  // 最长等 200ms
```

这里 `poll` 同时等两件事：用户按下 GPIO 按钮，或外部通过 Unix Socket 发来 `start` 命令。timeout 设为 200ms 使得即使没有事件，也能每 200ms 醒来检查 `g_running` 标志，从而响应 `SIGINT`。

**(b) 录制循环：监听停止信号**（第 323-328 行）

```c
while (g_session_running) {
    struct pollfd pfds[2];
    // ... pfds[0] = GPIO, pfds[1] = socket ...
    int ret = poll(pfds, np, 50);  // 50ms 超时
```

录制过程中，主线程用 `poll` 等 GPIO 按钮再次按下（停止）或 socket 发来 `stop` 命令。50ms 超时确保能及时响应停止。

**(c) 非阻塞探测：等待 sensor 就绪的同时处理 socket**（第 311-318 行）

```c
while (!gate.wait_all_arrived(100)) {
    if (g_sock_fd >= 0) {
        struct pollfd pfd;
        pfd.fd = g_sock_fd; pfd.events = POLLIN;
        if (poll(&pfd, 1, 0) > 0) {  // timeout=0, 非阻塞探测
            int c = accept(g_sock_fd, nullptr, nullptr);
```

timeout=0 使 `poll` 立即返回，不阻塞。这在等待 barrier 期间仍然能处理 socket 请求，实现"一边等、一边处理"。

### 简答题 2.1

**问题**：在录制循环中，`poll` 的超时被设为 50ms 而不是 -1（永久阻塞）。如果把 timeout 改成 -1，会发生什么问题？

**参考答案**：如果 timeout 改成 -1，`poll` 会在没有事件时永久阻塞。这导致：

1. 当外部通过另一个线程设置 `g_session_running = false` 时，poll 不会立即醒来，录制无法及时停止；
2. 实际上在这个设计中，`g_session_running` 只能在 poll 返回后被检查，所以如果 GPIO 按钮和 socket 都没有新事件，程序会永远卡在 poll 里，不会自行退出。50ms 超时确保了周期性检查退出条件。

---

## 2.2 fork() + execlp —— 创建子进程执行外部程序

### 是什么

`fork()` 创建一个当前进程的完整副本（子进程），子进程从 `fork()` 返回点继续执行，拥有独立的地址空间。`fork()` 返回两次：父进程收到子进程 PID，子进程收到 0。

`execlp()` 是 exec 家族的一员，用指定程序**替换**当前进程的整个地址空间——代码段、数据段、堆栈全被新程序覆盖，PID 不变。

```c
pid_t pid = fork();
if (pid == 0) {
    // 子进程
    execlp("ffmpeg", "ffmpeg", "-i", "input", "output.mkv", NULL);
    perror("exec");  // 只有 execlp 失败才会执行到这里
    _exit(1);
}
// 父进程继续
```

### 为什么需要

当程序需要用到一个外部工具（如 FFmpeg）但又不想（或不能）把它链接进来时，`fork+exec` 是最标准的做法。另一个场景是隔离风险：子进程崩溃不影响父进程。相比于 `system()`，`fork+exec` 不经过 shell，没有注入风险，且能精确控制环境。

### 项目中的使用

在 `video_sensor.h` 第 114-133 行，每个支持 H.265 编码的摄像头都会 fork 一个 FFmpeg 子进程来做 MKV 封装：

```c
ffmpeg_pid_ = fork();
if (ffmpeg_pid_ < 0) {
    perror("fork ffmpeg");
    return;
}
if (ffmpeg_pid_ == 0) {
    // 子进程: ffmpeg 读 FIFO → MKV
    snprintf(path, sizeof(path), "%s/%03d.mkv", out_dir_.c_str(), session_num_);
    execlp("ffmpeg", "ffmpeg",
           "-y", "-hide_banner", "-loglevel", "error",
           "-f", "hevc", "-r", fps_s,
           "-i", fifo_path_.c_str(),
           "-c", "copy", path, NULL);
    perror("exec ffmpeg");
    _exit(1);
}
```

关键设计点：
1. 子进程用 `execlp` 执行 `/usr/bin/ffmpeg`（`execlp` 在 PATH 中搜索），不去拼 shell 命令
2. `-f hevc` 告诉 FFmpeg 输入是 H.265 裸流，`-c copy` 表示不重新编码，直接封装
3. 输入 `-i` 指向一个 FIFO 文件（见下节 mkfifo），这是父子进程间的数据管道
4. `_exit(1)` 而非 `exit(1)`：避免子进程在 exec 失败后执行父进程注册的 atexit 回调或刷新父进程的 stdio 缓冲

父进程在 teardown 阶段用 `waitpid(ffmpeg_pid_, &status, 0)` 等待子进程退出，避免僵尸进程。

### 简答题 2.2

**问题**：为什么在 `execlp` 失败后要调 `_exit(1)` 而不是 `exit(1)`？如果误用了 `exit(1)` 会有什么后果？

**参考答案**：`exit()` 会执行 `atexit` 注册的回调函数，并 flush 所有 stdio 缓冲区。在 `fork` 后，子进程继承了父进程的完整内存镜像，包括 atexit 回调表和 stdio 缓冲区内容。如果子进程用 `exit()`，可能导致：(1) 父进程注册的清理函数被执行两次（一次在子进程、一次在父进程），可能 double-free 或关闭已经被父进程使用的 fd；(2) stdio 缓冲区中的内容被重复输出。`_exit()` 直接终止进程，跳过所有这些清理，更加安全。

---

## 2.3 mkfifo —— 命名管道 (FIFO)

### 是什么

`mkfifo(path, mode)` 在文件系统中创建一个特殊类型的文件——命名管道（FIFO）。它看起来像一个文件（有路径、有权限），但行为像管道：一个进程往里写，另一个进程从里读，数据在内核缓冲区中流转，不落盘。

```c
mkfifo("/tmp/my_fifo", 0666);
// 进程 A:
int fd_w = open("/tmp/my_fifo", O_WRONLY);
write(fd_w, data, len);
// 进程 B:
int fd_r = open("/tmp/my_fifo", O_RDONLY);
read(fd_r, buf, size);
```

### 为什么需要

匿名管道（`pipe()`）只能在有亲缘关系的进程间使用（父子进程通过 fork 继承 fd）。命名管道通过文件系统路径连接，**任意两个进程**只要知道路径名就能通信，不需要亲缘关系。典型场景是"生产者-消费者"模型，其中两个进程独立启动、独立运行。

重要语义：`open(O_WRONLY)` 会阻塞直到有读者打开读端（反之亦然），这天然提供了同步等待机制。

### 项目中的使用

在这个项目中，MPP 硬件编码器输出 H.265 裸流，FFmpeg 子进程负责把裸流封装成 MKV。FIFO 是连接两者的桥梁：

```c
// 1. 创建 FIFO
snprintf(path, sizeof(path), "/tmp/h265_%s_fifo", cfg_.name);
unlink(fifo_path_.c_str());                          // 清理旧 FIFO
mkfifo(fifo_path_.c_str(), 0666);                    // 创建新 FIFO

// 2. fork FFmpeg 子进程（它会 open FIFO 读端）
ffmpeg_pid_ = fork();
if (ffmpeg_pid_ == 0) {
    execlp("ffmpeg", "ffmpeg", ..., "-i", fifo_path_.c_str(), ...);
}

// 3. 父进程打开写端（阻塞直到 FFmpeg 打开读端）
fifo_fd_ = open(fifo_path_.c_str(), O_WRONLY);       // ★ 这里会阻塞
fifo_fp_ = fdopen(fifo_fd_, "w");
```

数据流：

```
MPP 编码器 → mpp_.put(nv12, fifo_fp_) → FIFO → FFmpeg 子进程 → .mkv 文件
```

关键设计点：
1. FIFO 路径用摄像头名称区分（`/tmp/h265_jhh2_left_fifo`），避免多路冲突
2. 先 fork FFmpeg，再由父进程 open 写端——利用 FIFO 的阻塞语义实现父子同步
3. teardown 时父进程 `fclose(fifo_fp_)`，FFmpeg 读到 EOF 后自动退出，然后 `waitpid` 回收

### 简答题 2.3

**问题**：代码中 `open(fifo_path_.c_str(), O_WRONLY)` 会阻塞直到 FFmpeg 子进程打开读端。如果把这两行代码的顺序反过来——先 `open(O_WRONLY)`，再 `fork+execlp`——会发生什么？

**参考答案**：会导致死锁。`open(O_WRONLY)` 在没有读端时会永久阻塞。如果先 open 写端再 fork，此时 FFmpeg 子进程还不存在，没有进程打开 FIFO 的读端，父进程的 `open(O_WRONLY)` 会永远卡住，程序无法继续。必须先 fork 子进程，让子进程的 `execlp("ffmpeg", ..., "-i", fifo_path, ...)` 去打开读端，父进程的 open 写端才能返回。

---

## 2.4 signal() + sigaction 思想 —— 信号处理

### 是什么

信号是 Unix/Linux 中进程间异步通知的机制。`signal(signum, handler)` 是最简单的注册方式：当进程收到 `signum` 信号时，内核中断当前执行流，调用 `handler` 函数。`sigaction()` 是更现代、更可控的替代品，支持额外选项如 `SA_SIGINFO`（附带更多上下文）、`SA_RESTART`（自动重启被中断的系统调用）、信号屏蔽字等。

**`signal()` vs `sigaction()` 的核心差异**：
- `signal()` 在不同 Unix 变体上行为不一致（BSD 语义 vs SysV 语义），`sigaction()` 是 POSIX 标准，行为确定
- `sigaction()` 可以在处理信号时阻塞其他信号（通过 `sa_mask`）
- `sigaction()` 的 `SA_SIGINFO` 标志让 handler 能收到额外信息（如发送者 PID、faulting address）

### 为什么需要

嵌入式系统至少需要处理三类信号：
1. **终止信号**（SIGINT/SIGTERM）：保证程序优雅退出，释放硬件资源、关闭文件、停止数据流
2. **崩溃信号**（SIGSEGV/SIGABRT）：在死之前打印诊断信息，否则程序默默崩溃，难以定位
3. **忽略信号**（SIGPIPE）：向已关闭的 socket/FIFO 写数据会收到 SIGPIPE，默认行为是杀死进程。在长期运行的服务中应忽略它，改用 `write()` 返回值 + `errno == EPIPE` 来判断

### 项目中的使用

```c
// main.cpp 第 355-359 行
signal(SIGINT,  sig_handler);   // Ctrl+C → 优雅停止
signal(SIGTERM, sig_handler);   // kill → 优雅停止
signal(SIGPIPE, SIG_IGN);       // 忽略 SIGPIPE
signal(SIGSEGV, sig_handler);   // 段错误 → 打印信息后退出
signal(SIGABRT, sig_handler);   // abort() → 打印信息后退出

// 第 46-54 行
static void sig_handler(int sig) {
    if (sig == SIGSEGV || sig == SIGABRT) {
        fprintf(stderr, "\n!!! FATAL: caught signal %d\n", sig);
        fflush(stderr);
        _exit(1);   // 不执行 atexit 清理, 不安全状态直接退出
    }
    g_running = false;
    g_session_running = false;
}
```

设计要点：
1. 对于 SIGINT/SIGTERM，只设置原子标志 `g_running = false`，不在信号处理器中做复杂操作——信号处理器中大部分函数不安全（不可重入）
2. 对 SIGSEGV/SIGABRT，快速输出信息后 `_exit()`，不在崩溃状态下做清理（可能引发二次崩溃）
3. `SIGPIPE` 设为 `SIG_IGN`：向已关闭的 socket 写数据时不会导致程序崩溃

### 简答题 2.4

**问题**：为什么信号处理器 `sig_handler` 中只用 `g_running = false` 这个赋值操作，而不在信号处理器中直接调用 `fflush`、`free`、`pthread_join` 等函数？这背后是什么原则？

**参考答案**：这是"异步信号安全"（async-signal-safe）原则。信号可能在任何时刻到达——包括程序正在执行 `malloc`/`free`、持有互斥锁、操作 FILE 流内部缓冲区的时刻。如果在信号处理器中调用了这些非异步信号安全的函数，可能触发：
- 死锁（信号到达时正好持有 `malloc` 的内部锁，再次调用 `malloc` 会死锁）
- 数据结构损坏（如 FILE 流缓冲区被部分更新）
- 二次崩溃

POSIX 标准列出了一小批保证异步信号安全的函数（如 `write`、`_exit`、`signal`、原子类型的简单赋值）。正确的做法是信号处理器只设置 `volatile sig_atomic_t` 或 `std::atomic<bool>` 标志，让主循环下次检查时安全地处理。

---

## 2.5 backtrace() + backtrace_symbols_fd —— 崩溃回溯

### 是什么

`backtrace()` 和 `backtrace_symbols_fd()` 是 glibc 提供的 GNU 扩展（`<execinfo.h>`），用于在运行时获取当前调用栈。

```c
#include <execinfo.h>

void crash_handler(int sig) {
    void *buffer[64];
    int nptrs = backtrace(buffer, 64);                    // 获取返回地址数组
    backtrace_symbols_fd(buffer, nptrs, STDERR_FILENO);   // 解析符号并输出到 stderr
    _exit(1);
}
```

- `backtrace(buffer, max)` 把当前调用栈的返回地址（PC 指针）写入 `buffer` 数组，返回实际层数
- `backtrace_symbols_fd(buffer, n, fd)` 把这些地址解析成 `函数名+偏移量` 格式，直接写入文件描述符 `fd`，**不需要 malloc**，因此是异步信号安全的

### 为什么需要

嵌入式设备通常在远程无人值守的环境中运行。程序崩溃时没有 gdb 现场调试，日志里只有一句 "Segmentation fault"。有了崩溃回溯，每次 SIGSEGV 时自动打印调用栈，工程师看着栈就能定位到大概哪行代码出了问题——至少知道崩溃发生在哪个模块、哪条调用路径上。

### 项目中的使用

在 `main.cpp` 中已实现（第 47-51 行）：

```cpp
if (sig == SIGSEGV || sig == SIGABRT) {
    void* bt[32];
    int n = backtrace(bt, 32);
    backtrace_symbols_fd(bt, n, STDERR_FILENO);
    fsync(STDERR_FILENO);
    _exit(1);
}
```

输出示例：

```
./unified_capture(+0x3a2c) [0x55555555ba2c]
./unified_capture(+0x3b18) [0x55555555bb18]
/lib/aarch64-linux-gnu/libc.so.6(+0x27340) [0x7f8b04a73340]
./unified_capture(+0x5f80) [0x55555555df80]
```

要在输出中看到函数名，编译时需要加 `-rdynamic` 链接选项。如果 strip 了，可以配合 `addr2line -e ./unified_capture -f 0x5f80` 离线还原。

选择 `backtrace_symbols_fd` 而非 `backtrace_symbols` 的关键原因是：前者内部只调用 `write()`，属于异步信号安全的函数，适合在信号处理器中使用；后者内部会 `malloc`。

### 简答题 2.5

**问题**：`backtrace_symbols_fd` 和 `backtrace_symbols` 都是获取调用栈符号，为什么在信号处理器中只能用 `backtrace_symbols_fd`？两者的内部实现有什么区别？

**参考答案**：`backtrace_symbols()` 返回值是 `char**`，其内部需要调用 `malloc()` 分配内存来存储字符串。信号处理器中不能安全使用 `malloc()`——如果信号到达时程序恰好正在 `malloc()` 内部（持有 arena 锁），再次调用 `malloc()` 会造成死锁。而 `backtrace_symbols_fd()` 内部直接把符号字符串通过 `write()` 系统调用写入文件描述符，不涉及任何堆分配，完全使用栈上的临时缓冲区，因此是异步信号安全的。

---

## 2.6 fcntl (F_GETFL/F_SETFL, O_NONBLOCK) —— 文件描述符属性控制

### 是什么

`fcntl()` (file control) 是一个多功能系统调用，用于操作已打开文件描述符的各种属性。最常用的是通过 `F_GETFL` / `F_SETFL` 获取和设置文件状态标志（file status flags）。

```c
// 获取当前标志
int flags = fcntl(fd, F_GETFL);

// 追加非阻塞标志
fcntl(fd, F_SETFL, flags | O_NONBLOCK);

// 取消非阻塞标志
fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
```

`O_NONBLOCK` 的作用：对 `read()`、`write()`、`accept()` 等操作，如果没有数据可读 / 缓冲区满无法写 / 没有连接可接受，不阻塞等待，而是立即返回 -1 并设 `errno = EAGAIN`（或 `EWOULDBLOCK`）。

### 为什么需要

默认情况下，socket 的 `accept()` 和 `read()` 都是阻塞的——没有新连接或数据时，调用线程会挂起。这对于单线程程序来说是致命的：如果主线程在 `accept()` 上阻塞，就无法同时处理 GPIO 事件、定时器、或者其他 socket 的数据。

解决方案：把 socket 设为非阻塞模式，用 `poll()` 统一管理所有 fd。

### 项目中的使用

**用法一：Socket 非阻塞**（`main.cpp` 第 264 行）

```c
static int socket_setup() {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    // ... bind, listen ...
    int flags = fcntl(fd, F_GETFL);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    return fd;
}
```

配合主循环中的 poll：

```c
pfds[0].fd = gpiod_line_event_get_fd(btn); pfds[0].events = POLLIN;
pfds[1].fd = g_sock_fd;                     pfds[1].events = POLLIN;
int ret = poll(pfds, 2, 200);
if (ret > 0 && pfds[1].revents & POLLIN) {
    int c = accept(g_sock_fd, nullptr, nullptr);  // 非阻塞 accept, 此时肯定有连接
    if (c >= 0) { socket_handle_client(c); close(c); }
}
```

**用法二：摄像头设备非阻塞**（`video_sensor.h` 第 85 行）

```c
dev_fd_ = open(dev_info_.Device_Path, O_RDWR | O_NONBLOCK);
```

TSTC SDK 通过其内部的 event loop 驱动设备 I/O，不需要主线程在 `open()`/`read()` 上阻塞。

设计要点：
1. socket 设为 `O_NONBLOCK` 后，`accept()` 不会阻塞主线程
2. `poll()` 保证只有 fd 就绪时才调 `accept()`，避免了非阻塞模式下的忙轮询
3. `poll` + `O_NONBLOCK` 的组合使得主线程可以同时处理 GPIO 和 socket，不需要多线程

### 简答题 2.6

**问题**：如果把 socket 的 `O_NONBLOCK` 去掉，但保持 `poll` 不变，程序还能正常工作吗？可能会出现什么问题？

**参考答案**：大部分时候仍然能工作，但存在一种危险场景：`poll()` 返回 `POLLIN` 后，在调用 `accept()` 之前的那一瞬间，客户端可能已经断开连接。对于阻塞 socket，`accept()` 会阻塞等待下一个连接，导致主线程卡死——GPIO 按键不再响应、定时器不再触发、整个程序假死。对于非阻塞 socket，`accept()` 立即返回 -1 并设 `errno = EAGAIN`，程序可以继续循环。这就是为什么在 I/O 多路复用模式下，被监听的 fd 应当设为非阻塞——"poll 返回就绪"和"实际执行 I/O"之间存在时间窗口，非阻塞 I/O 保证了在这个窗口内状态变化时不会卡死。

---

## 专题二总结

这六个概念形成了一个完整的系统编程范式——单线程事件驱动架构，核心是 `poll + O_NONBLOCK` 的 I/O 多路复用，配合 `fork+exec` 创建外部辅助进程、`mkfifo` 进程间通信、`signal` 异步事件响应、`backtrace` 崩溃自诊断。在嵌入式设备资源受限、需要长时间稳定运行的场景下，这套范式比多线程方案更简洁、更可调试、更可靠。

---

# 专题三：Unix Domain Socket（6 题）

## 3.1 AF_UNIX vs AF_INET —— Unix Domain Socket 与 TCP Socket 的区别

### 是什么

`AF_UNIX`（也称 `AF_LOCAL`）和 `AF_INET` 是 `socket()` 系统调用的第一个参数，决定了 Socket 的**地址族**（Address Family），即通信双方用什么方式寻址对方。

| | AF_UNIX（Unix Domain Socket） | AF_INET（TCP Socket） |
|---|---|---|
| **寻址方式** | 文件系统路径，如 `/tmp/unified_capture.sock` | IP 地址 + 端口号，如 `127.0.0.1:8080` |
| **通信范围** | 仅限本机进程间 | 可跨主机，也可本机 |
| **数据路径** | 内核内存拷贝，不经过网络协议栈 | 完整 TCP/IP 协议栈（即使本机也走 loopback） |
| **性能** | 更高（零拷贝优化，无 TCP 握手/拥塞控制开销） | 较低（协议栈开销大） |
| **安全性** | 依赖文件权限（`srwxr-xr-x`） | 依赖防火墙 + 端口绑定权限 |

内核实现层面的关键差异：AF_UNIX 在内核中直接通过 `struct unix_sock` 和 `sk_buff` 队列传递数据，旁路了 IP 层分片重组、TCP 拥塞窗口、慢启动等一系列机制。

### 为什么需要

两个核心原因：

**1. 性能。** 同一台机器上的进程通信，如果走 TCP loopback，数据仍然要穿过完整的 TCP/IP 协议栈——封装 TCP 头、计算校验和、经过 netfilter 钩子、ACK 确认等。AF_UNIX 把这些全部跳过，数据直接在内核空间从一个 socket buffer 拷贝到另一个。

**2. 安全与隔离。** AF_UNIX socket 文件受文件系统权限控制。只有对 `.sock` 文件有读写权限的进程才能连接，这比 TCP 端口（任何本机进程都能 `connect 127.0.0.1:port`）更可控。

### 项目中的使用

```c
int fd = socket(AF_UNIX, SOCK_STREAM, 0);
```

这个采集程序需要对外暴露一个控制接口（启动采集、停止采集、查询状态），调用方是同一台 RK3588 上的其他进程（如 web 管理界面、自动测试脚本）。用 AF_UNIX 的理由：
- **不需要跨机访问** —— 控制接口只需要本地进程调用
- **文件权限天然隔离** —— socket 文件放在 `/tmp/unified_capture.sock`
- **低延迟** —— 控制命令是短消息，避免 TCP 握手开销
- **无端口冲突风险** —— 不需要占一个 TCP 端口号

客户端无需写 C 代码，直接用 `nc -U /tmp/unified_capture.sock` 即可连接，方便调试。

### 简答题 3.1

**问题**：如果把本项目的 AF_UNIX 换成 AF_INET（绑定 `127.0.0.1:9999`），会带来哪些具体的问题？至少说出三个，并解释原因。

**参考答案**：

1. **端口占用风险。** TCP 端口是全局资源，`9999` 可能被其他服务占用，导致 `bind` 失败（`EADDRINUSE`）。而 AF_UNIX 用文件路径，只要路径不冲突即可。

2. **TIME_WAIT 问题。** TCP 主动关闭的一方会进入 `TIME_WAIT` 状态（通常 60 秒），频繁的 start/stop 命令可能在短时间内耗尽客户端临时端口，导致 `connect` 失败。AF_UNIX 没有这个问题。

3. **安全性降低。** 任何本机进程都能 `nc 127.0.0.1 9999` 连接控制接口，无法通过文件权限限制调用方。如果有两个用户同时登录 RK3588，非特权用户也能控制摄像头。

4. **延迟增加。** 每个命令（如 `start`）都要经过 TCP 三次握手 + 数据传输 + 四次挥手，频繁调用时延迟累积。AF_UNIX 省去了握手挥手的开销。

（答出任意三点即可。）

---

## 3.2 sockaddr_un —— Unix Socket 地址结构体

### 是什么

`sockaddr_un` 是 POSIX 定义的、专门用于 AF_UNIX 地址族的地址结构体，定义在 `<sys/un.h>`：

```c
struct sockaddr_un {
    sa_family_t sun_family;    // 地址族，必须填 AF_UNIX
    char        sun_path[108]; // socket 文件路径，以 '\0' 结尾
};
```

`sockaddr_un` 的核心价值在于：**用一个文件名就唯一标识了一个通信端点**，比 IP+端口号更直觉，而且天然具备文件权限控制。

Socket API（`bind`、`connect`、`accept` 等）是**多态的**——同一个 `bind()` 函数可以绑定 IPv4 地址（`sockaddr_in`）、IPv6 地址（`sockaddr_in6`）、Unix 路径（`sockaddr_un`）等。为了让这些函数能接受不同类型的地址，所有地址结构体都继承自通用结构体 `struct sockaddr`，实际使用时把 `sockaddr_un*` 强转成 `sockaddr*` 传给函数，内核根据 `sa_family` 字段知道它实际是 `sockaddr_un`。

### 项目中的使用

在 `main.cpp` 的 `socket_setup()` 函数中（第 255-260 行）：

```c
struct sockaddr_un addr;
memset(&addr, 0, sizeof(addr));          // 清零整个结构体
addr.sun_family = AF_UNIX;               // 标记为 Unix Socket
strncpy(addr.sun_path, SOCK_PATH,        // 填入路径
        sizeof(addr.sun_path) - 1);      // 留一个字节给 \0

// 先 connect 试探是否有人监听
int t = socket(AF_UNIX, SOCK_STREAM, 0);
if (t >= 0) {
    if (connect(t, (struct sockaddr*)&addr, sizeof(addr)) < 0)
        unlink(SOCK_PATH);  // 没人监听，清理旧文件
    close(t);
}

bind(fd, (struct sockaddr*)&addr, sizeof(addr));
```

三个值得注意的细节：

1. **`memset` 清零是必须的。** `sun_path` 剩余的未使用字节如果不是 0，内核可能把后面的垃圾数据当作路径名的一部分，导致实际 bind 路径与预期不符。这是 Unix Socket 编程最常见的坑之一。

2. **`strncpy` 而非 `strcpy`。** 防止路径名过长导致缓冲区溢出。

3. **先 connect 试探。** 如果上次程序异常退出（被 `kill -9`），socket 文件还残留在磁盘上。如果没人监听（connect 失败），安全删除旧文件再 bind；如果有人监听（connect 成功），不能 unlink。

### 简答题 3.2

**问题**：如果代码中去掉了 `memset(&addr, 0, sizeof(addr))` 这一行，但其他代码不变，程序行为会有什么变化？在什么条件下会出问题？

**参考答案**：去掉 `memset` 后，`addr` 是栈上的局部变量，其内存内容是未初始化的垃圾值。`strncpy(addr.sun_path, SOCK_PATH, sizeof(addr.sun_path) - 1)` 只覆盖了 `sun_path` 的前 `strlen(SOCK_PATH)` 个字节。如果 `SOCK_PATH = "/tmp/unified_capture.sock"`（26 字节），则 `sun_path[26]` 到 `sun_path[107]` 之间是栈上的残留数据。

具体后果取决于残留内容：
- **如果残留字节恰好都是 0（概率极低），** bind 行为正常。
- **如果残留字节中有非零值（大概率），** 内核在解析 `sun_path` 时扫描到第一个 `\0` 才停止，会认为实际路径名更长或不同。这会导致 `bind` 创建的文件名与预期的 `SOCK_PATH` 不一致，后续 `nc -U /tmp/unified_capture.sock` 无法连接。

---

## 3.3 socket() + bind() + listen() + accept() —— 服务端四步曲

### 是什么

这是 TCP/AF_UNIX 流式服务端的标准创建流程，每一步都是不可跳过的状态转换：

```
socket()         创建 socket，分配内核资源，返回 fd
    ↓
bind()           将 socket 绑定到一个地址（路径或 IP:Port）
    ↓
listen()         将 socket 从 CLOSED 状态切换为 LISTEN 状态，指定 backlog
    ↓
accept()         从已完成连接的队列中取出一个客户端连接，返回新的 fd
```

**`accept()` 返回独立 fd 的设计使得服务端可以同时服务多个客户端**——每个客户端有自己独立的 fd，配合 I/O 多路复用（`poll`/`epoll`）同时监听所有 client fd。

### 项目中的使用

在 `main.cpp` 的 `socket_setup()` 函数中（第 255-266 行），四步完整呈现：

```c
int fd = socket(AF_UNIX, SOCK_STREAM, 0);         // 步骤1: 创建 socket
// ...构建 addr...
bind(fd, (struct sockaddr*)&addr, sizeof(addr));   // 步骤2: 绑定路径
listen(fd, 4);                                     // 步骤3: 开始监听，backlog=4
int flags = fcntl(fd, F_GETFL);
fcntl(fd, F_SETFL, flags | O_NONBLOCK);            // 设为非阻塞
```

然后 `accept()` 出现在三个地方（主循环、socket 模式循环、session 内的 poll 循环），每次都是同一个模式：

```c
int c = accept(g_sock_fd, nullptr, nullptr);  // 步骤4: 接受连接
if (c >= 0) {
    socket_handle_client(c);  // 处理一个命令→响应
    close(c);                 // 用完即关
}
```

注意两点设计决策：
1. **`backlog` 设为 4**：因为只有操作员偶尔发个命令，并发连接极少，4 完全够用。
2. **`accept()` 后立即 close**：这是"一问一答"的短连接模式。每个命令独立连接，服务器处理完立即关闭。不需要维护长连接状态机，简单可靠。

### 简答题 3.3

**问题**：为什么 `accept()` 返回一个新的 fd，而不是在原来的 `listen_fd` 上直接收发数据？如果设计成在 listen_fd 上直接收发，会出现什么问题？

**参考答案**：**根本原因：一个服务端需要同时服务多个客户端。** 如果只在 `listen_fd` 上收发数据，那么同时只能有一个客户端连接——因为 `listen_fd` 只有一个，所有数据混在一起，无法区分来自哪个客户端。

`accept()` 返回独立 fd 的设计实现了"连接复用"：
- `listen_fd` 专职监听新连接，永远不用于收发数据。
- 每个 `client_fd` 代表一个独立的客户端连接，有自己的读写缓冲区、接收队列和发送队列。
- 服务端可以在事件循环中同时轮询 `listen_fd`（有新连接）和多个 `client_fd`（有数据到达），实现并发服务。

---

## 3.4 SOCK_STREAM vs SOCK_DGRAM —— 流式 vs 数据报

### 是什么

`socket()` 的第二个参数，决定 Socket 的**传输语义**：

| | SOCK_STREAM（流式） | SOCK_DGRAM（数据报） |
|---|---|---|
| **类比** | 电话（TCP） | 信件（UDP） |
| **连接模型** | 面向连接，先 connect/accept 再通信 | 无连接，直接 sendto/recvfrom |
| **数据边界** | 无边界，字节流。发送 3 次 100 字节，对方可能一次 recv 收到 300 字节 | 保留边界。发送 3 次 100 字节，对方必须 recv 3 次 |
| **可靠性** | 可靠有序 | 不可靠（但 AF_UNIX+DGRAM 是可靠的） |

一个关键知识点：**AF_UNIX + SOCK_DGRAM 是可靠的**，不会丢包。这与 AF_INET + SOCK_DGRAM（UDP，不可靠）有本质区别。

### 项目中的使用

```c
int fd = socket(AF_UNIX, SOCK_STREAM, 0);
```

本项目选择 **SOCK_STREAM**，原因：

1. **命令是变长文本。** `start` 很短（5 字节），但 `status` 返回的 JSON 可能很长（几百字节）。STREAM 可以自动处理任意长度。
2. **换行符天然处理粘包。** 应用层协议用 `\n` 分隔命令和响应，在 STREAM 之上很容易实现"读一行"的逻辑。
3. **服务端模型一致。** 本项目的服务端需要 `listen` + `accept` 才能区分多个客户端。SOCK_STREAM 天然支持这个模型。

### 简答题 3.4

**问题**：在本项目的 SOCK_STREAM 场景下，客户端一次性发送 `"start\nstop\n"`（两条命令粘在一起），服务端的协议解析代码需要对这种情况做什么处理？如果处理不当会出现什么 bug？

**参考答案**：服务端必须实现**行缓冲读取**——即循环调用 `recv()`，将收到的数据追加到内部缓冲区，然后逐行扫描 `\n`，每次遇到 `\n` 就提取一行作为一条完整命令处理。

对于 `"start\nstop\n"` 这个输入：
1. 第一次 `recv` 可能收到 `"sta"`（STREAM 不保证边界），加入缓冲区。
2. 扫描缓冲区，没有 `\n`，继续等待。
3. 第二次 `recv` 收到 `"rt\nstop\n"`，加入缓冲区后变成 `"start\nstop\n"`。
4. 扫描到第一个 `\n`，提取 `"start"` 作为第一条命令执行。
5. 继续扫描，遇到第二个 `\n`，提取 `"stop"` 作为第二条命令执行。

**如果处理不当——比如每次 recv 后直接把收到的数据当作一条完整命令**，第一次 recv 拿到 `"sta"` 会当作命令 `"sta"` 解析，协议不认识，返回错误。第二次 recv 拿到 `"rt\nstop\n"`，命令解析混乱。这就是经典的 **TCP 粘包/拆包问题**。

---

## 3.5 纯文本控制协议 —— 换行分隔的 JSON 命令/响应

### 是什么

这是一种应用层协议设计模式：通信双方用**人类可读的文本**（而非二进制）交换控制消息，以**换行符 `\n`** 作为消息边界，消息体使用 **JSON** 格式。

本项目定义了三条命令：

```
请求（客户端 → 服务端）         响应（服务端 → 客户端）
─────────────────────────────────────────────────────
start\n              →         {"ok":true}\n
stop\n               →         {"ok":true,"elapsed_ms":8032}\n
status\n             →         {"ok":true,"ready":true,"running":false,...}\n
```

每条响应都带 `"ok":true/false` 字段——这是一个好的协议设计实践。客户端可以用统一逻辑判断命令是否成功。

### 为什么需要

1. **可调试性第一**：`echo "start" | nc -U /tmp/unified_capture.sock` 就能发命令，出问题时不需要专用工具就能排查。
2. **JSON 是事实标准**：几乎每种语言都有 JSON 库。`"ok":true` 这种结构让客户端只需要判断 `ok` 字段就能知道命令执行结果。
3. **换行符 `\n` 是最简单的消息边界**：在 `SOCK_STREAM` 字节流上，你必须在某处划边界才能知道"一条消息结束了"。
4. **协议版本演化友好**：如果以后要加字段，在 JSON 里加就行了，老客户端忽略未知字段即可。

### 项目中的使用

核心处理逻辑在 `socket_handle_client()` 函数（第 222-252 行）：

```c
static void socket_handle_client(int fd) {
    char buf[256];
    ssize_t n = read(fd, buf, sizeof(buf)-1);   // 1. 读取
    if (n <= 0) return;
    buf[n] = '\0';
    if (buf[n-1]=='\n') buf[n-1]='\0';           // 2. 去换行符

    std::string resp;
    if (!strcmp(buf, "start")) {                  // 3. 精确字符串匹配
        // ...构造响应 JSON...
    } else if (!strcmp(buf, "stop")) {
        // ...
    } else if (!strcmp(buf, "status")) {
        // ...
    } else {
        resp = "{\"ok\":false,\"error\":\"unknown command\"}";  // 4. 兜底
    }
    resp += "\n";                                  // 5. 加上换行
    write(fd, resp.c_str(), resp.size());          // 6. 写回
}
```

### 简答题 3.5

**问题**：为什么协议选择了 `\n` 作为消息分隔符，而不是用固定长度（比如每条命令固定 256 字节）或者长度前缀（前 4 字节是消息长度）？每种方案在本项目中的优缺点是什么？

**参考答案**：

| 方案 | 优点 | 缺点（本项目语境下） |
|---|---|---|
| **换行分隔** `\n` | 支持 `nc` 直接交互调试；消息变长无浪费；实现极简；人可以手打命令 | 消息内容不能包含 `\n`（本项目命令不含换行，无影响）；需要逐字节扫描 |
| **固定长度**（如 256 字节） | 解析最简单，一次 `recv` 读完；没有粘包问题 | 浪费带宽（`"start"` 占 256 字节）；未来扩展受限；`nc` 调试不方便 |
| **长度前缀**（如 4 字节头） | 无浪费；支持任意二进制内容；性能最优 | 无法用 `nc` 调试（无法手算并输入二进制长度头）；实现复杂；引入大端/小端问题 |

**本项目选 `\n` 的核心考量：可调试性 > 性能。** 控制协议的吞吐量极低（每秒最多几个命令），性能差异完全可以忽略。但"能用 `nc` 手动测试"这个能力在嵌入式开发和现场调试中无价。

---

## 3.6 O_NONBLOCK + poll —— 非阻塞 Socket 配合事件循环

### 是什么

**O_NONBLOCK** 是 fd 的一个标志位。设了 `O_NONBLOCK` 后：
- `read()` 如果没有数据可读，不阻塞等待，立即返回 `-1` 并设 `errno = EAGAIN/EWOULDBLOCK`。
- `accept()` 如果没有新连接，同样立即返回 `-1` + `EAGAIN`。

**`O_NONBLOCK` + `poll()` 是一对黄金搭档**：
- `O_NONBLOCK` 保证单次 I/O 调用不会卡住整个程序。
- `poll()` 作为"前哨"，只在数据就绪时才去调用 I/O，避免无意义的 `EAGAIN` 循环。

### 为什么需要

如果只用一个阻塞 socket：
```c
int c = accept(fd, NULL, NULL);  // 阻塞！
```
那么当没有客户端连接时，整个程序都卡在这行代码上，摄像头数据没人读，IMU 队列没人消费，系统瘫痪。

`O_NONBLOCK + poll()` 的**单线程事件循环**模型：所有事情在一个线程里串行完成——网络 I/O、命令处理、GPIO 监控。没有锁，没有竞态条件，逻辑清晰，bug 少。

### 项目中的使用

```c
// 设 listen_fd 为非阻塞
int flags = fcntl(fd, F_GETFL);
fcntl(fd, F_SETFL, flags | O_NONBLOCK);

// 事件循环
struct pollfd fds[MAX_CLIENTS + 1];
fds[0].fd = listen_fd;
fds[0].events = POLLIN;

while (running) {
    int ret = poll(fds, nfds, poll_timeout_ms);
    if (ret < 0) {
        if (errno == EINTR) continue;  // 信号中断，重试
        break;
    }
    if (fds[0].revents & POLLIN) {
        int cfd = accept(listen_fd, NULL, NULL);
        if (cfd >= 0) {
            int cflags = fcntl(cfd, F_GETFL);
            fcntl(cfd, F_SETFL, cflags | O_NONBLOCK);  // 新 client 也设非阻塞
        }
    }
    // ...处理每个 client_fd...
}
```

重要的非阻塞编程要点：

1. **`accept` 返回的新 client fd 也必须设 `O_NONBLOCK`。** 新 fd 继承 listen_fd 的非阻塞标志在某些系统上不可靠，显式设置是最稳妥的做法。
2. **`EINTR` 处理。** `poll` 可能被信号中断返回 -1，此时应该重试而非退出循环。
3. **`recv` 返回 0 表示对端关闭。** 这是 SOCK_STREAM 的规则——FIN 到达后 `recv` 返回 0，不是 -1。必须正确区分"连接关闭"和"暂时无数据（EAGAIN）"。

### 简答题 3.6

**问题**：如果在这个项目中去掉 `O_NONBLOCK`，只保留 `poll`（fd 使用默认的阻塞模式），会出现什么具体的 bug？描述一个场景，说明在哪一步会出问题。

**参考答案**：

场景：服务端已有一个客户端连接，正在运行。第二个客户端连接进来，发送 `status\n` 命令后立即关闭连接（不等待响应）。

**阻塞模式下的执行流程：**
1. `poll()` 返回，报告 `client_fd` 可读，有新数据。
2. 处理第一个客户端的数据，开始 `send()` 回复。
3. 如果第一个客户端的 TCP 发送缓冲区满了，`send()` 会**阻塞**，整个事件循环卡住。
4. 在 `send()` 阻塞期间：第二个客户端的新连接到达（`listen_fd` 上有 `POLLIN`），但线程卡在 `send()` 里，`poll()` 根本没机会再次被调用。`accept()` 无法执行。
5. 多个客户端同时连接时，一个慢客户端会拖死整个服务。

更糟糕的场景——如果恶意客户端不断 `connect` 但从不发送数据，阻塞的 `recv()` 会永久挂起事件循环。非阻塞 + `poll` 的模式下，没数据的客户端不会阻塞 `recv()`，`poll` 只轮询"真正的数据"，配合超时保证了采集循环的执行。

**总结：`O_NONBLOCK` 让每个 I/O 操作都是"尽力而为，不等待"，把"等待"的职责完全交给 `poll()`，确保一个线程能公平服务所有 fd 和周期性任务。**

---

## 专题三总结

这六个概念构成了一条完整的技术链路：

```
客户端发出 "start\n"
        │
        ▼
    AF_UNIX SOCK_STREAM（概念 3.1 + 3.4）
    文件路径寻址 /tmp/unified_capture.sock（概念 3.2）
        │
        ▼
    poll() 检测到 listening fd 可读（概念 3.6 O_NONBLOCK+poll）
        │
        ▼
    accept() 拿到 connected fd（概念 3.3）
        │
        ▼
    read() → 去换行符 → 字符串匹配 "start"（概念 3.5 文本协议）
        │
        ▼
    write("{\"ok\":true}\n") → close(fd)
```

每一步设计都有明确的原因，是"麻雀虽小五脏俱全"的经典实现。

---

# 全部 17 道题目汇总

## 专题一：C++ 多线程

1. **悬挂引用**：lambda 通过 `[this, &gate]` 捕获 gate 引用，如果 gate 提前析构会怎样？本项目为什么安全？
2. **临界区范围**：lock_guard 的大括号去掉会有什么后果？
3. **条件变量 predicate**：`cv_.wait(lock)` 不加 predicate 有什么风险？
4. **memory_order_relaxed**：用 relaxed 内存序替代默认 seq_cst 会导致什么问题？
5. **pthread_create 后的 usleep**：为什么加 200ms sleep？去掉会怎样？

## 专题二：Linux 系统编程

6. **poll timeout**：把 50ms 超时改成 -1 会发生什么？
7. **_exit vs exit**：execlp 失败后为什么用 `_exit(1)` 而不是 `exit(1)`？
8. **mkfifo open 顺序**：先 open(O_WRONLY) 再 fork 会发生什么？
9. **异步信号安全**：为什么信号处理器只用 `g_running = false`？
10. **backtrace_symbols_fd vs backtrace_symbols**：为什么信号处理器只能用前者？
11. **O_NONBLOCK 去掉的风险**：去掉 O_NONBLOCK 但保留 poll 会有什么问题？

## 专题三：Unix Domain Socket

12. **AF_UNIX vs AF_INET**：换成 TCP Socket 会带来哪些问题？（至少三个）
13. **sockaddr_un 的 memset**：去掉 memset 会怎样？
14. **accept 返回新 fd 的原因**：为什么不在 listen_fd 上直接收发？
15. **SOCK_STREAM 粘包**：客户端发 "start\nstop\n" 需要怎么处理？
16. **协议分隔符选择**：为什么选 \n 而不是固定长度或长度前缀？
17. **O_NONBLOCK + poll 的必要性**：去掉 O_NONBLOCK 会出什么 bug？
