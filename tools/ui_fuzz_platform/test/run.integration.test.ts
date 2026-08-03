//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

import { readFile } from "node:fs/promises";
import { fileURLToPath } from "node:url";

import { describe, expect, it } from "vitest";

import { parseScenario } from "../src/loader.js";
import { runScenario } from "../src/run.js";
import type { RunConfig } from "../src/scenario.js";
import { ACCEPTANCE_SCENARIO_YAML, makeTempDir, WRONG_EXPECT_SCENARIO_YAML } from "./helpers/fixtures.js";

const fakeHostPath = fileURLToPath(new URL("./helpers/fake-host.mjs", import.meta.url));

/** A run config that spawns the fake Node test host instead of the Qt executable. */
function fakeHostConfig(outDir: string): RunConfig {
  return {
    seed: 7,
    maxSteps: 100,
    maxDurationMs: 30_000,
    livenessThresholdMs: 5000,
    startupTimeoutMs: 10_000,
    reuseProject: false,
    hostCommand: [process.execPath, fakeHostPath],
    projectPath: makeTempDir(),
    importDir: makeTempDir(),
    outDir,
  };
}

describe("runScenario (fake test host over a real named pipe)", () => {
  it("runs the acceptance scenario end-to-end and passes", async () => {
    const scenario = parseScenario(ACCEPTANCE_SCENARIO_YAML);
    const result = await runScenario(scenario, fakeHostConfig(makeTempDir()));

    expect(result.verdict).toBe("pass");
    expect(result.steps.map((step) => step.nodeId)).toEqual([
      "workspace_ready",
      "open_first_image",
      "drag_exposure_slider",
    ]);
    expect(result.probeSocket).toMatch(/^fake-host-/);
  });

  it("captures a failure bundle with operation history, log tail, and screenshot when an expect fails", async () => {
    const scenario = parseScenario(WRONG_EXPECT_SCENARIO_YAML);
    const outDir = makeTempDir();
    const result = await runScenario(scenario, fakeHostConfig(outDir));

    expect(result.verdict).toBe("correctness");
    expect(result.failure?.reason).toContain("Finished");

    const bundle = result.bundle;
    expect(bundle).toBeDefined();
    expect(bundle!.dir).toBe(outDir);

    const operations = JSON.parse(await readFile(bundle!.operationsPath, "utf8"));
    expect(operations).toHaveLength(1);
    expect(operations[0].expectResults[0].errorCode).toBe("wait_timeout");
    expect(operations[0].expectResults[0].actual).toBe("Ready");

    const logTail = await readFile(bundle!.logTailPath, "utf8");
    expect(logTail.length).toBeGreaterThanOrEqual(0);

    expect(bundle!.screenshotPath).toBeDefined();
    const screenshot = await readFile(bundle!.screenshotPath!);
    expect(screenshot.length).toBeGreaterThan(0);
  });
});