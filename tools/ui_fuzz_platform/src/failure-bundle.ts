//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

/**
 * Failure-bundle capture. On a non-pass verdict the runner assembles a complete
 * reproduction bundle: the ordered operation history, the last N KiB of the Qt
 * log captured from the child pipes, the last tree snapshot, and a PNG screenshot
 * when the window was still responsive. The bundle is written to the run output
 * directory so a failure seed can be replayed (Phase 4 persists it to SQLite).
 */

import { mkdir, writeFile } from "node:fs/promises";
import { join } from "node:path";

import type { RunConfig } from "./scenario.js";
import type { Verdict, WalkFailure, WalkResult } from "./walker.js";

/** Inputs needed to write a failure bundle. */
export interface FailureBundleInput {
  readonly outDir: string;
  readonly scenarioName: string;
  readonly seed: number;
  readonly verdict: Verdict;
  readonly walkResult: WalkResult;
  readonly failure?: WalkFailure;
  readonly logTailKib: number;
  readonly logLines: readonly string[];
  readonly screenshotPng?: Buffer;
  readonly treeSnapshot?: unknown;
  readonly config: RunConfig;
}

/** Paths to the written bundle artifacts. */
export interface FailureBundle {
  readonly dir: string;
  readonly operationsPath: string;
  readonly logTailPath: string;
  readonly screenshotPath?: string;
  readonly snapshotPath?: string;
  readonly failurePath: string;
  readonly runPath: string;
}

/**
 * Writes the failure bundle to `input.outDir`. Creates the directory. Every
 * artifact is a separate file so the results browser (Phase 4) can stream each
 * independently. Returns the resolved artifact paths.
 */
export async function captureFailureBundle(input: FailureBundleInput): Promise<FailureBundle> {
  await mkdir(input.outDir, { recursive: true });

  const dir = input.outDir;
  const operationsPath = join(dir, "operations.json");
  const logTailPath = join(dir, "log_tail.txt");
  const failurePath = join(dir, "failure.json");
  const runPath = join(dir, "run.json");

  await writeFile(operationsPath, JSON.stringify(input.walkResult.steps, null, 2) + "\n", "utf8");

  const logTail = extractTail(input.logLines, input.logTailKib);
  await writeFile(logTailPath, logTail, "utf8");

  await writeFile(failurePath, JSON.stringify(input.failure ?? { reason: input.verdict }, null, 2) + "\n", "utf8");

  await writeFile(
    runPath,
    JSON.stringify(
      {
        scenario: input.scenarioName,
        seed: input.seed,
        verdict: input.verdict,
        startedAt: input.walkResult.startedAt,
        endedAt: input.walkResult.endedAt,
        stepCount: input.walkResult.steps.length,
        config: input.config,
      },
      null,
      2,
    ) + "\n",
    "utf8",
  );

  let screenshotPath: string | undefined;
  if (input.screenshotPng !== undefined && input.screenshotPng.length > 0) {
    screenshotPath = join(dir, "screenshot.png");
    await writeFile(screenshotPath, input.screenshotPng);
  }

  let snapshotPath: string | undefined;
  if (input.treeSnapshot !== undefined) {
    snapshotPath = join(dir, "tree_snapshot.json");
    await writeFile(snapshotPath, JSON.stringify(input.treeSnapshot, null, 2) + "\n", "utf8");
  }

  return {
    dir,
    operationsPath,
    logTailPath,
    failurePath,
    runPath,
    ...(screenshotPath !== undefined ? { screenshotPath } : {}),
    ...(snapshotPath !== undefined ? { snapshotPath } : {}),
  };
}

/** Returns the last `kib` KiB of `lines` joined by newlines. */
function extractTail(lines: readonly string[], kib: number): string {
  const maxBytes = kib * 1024;
  const kept: string[] = [];
  let bytes = 0;
  for (let index = lines.length - 1; index >= 0; index--) {
    const line = lines[index]!;
    bytes += line.length + 1;
    kept.unshift(line);
    if (bytes >= maxBytes) break;
  }
  return kept.join("\n");
}