# 架构决策记录 (ADR)

## 001: 多 Session 稳定性 — `--single` + systemd 重启

**日期:** 2026-07-25

**状态:** 已采纳

**问题:**

同一进程内连续跑多个录制 session 时，TSTC SDK (v1.0.0, 2022-03-24) 的 `STREAM_STATUS(1)` 在第二轮或第三轮 session 的 setup 阶段死锁，SIGTERM 无法杀死进程。

**排查过程:**

1. 怀疑 MPP 编码器崩溃 → 实际是 nv12 缓冲区溢出（jhh02 实际输出 4000×1200 而非配置的 3104×480）
2. 怀疑 CMA 不足 → 扩到 256M 但问题依旧（只是消除了 `mpp_buffer_group_init` 断言）
3. 怀疑 SDK 清理函数调用问题 → 注释掉 UNINIT/DELETE 无改善
4. 怀疑缺少 `STREAM_STATUS(0)` 配对 → 部分有效（Session 2 不死锁了），但 Session 3 仍死锁
5. 怀疑 fd 泄漏 → 显式 close() 无改善

**根因分析:**

SDK 内部维护全局静态状态表，`CREATE_DEVICE_POINT` → `DELETE_DEVICE_POINT` 的完整生命周期调用后，内部状态未能完全归零。SDK 原始设计假设设备节点长期持有，不支持频繁的创建-销毁循环。

**考虑过的方案:**

| 方案 | 优点 | 缺点 | 决策 |
|------|------|------|------|
| `--single` + systemd Restart | SDK 状态彻底重置，零死锁风险 | 3-5s 重启间隙 | ✅ 采纳 |
| Always-streaming（流只启不停） | 零间隙切换 | 空闲功耗高（4路 MJPEG 解码空转），代码改动大 | ❌ 拒绝 |
| 修复 SDK 生命周期（补充缺失 API 调用） | 理想方案 | SDK 无源码无文档，无法确认内部状态机 | ❌ 不可行 |

**最终方案:**

- 使用 `--single` 参数，每次 session 结束后进程主动退出
- systemd `Restart=always` + `RestartSec=5` 自动重新拉起
- Session 编号通过扫描已有目录递增，进程重启后数据不覆盖
- SDK 的 CREATE/INIT/DEAL/UNINIT/DELETE 完整调用链保持不变（即使 --single 下这些调用冗余，保留是为了非 --single 调试场景）

**代价:**

- 每次 session 之间有 3-5 秒 socket 不可用窗口
- 对于数据采集场景，这个间隙可接受

**妥协:**

理想方案是同进程内支持多 session，但 TSTC SDK 的黑盒性质使得这项投入不值得。在 SDK 无源码无文档的前提下，进程重启是最可靠的隔离手段。
