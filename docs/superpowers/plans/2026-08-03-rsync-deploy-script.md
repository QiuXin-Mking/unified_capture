# RK3588 Source Sync Script Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a safe one-command rsync script that copies the local source tree to `root@192.168.100.200:/root/unified_capture/`.

**Architecture:** A Bash script resolves the repository root relative to itself, creates the fixed remote directory over SSH, and invokes rsync with explicit exclusions and without deletion. A shell test replaces SSH and rsync through `PATH` to verify the command contract without contacting the board.

**Tech Stack:** Bash, SSH, rsync

## Global Constraints

- The destination is `root@192.168.100.200:/root/unified_capture/`.
- Do not use rsync `--delete`.
- Exclude `.git/`, `.worktrees/`, `build/`, `.DS_Store`, and the repository-root `unified_capture` binary.
- The script must work regardless of the caller's current directory.

---

### Task 1: Add and verify the sync script

**Files:**
- Create: `deploy/sync_to_rk3588.sh`
- Create: `tests/test_sync_to_rk3588.sh`

**Interfaces:**
- Consumes: local repository tree and passwordless or otherwise configured SSH access.
- Produces: an executable command `deploy/sync_to_rk3588.sh` that synchronizes source files to the fixed RK3588 destination.

- [ ] **Step 1: Write the failing behavior test**

  Create fake `ssh` and `rsync` commands in a temporary directory, run the deployment script from outside the repository, and assert the captured argument list contains the fixed host/path and all exclusions but not `--delete`.

- [ ] **Step 2: Run the test to verify it fails**

  Run: `bash tests/test_sync_to_rk3588.sh`

  Expected: FAIL because `deploy/sync_to_rk3588.sh` does not exist.

- [ ] **Step 3: Write the minimal implementation**

  Implement a strict-mode Bash script that resolves its source root, creates the remote directory, and runs rsync with the approved options and exclusions.

- [ ] **Step 4: Run verification**

  Run: `bash tests/test_sync_to_rk3588.sh && bash -n deploy/sync_to_rk3588.sh && git diff --check`

  Expected: all commands exit zero.
