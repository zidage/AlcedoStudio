//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

import { join } from "node:path";
import { fileURLToPath } from "node:url";

import { afterEach, describe, expect, it } from "vitest";

import { parseScenario } from "../src/loader.js";
import { replayRun } from "../src/replay.js";
import { ResultStore, stepSequencesEqual } from "../src/result-store.js";
import { runScenario } from "../src/run.js";
import type { RunConfig } from "../src/scenario.js";
import {
  ACCEPTANCE_SCENARIO_YAML,
  makeTempDir,
  WRONG_EXPECT_SCENARIO_YAML,
} from "./helpers/fixtures.js";

const fakeHostPath = fileURLToPath(new URL("./helpers/fake-host.mjs", import.meta.url));
const acceptanceScenarioPath = fileURLToPath(
  new URL("../scenarios/library_to_editor_exposure.yaml", import.meta.url),
);
const wrongExpectScenarioPath = fileURLToPath(
  new URL("../scenarios/wrong_expect_fails.yaml", import.meta.url),
);

function fakeHostConfig(outDir: string, seed = 11): RunConfig {
  return {
    seed,
    maxSteps: 100,
    maxDurationMs: 30_000,
    livenessThresholdMs: 5000,
    startupTimeoutMs: 10_000,
    reuseProject: false,
    hostCommand: [process.execPath, fakeHostPath],
    hostPath: "unused-when-command-set",
    projectPath: makeTempDir(),
    importDir: makeTempDir(),
    outDir,
  };
}

describe("replay-by-seed", () => {
  const stores: ResultStore[] = [];

  afterEach(() => {
    for (const store of stores) store.close();
    stores.length = 0;
  });

  function openStore(): ResultStore {
    const store = new ResultStore(join(makeTempDir(), "results.sqlite"));
    stores.push(store);
    return store;
  }

  it("ReplayBySeedReproducesIdenticalStepSequence", async () => {
    const store = openStore();
    const scenario = parseScenario(ACCEPTANCE_SCENARIO_YAML);
    const config = fakeHostConfig(makeTempDir(), 19);
    const original = await runScenario(scenario, config);

    expect(original.verdict).toBe("pass");
    const originalRunId = store.archiveRun({
      result: original,
      config,
      scenarioPath: acceptanceScenarioPath,
      logTail: original.logTail,
    });

    const replay = await replayRun({
      store,
      originalRunId,
      hostCommandOverride: [process.execPath, fakeHostPath],
      outDir: makeTempDir(),
    });

    expect(replay.seed).toBe(19);
    expect(replay.sequencesMatch).toBe(true);
    expect(replay.replayFingerprints.map((step) => step.nodeId)).toEqual([
      "workspace_ready",
      "open_first_image",
      "drag_exposure_slider",
    ]);

    const originalSteps = store.getStepFingerprints(originalRunId);
    const replaySteps = store.getStepFingerprints(replay.replayRunId);
    expect(stepSequencesEqual(originalSteps, replaySteps)).toBe(true);

    const replayRow = store.getRun(replay.replayRunId);
    expect(replayRow?.parentRunId).toBe(originalRunId);
    expect(replayRow?.seed).toBe(19);
  });

  it("ReplayFailedRunKeepsSeedAndOperationListInBothRows", async () => {
    const store = openStore();
    const scenario = parseScenario(WRONG_EXPECT_SCENARIO_YAML);
    const config = fakeHostConfig(makeTempDir(), 55);
    const original = await runScenario(scenario, config);

    expect(original.verdict).toBe("correctness");
    const originalRunId = store.archiveRun({
      result: original,
      config,
      scenarioPath: wrongExpectScenarioPath,
      logTail: original.logTail,
      treeSnapshot: original.treeSnapshot,
      screenshotPath: original.bundle?.screenshotPath,
    });

    const originalDetail = store.getRunDetail(originalRunId);
    expect(originalDetail!.failure).not.toBeNull();
    expect(originalDetail!.run.seed).toBe(55);
    expect(originalDetail!.steps.length).toBeGreaterThan(0);
    // Log may be empty on a fast fail path; failure row must still exist with op history.
    expect(originalDetail!.failure!.opHistory).toHaveLength(original.steps.length);

    const replay = await replayRun({
      store,
      originalRunId,
      hostCommandOverride: [process.execPath, fakeHostPath],
      outDir: makeTempDir(),
    });

    expect(replay.sequencesMatch).toBe(true);
    expect(store.getFailure(replay.replayRunId)).not.toBeNull();
    expect(store.getRun(replay.replayRunId)?.seed).toBe(55);
  });
});
