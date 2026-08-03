//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

import { spawn } from "node:child_process";
import { readFile, unlink } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { fileURLToPath } from "node:url";

import { afterEach, describe, expect, it } from "vitest";

import { isProcessAlive, killProcessTree } from "../src/process-tree.js";

const treeHostPath = fileURLToPath(new URL("./helpers/tree-host.mjs", import.meta.url));

async function waitForFile(path: string, timeoutMs = 5000): Promise<string> {
  const started = Date.now();
  while (Date.now() - started < timeoutMs) {
    try {
      const text = (await readFile(path, "utf8")).trim();
      if (text.length > 0) return text;
    } catch {
      // not yet
    }
    await new Promise((resolve) => setTimeout(resolve, 50));
  }
  throw new Error(`Timed out waiting for ${path}`);
}

describe("killProcessTree", () => {
  const files: string[] = [];

  afterEach(async () => {
    for (const file of files) {
      try {
        await unlink(file);
      } catch {
        // ignore
      }
    }
    files.length = 0;
  });

  it("KillProcessTreeTerminatesParentAndLeafWithNoOrphan", async () => {
    const parentPidFile = join(tmpdir(), `ui-fuzz-parent-${process.pid}-${Date.now()}.pid`);
    const leafPidFile = join(tmpdir(), `ui-fuzz-leaf-${process.pid}-${Date.now()}.pid`);
    files.push(parentPidFile, leafPidFile);

    const child = spawn(
      process.execPath,
      [treeHostPath, "--parent-pid-file", parentPidFile, "--leaf-pid-file", leafPidFile],
      { stdio: ["ignore", "pipe", "pipe"], windowsHide: true },
    );

    const parentPid = Number.parseInt(await waitForFile(parentPidFile), 10);
    const leafPid = Number.parseInt(await waitForFile(leafPidFile), 10);
    expect(parentPid).toBeGreaterThan(0);
    expect(leafPid).toBeGreaterThan(0);
    expect(isProcessAlive(parentPid)).toBe(true);
    expect(isProcessAlive(leafPid)).toBe(true);

    killProcessTree(child.pid ?? parentPid);

    const deadline = Date.now() + 5000;
    while (Date.now() < deadline && (isProcessAlive(parentPid) || isProcessAlive(leafPid))) {
      await new Promise((resolve) => setTimeout(resolve, 50));
    }

    expect(isProcessAlive(parentPid)).toBe(false);
    expect(isProcessAlive(leafPid)).toBe(false);
  });
});
