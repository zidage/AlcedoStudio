//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

import { readFile, readdir } from "node:fs/promises";
import { join } from "node:path";

import { describe, expect, it } from "vitest";

import { captureFailureBundle } from "../src/failure-bundle.js";
import type { RunConfig } from "../src/scenario.js";
import type { WalkResult } from "../src/walker.js";
import { makeTempDir } from "./helpers/fixtures.js";

const config: RunConfig = {
  seed: 42,
  maxSteps: 1000,
  maxDurationMs: 300_000,
  livenessThresholdMs: 5000,
  startupTimeoutMs: 120_000,
  reuseProject: false,
  outDir: "",
};

const PNG_1X1 = Buffer.from(
  "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mNk+M8AAAMBAQDJ/pLvAAAAAElFTkSuQmCC",
  "base64",
);

function walkResult(): WalkResult {
  return {
    verdict: "correctness",
    startedAt: 1000,
    endedAt: 2000,
    steps: [
      {
        seq: 1,
        nodeId: "open_first_image",
        op: { action: "doubleClick", target: "thumbnailGridView_firstCard" },
        opOk: true,
        expectResults: [
          { expect: { target: "editorWorkspace", property: "visible", matcher: "eq", expected: true }, ok: true, actual: true },
          {
            expect: { target: "editorSessionStatus", property: "text", matcher: "contains", expected: "Ready" },
            ok: false,
            actual: "Running",
            errorCode: "wait_timeout",
            errorMessage: "Timed out waiting for 'editorSessionStatus.text'.",
          },
        ],
        startedAt: 1500,
        endedAt: 2000,
      },
    ],
    failure: { nodeId: "open_first_image", kind: "expect", expectIndex: 1, reason: "timed out" },
  };
}

describe("captureFailureBundle", () => {
  it("writes operations.json, log_tail.txt, failure.json, and run.json", async () => {
    const dir = makeTempDir();
    const bundle = await captureFailureBundle({
      outDir: dir,
      scenarioName: "wrong_expect_fails",
      seed: 42,
      verdict: "correctness",
      walkResult: walkResult(),
      failure: { kind: "expect", nodeId: "open_first_image", reason: "timed out" },
      logTailKib: 64,
      logLines: ["line one", "line two", "line three"],
      config: { ...config, outDir: dir },
    });

    const names = await readdir(dir);
    expect(names).toContain("operations.json");
    expect(names).toContain("log_tail.txt");
    expect(names).toContain("failure.json");
    expect(names).toContain("run.json");

    const operations = JSON.parse(await readFile(bundle.operationsPath, "utf8"));
    expect(operations).toHaveLength(1);
    expect(operations[0].expectResults[1].errorCode).toBe("wait_timeout");

    const logTail = await readFile(bundle.logTailPath, "utf8");
    expect(logTail).toContain("line three");

    const run = JSON.parse(await readFile(bundle.runPath, "utf8"));
    expect(run.scenario).toBe("wrong_expect_fails");
    expect(run.seed).toBe(42);
    expect(run.verdict).toBe("correctness");
  });

  it("includes screenshot.png when a PNG buffer is supplied", async () => {
    const dir = makeTempDir();
    const bundle = await captureFailureBundle({
      outDir: dir,
      scenarioName: "s",
      seed: 1,
      verdict: "correctness",
      walkResult: walkResult(),
      failure: { kind: "expect", nodeId: "n", reason: "r" },
      logTailKib: 64,
      logLines: [],
      screenshotPng: PNG_1X1,
      config: { ...config, outDir: dir },
    });
    expect(bundle.screenshotPath).toBe(join(dir, "screenshot.png"));
    const bytes = await readFile(bundle.screenshotPath!);
    expect(bytes.equals(PNG_1X1)).toBe(true);
  });

  it("omits screenshot.png when no screenshot is supplied", async () => {
    const dir = makeTempDir();
    const bundle = await captureFailureBundle({
      outDir: dir,
      scenarioName: "s",
      seed: 1,
      verdict: "correctness",
      walkResult: walkResult(),
      failure: { kind: "expect", nodeId: "n", reason: "r" },
      logTailKib: 64,
      logLines: [],
      config: { ...config, outDir: dir },
    });
    expect(bundle.screenshotPath).toBeUndefined();
    const names = await readdir(dir);
    expect(names).not.toContain("screenshot.png");
  });

  it("truncates the log tail to the requested KiB", async () => {
    const dir = makeTempDir();
    const bigLine = "x".repeat(600);
    const logLines = Array.from({ length: 30 }, () => bigLine);
    const bundle = await captureFailureBundle({
      outDir: dir,
      scenarioName: "s",
      seed: 1,
      verdict: "correctness",
      walkResult: walkResult(),
      failure: { kind: "expect", nodeId: "n", reason: "r" },
      logTailKib: 1,
      logLines,
      config: { ...config, outDir: dir },
    });
    const tail = await readFile(bundle.logTailPath, "utf8");
    // Line-based tail keeps whole lines, so it may overshoot 1 KiB by one line
    // but must drop the older lines (30 * 600 chars would be ~18 KiB untruncated).
    expect(tail.split("\n").length).toBeLessThan(30);
    expect(tail.endsWith("x".repeat(600))).toBe(true);
  });
});