#!/bin/bash
# wrist_check.sh — 检查 USB 拓扑：外部 Hub 2.0 和摄像头状态

echo "========================================"
echo "  USB 拓扑检查 — $(date '+%Y-%m-%d %H:%M:%S')"
echo "========================================"

# 外部 USB Hub（排除 root hub）
echo ""
echo ">>> 外部 USB Hub:"
lsusb | grep -i hub | grep -v "Linux Foundation" | while read line; do
    bus=$(echo "$line" | awk '{print $2}')
    dev=$(echo "$line" | awk '{print $4}' | tr -d ':')
    vid_pid=$(echo "$line" | awk '{print $6}')
    name=$(echo "$line" | cut -d' ' -f7-)
    echo "  Bus $bus Dev $dev  $vid_pid  $name"
done

# 摄像头（通过 v4l2-ctl 检测）
echo ""
echo ">>> 摄像头设备:"
for dev in /dev/video*; do
    [ -e "$dev" ] || continue
    card=$(v4l2-ctl -D -d "$dev" 2>/dev/null | grep "Card type" | cut -d':' -f2- | xargs)
    [ -n "$card" ] && echo "  $dev  →  $card"
done

# USB 树形拓扑
echo ""
echo ">>> 设备树:"
lsusb -t 2>/dev/null

# 汇总
echo ""
ext_hub=$(lsusb | grep -i hub | grep -v "Linux Foundation" | wc -l | xargs)
cam_count=$(ls /dev/video* 2>/dev/null | wc -l | xargs)
echo ">>> 汇总: 外部Hub=${ext_hub} 个, 视频节点=${cam_count} 个"
echo "========================================"
