# RK3588 源码同步脚本设计

## 目标

在 `deploy/` 中提供一个可从任意工作目录运行的脚本，将当前 `unified_capture` 源码树增量同步到 `root@192.168.100.200:/root/unified_capture/`。

## 设计

- 脚本根据自身位置计算仓库根目录，不依赖调用者的当前目录。
- 先通过 SSH 执行 `mkdir -p /root/unified_capture`，确保首次同步也可用。
- 使用 rsync 的 archive、压缩、人类可读输出和进度选项同步目录内容；进度参数使用兼容 macOS rsync 2.6.9 的 `--progress`。
- 排除 `.git/`、`.worktrees/`、`build/`、`.DS_Store` 和根目录 `unified_capture` 编译产物，避免传输仓库元数据、本地中间产物或错误架构的二进制。
- 默认不使用 `--delete`，不删除板端独有文件。
- 任一 SSH 或 rsync 命令失败时立即退出非零状态。

## 验证

通过临时的假 `ssh` 和假 `rsync` 捕获实际参数，验证远端目录、本地源目录、目标目录、排除规则及无 `--delete`；另用 `bash -n` 检查语法。
