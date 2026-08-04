//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

/**
 * Replay-by-seed: re-executes a previously archived run with the same seed and
 * scenario configuration, then verifies the step sequence against the original
 * `steps` rows (node id + op action/target order).
 *
 * Phase 2 walks edges in declaration order, so the same host + scenario yields
 * the same node sequence; the seed is stored for Phase 6 weighted walk replay.
 */

import { join, resolve } from "node:path";
import { randomUUID } from "node:crypto";

import { loadScenario } from "./loader.js";
import { runScenario, type RunProgressHooks, type RunResult } from "./run.js";
import {
  ResultStore,
  stepFingerprintsFromRecords,
  stepSequencesEqual,
  type StepFingerprint,
  type StoredRun,
} from "./result-store.js";
import type { RunConfig } from "./scenario.js";

/** Outcome of a replay attempt. */
export interface ReplayResult {
  readonly originalRunId: string;
  readonly replayRunId: string;
  readonly seed: number;
  readonly originalFingerprints: readonly StepFingerprint[];
  readonly replayFingerprints: readonly StepFingerprint[];
  readonly sequencesMatch: boolean;
  readonly runResult: RunResult;
}

/** Optional overrides when replaying (e.g. host command for tests). */
export interface ReplayOptions {
  readonly store: ResultStore;
  readonly originalRunId: string;
  /** When set, replaces hostPath / hostCommand from the stored config. */
  readonly hostCommandOverride?: readonly string[];
  readonly hostPathOverride?: string;
  readonly outDir?: string;
  readonly hooks?: RunProgressHooks;
}

/**
 * Replays the archived run identified by `originalRunId`. Loads the scenario
 * from the stored `scenario_path`, reuses the original seed and run bounds,
 * archives the replay under a new id with `parent_run_id` set, and compares
 * step fingerprints.
 *
 * @throws when the original run is missing or has no scenario path.
 */
export async function replayRun(options: ReplayOptions): Promise<ReplayResult> {
  const original = options.store.getRun(options.originalRunId);
  if (original === undefined) {
    throw new Error(`Unknown run id: ${options.originalRunId}`);
  }
  if (original.scenarioPath === null || original.scenarioPath.length === 0) {
    throw new Error(
      `Run ${options.originalRunId} has no scenario_path; cannot replay.`,
    );
  }

  const scenario = await loadScenario(resolve(original.scenarioPath));
  const config = buildReplayConfig(original, options);
  const hooks = options.hooks ?? {};
  const runResult = await runScenario(scenario, config, hooks);

  const replayRunId = randomUUID();
  options.store.archiveRun({
    result: runResult,
    config,
    scenarioPath: original.scenarioPath,
    runId: replayRunId,
    parentRunId: options.originalRunId,
    logTail: runResult.logTail,
    treeSnapshot: runResult.treeSnapshot,
    screenshotPath: runResult.bundle?.screenshotPath,
  });

  const originalFingerprints = options.store.getStepFingerprints(options.originalRunId);
  const replayFingerprints = stepFingerprintsFromRecords(runResult.steps);

  return {
    originalRunId: options.originalRunId,
    replayRunId,
    seed: original.seed,
    originalFingerprints,
    replayFingerprints,
    sequencesMatch: stepSequencesEqual(originalFingerprints, replayFingerprints),
    runResult,
  };
}

function buildReplayConfig(original: StoredRun, options: ReplayOptions): RunConfig {
  const base = original.config;
  const outDir =
    options.outDir !== undefined
      ? resolve(options.outDir)
      : resolve(
          join(
            "build",
            "tmp",
            "ui_fuzz_platform",
            `replay-${original.id}-${Date.now()}`,
          ),
        );

  return {
    ...base,
    seed: original.seed,
    outDir,
    hostPath: options.hostPathOverride ?? base.hostPath,
    hostCommand: options.hostCommandOverride ?? base.hostCommand,
  };
}
