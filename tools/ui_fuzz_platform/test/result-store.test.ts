//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

import { join } from "node:path";

import { afterEach, describe, expect, it } from "vitest";

import {
  ResultStore,
  stepFingerprintsFromRecords,
  stepSequencesEqual,
} from "../src/result-store.js";
import type { RunConfig } from "../src/scenario.js";
import type { RunResult } from "../src/run.js";
import { makeTempDir } from "./helpers/fixtures.js";

const config: RunConfig = {
  seed: 42,
  maxSteps: 100,
  maxDurationMs: 30_000,
  livenessThresholdMs: 5000,
  startupTimeoutMs: 10_000,
  reuseProject: false,
  hostPath: "C:/fake/host.exe",
  projectPath: "/tmp/project",
  importDir: "/tmp/import",
  outDir: "/tmp/out",
};

function failedRunResult(): RunResult {
  return {
    scenarioName: "wrong_expect_fails",
    seed: 42,
    verdict: "correctness",
    startedAt: 1000,
    endedAt: 2500,
    steps: [
      {
        seq: 1,
        nodeId: "step_one",
        op: { action: "doubleClick", target: "thumbnailGridView_firstCard" },
        opOk: true,
        expectResults: [
          {
            expect: {
              target: "editorSessionStatus",
              property: "text",
              matcher: "eq",
              expected: "Finished",
            },
            ok: false,
            actual: "Ready",
            errorCode: "wait_timeout",
            errorMessage: "Timed out waiting for 'editorSessionStatus.text'.",
          },
        ],
        startedAt: 1100,
        endedAt: 2400,
      },
    ],
    failure: {
      kind: "expect",
      nodeId: "step_one",
      reason: "Timed out waiting for 'editorSessionStatus.text'.",
    },
    logTail: "PROBE_SOCKET=fake\nline two\nQt warning: demo",
    treeSnapshot: { window: "Alcedo Studio", elements: [] },
  };
}

function passRunResult(): RunResult {
  return {
    scenarioName: "library_to_editor_exposure",
    seed: 7,
    verdict: "pass",
    startedAt: 10,
    endedAt: 99,
    steps: [
      {
        seq: 1,
        nodeId: "workspace_ready",
        op: { action: "wait", target: "workspaceHost" },
        opOk: true,
        expectResults: [],
        startedAt: 11,
        endedAt: 20,
      },
      {
        seq: 2,
        nodeId: "open_first_image",
        op: { action: "doubleClick", target: "thumbnailGridView_firstCard" },
        opOk: true,
        expectResults: [],
        startedAt: 21,
        endedAt: 40,
      },
      {
        seq: 3,
        nodeId: "drag_exposure_slider",
        op: { action: "drag", target: "toneExposureSlider" },
        opOk: true,
        expectResults: [],
        startedAt: 41,
        endedAt: 90,
      },
    ],
    logTail: "",
  };
}

describe("ResultStore", () => {
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

  it("ResultStorePersistsFailedRunWithSeedOperationsAndLogTail", () => {
    const store = openStore();
    const result = failedRunResult();
    const runId = store.archiveRun({
      result,
      config,
      scenarioPath: "/scenarios/wrong_expect_fails.yaml",
      logTail: result.logTail,
      treeSnapshot: result.treeSnapshot,
      screenshotPath: "/tmp/out/screenshot.png",
    });

    const detail = store.getRunDetail(runId);
    expect(detail).toBeDefined();
    expect(detail!.run.seed).toBe(42);
    expect(detail!.run.verdict).toBe("correctness");
    expect(detail!.run.scenario).toBe("wrong_expect_fails");
    expect(detail!.run.scenarioPath).toBe("/scenarios/wrong_expect_fails.yaml");
    expect(detail!.run.failureReason).toContain("editorSessionStatus");

    expect(detail!.steps).toHaveLength(1);
    expect(detail!.steps[0]!.nodeId).toBe("step_one");
    expect(detail!.steps[0]!.op).toEqual({
      action: "doubleClick",
      target: "thumbnailGridView_firstCard",
    });

    expect(detail!.failure).not.toBeNull();
    expect(detail!.failure!.logTail).toContain("PROBE_SOCKET=fake");
    expect(detail!.failure!.logTail).toContain("Qt warning: demo");
    expect(detail!.failure!.opHistory).toHaveLength(1);
    expect(detail!.failure!.screenshotPath).toBe("/tmp/out/screenshot.png");
    expect(detail!.failure!.kind).toBe("expect");
  });

  it("ResultStoreListsAndLoadsRunDetail", () => {
    const store = openStore();
    const firstId = store.archiveRun({
      result: failedRunResult(),
      config,
      scenarioPath: "a.yaml",
      logTail: "log-a",
    });
    // Newer started_at so list order is newest-first.
    const laterPass: typeof passRunResult extends () => infer R ? R : never = {
      ...passRunResult(),
      startedAt: 10_000,
      endedAt: 10_100,
    };
    const secondId = store.archiveRun({
      result: laterPass,
      config: { ...config, seed: 7 },
      scenarioPath: "b.yaml",
    });

    const listed = store.listRuns();
    expect(listed.map((row) => row.id)).toEqual([secondId, firstId]);

    const pass = store.getRunDetail(secondId);
    expect(pass!.failure).toBeNull();
    expect(pass!.steps.map((step) => step.nodeId)).toEqual([
      "workspace_ready",
      "open_first_image",
      "drag_exposure_slider",
    ]);
  });

  it("stepSequencesEqualComparesNodeAndOpOrder", () => {
    const steps = passRunResult().steps;
    const a = stepFingerprintsFromRecords(steps);
    const b = stepFingerprintsFromRecords(steps);
    expect(stepSequencesEqual(a, b)).toBe(true);

    const mutated = stepFingerprintsFromRecords([
      steps[0]!,
      { ...steps[1]!, nodeId: "other_node" },
      steps[2]!,
    ]);
    expect(stepSequencesEqual(a, mutated)).toBe(false);
  });
});
