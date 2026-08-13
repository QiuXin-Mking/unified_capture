# app/commands — 命令字扩展模块

## 概述

本目录是 Socket 命令字的**扩展入口**。当前命令字定义在 `app/socket_server.h`（枚举 + 结构体）和 `app/socket_server.cpp`（解析），分发逻辑在 `app/runtime.cpp` 的 `handle_socket_command` lambda 中。

随着命令字增多，新增命令应遵循本目录的扩展约定。

## 当前命令字

| 命令 | 枚举 | 参数 | 说明 |
|------|------|------|------|
| `start` | `SocketCommandKind::start` | 无 | 启动采集 |
| `stop` | `SocketCommandKind::stop` | 无 | 停止采集 |
| `status` | `SocketCommandKind::status` | 无 | 查询状态 |
| `preview:<path>` | `SocketCommandKind::preview` | `preview_path` | 导出预览帧 |

## 如何新增命令字

### 步骤 1：在 `SocketCommandKind` 添加枚举

```cpp
// app/socket_server.h
enum class SocketCommandKind { start, stop, preview, status, your_new_cmd, unknown };
```

### 步骤 2：如需参数，在 `SocketCommand` 添加字段

```cpp
// app/socket_server.h
struct SocketCommand {
    SocketCommandKind kind;
    std::string preview_path;
    std::string your_new_param;  // 新增
};
```

### 步骤 3：在 `parse_socket_command()` 添加解析

```cpp
// app/socket_server.cpp
if (request == "your_new_cmd") {
    return {SocketCommandKind::your_new_cmd, {}};
}
// 带参数的命令：
constexpr std::string_view your_prefix = "your_cmd:";
if (request.starts_with(your_prefix) && request.size() > your_prefix.size()) {
    return {SocketCommandKind::your_new_cmd,
            {}, std::string(request.substr(your_prefix.size()))};
}
```

### 步骤 4：在 `handle_socket_command` 添加处理

```cpp
// app/runtime.cpp
if (command.kind == SocketCommandKind::your_new_cmd) {
    // 处理逻辑
    return std::string("{\"ok\":true}");
}
```

### 步骤 5：更新文档

- 更新 `docs/socket-control.md` 的命令表格
- 更新 `app/commands/README.md` 的命令列表

## 交互约定

- 请求：UTF-8 纯文本，以 `\n` 结束
- 响应：单行 JSON，以 `\n` 结束
- 短连接：每个连接一条命令，一次响应后关闭
- 未知命令返回 `{"ok":false,"error":"unknown command"}`

## 相关文件

- `../socket_server.h` — 命令枚举与结构体定义
- `../socket_server.cpp` — 命令解析
- `../runtime.cpp` — 命令分发与执行
- `../../docs/socket-control.md` — Socket 协议完整文档