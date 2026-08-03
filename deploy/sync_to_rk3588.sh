#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SOURCE_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
REMOTE_HOST="root@192.168.100.200"
REMOTE_DIR="/root/unified_capture"

command -v ssh >/dev/null 2>&1 || {
    echo "error: ssh is not installed or not in PATH" >&2
    exit 1
}
command -v rsync >/dev/null 2>&1 || {
    echo "error: rsync is not installed or not in PATH" >&2
    exit 1
}

echo "Syncing $SOURCE_DIR/ to $REMOTE_HOST:$REMOTE_DIR/"

ssh "$REMOTE_HOST" "mkdir -p -- $REMOTE_DIR"

rsync -ahvz --progress \
    --exclude='/.git' \
    --exclude='/.git/' \
    --exclude='/.worktrees/' \
    --exclude='/build/' \
    --exclude='.DS_Store' \
    --exclude='/unified_capture' \
    "$SOURCE_DIR/" \
    "$REMOTE_HOST:$REMOTE_DIR/"

echo "Sync complete: $REMOTE_HOST:$REMOTE_DIR/"
