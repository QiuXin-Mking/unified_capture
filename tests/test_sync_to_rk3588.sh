#!/usr/bin/env bash
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SCRIPT="$PROJECT_ROOT/deploy/sync_to_rk3588.sh"
TEST_TMP="$(mktemp -d)"
trap 'rm -rf "$TEST_TMP"' EXIT

mkdir -p "$TEST_TMP/bin" "$TEST_TMP/run-from-here"

cat >"$TEST_TMP/bin/ssh" <<'EOF'
#!/usr/bin/env bash
printf '%s\n' "$@" >"$SYNC_TEST_LOG/ssh.args"
EOF

cat >"$TEST_TMP/bin/rsync" <<'EOF'
#!/usr/bin/env bash
printf '%s\n' "$@" >"$SYNC_TEST_LOG/rsync.args"
EOF

chmod +x "$TEST_TMP/bin/ssh" "$TEST_TMP/bin/rsync"

(
    cd "$TEST_TMP/run-from-here"
    PATH="$TEST_TMP/bin:$PATH" SYNC_TEST_LOG="$TEST_TMP" bash "$SCRIPT"
)

assert_line() {
    local expected="$1"
    local file="$2"
    if ! grep -Fqx -- "$expected" "$file"; then
        echo "missing argument '$expected' in $file" >&2
        exit 1
    fi
}

assert_line "root@192.168.100.200" "$TEST_TMP/ssh.args"
assert_line "mkdir -p -- /root/unified_capture" "$TEST_TMP/ssh.args"

assert_line "-ahvz" "$TEST_TMP/rsync.args"
assert_line "--progress" "$TEST_TMP/rsync.args"
assert_line "--exclude=/.git/" "$TEST_TMP/rsync.args"
assert_line "--exclude=/.worktrees/" "$TEST_TMP/rsync.args"
assert_line "--exclude=/build/" "$TEST_TMP/rsync.args"
assert_line "--exclude=.DS_Store" "$TEST_TMP/rsync.args"
assert_line "--exclude=/unified_capture" "$TEST_TMP/rsync.args"
assert_line "$PROJECT_ROOT/" "$TEST_TMP/rsync.args"
assert_line "root@192.168.100.200:/root/unified_capture/" "$TEST_TMP/rsync.args"

if grep -Fqx -- "--delete" "$TEST_TMP/rsync.args"; then
    echo "unsafe --delete option found" >&2
    exit 1
fi

echo "sync_to_rk3588 behavior test passed"
