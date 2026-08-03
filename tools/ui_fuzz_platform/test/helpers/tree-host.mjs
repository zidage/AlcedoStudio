#!/usr/bin/env node
//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

/**
 * Minimal process-tree fixture for kill tests. Spawns a long-lived leaf child,
 * writes both PIDs to files, prints PROBE_SOCKET, then sleeps until killed.
 * It does not implement the probe protocol — callers that only need spawn/kill
 * semantics should use this; full walk tests use fake-host.mjs.
 */

import { spawn } from "node:child_process";
import { writeFileSync } from "node:fs";

function parseArgs(argv) {
  const out = {};
  for (let i = 0; i < argv.length; i++) {
    const arg = argv[i];
    if (arg.startsWith("--")) {
      const key = arg.slice(2);
      out[key] = argv[++i];
    }
  }
  return out;
}

const args = parseArgs(process.argv.slice(2));
const parentPidFile = args["parent-pid-file"];
const leafPidFile = args["leaf-pid-file"];
if (!parentPidFile || !leafPidFile) {
  process.stderr.write("tree-host requires --parent-pid-file and --leaf-pid-file\n");
  process.exit(2);
}

const leaf = spawn(
  process.execPath,
  [
    "-e",
    `require('fs').writeFileSync(process.env.LEAF_PID_FILE, String(process.pid)); setInterval(() => {}, 1000);`,
  ],
  {
    env: { ...process.env, LEAF_PID_FILE: leafPidFile },
    stdio: "ignore",
    windowsHide: true,
  },
);

writeFileSync(parentPidFile, String(process.pid), "utf8");
process.stdout.write(`PROBE_SOCKET=tree-host-${process.pid}\n`);

const shutdown = () => {
  try {
    leaf.kill();
  } catch {
    // ignore
  }
  process.exit(0);
};
process.on("SIGTERM", shutdown);
process.on("SIGINT", shutdown);

setInterval(() => {}, 1000);
