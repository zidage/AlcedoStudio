//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

/**
 * Cross-platform process-tree teardown. A plain `ChildProcess.kill()` only
 * signals the direct child; on Windows the Qt host can leave helper processes
 * behind, and on POSIX a shell-wrapped spawn can leave a grandchild. Phase 3
 * acceptance requires stop/kill to leave no orphans.
 */

import { spawnSync } from "node:child_process";

/**
 * Kills `pid` and every descendant. On Windows this uses `taskkill /T /F`. On
 * POSIX it first tries the process-group id (negative pid) then falls back to
 * the single process. Best-effort: failures are ignored when the process is
 * already gone.
 */
export function killProcessTree(pid: number | undefined): void {
  if (pid === undefined || pid <= 0) return;

  if (process.platform === "win32") {
    spawnSync("taskkill", ["/pid", String(pid), "/T", "/F"], {
      windowsHide: true,
      stdio: "ignore",
    });
    return;
  }

  try {
    process.kill(-pid, "SIGKILL");
  } catch {
    try {
      process.kill(pid, "SIGKILL");
    } catch {
      // Already exited.
    }
  }
}

/**
 * Returns true when a process with `pid` is still alive. Used by tests to
 * assert that stop/kill left no orphans.
 */
export function isProcessAlive(pid: number): boolean {
  if (pid <= 0) return false;
  try {
    process.kill(pid, 0);
    return true;
  } catch (error) {
    const err = error as NodeJS.ErrnoException;
    // EPERM means the process exists but we lack permission to signal it.
    return err.code === "EPERM";
  }
}
