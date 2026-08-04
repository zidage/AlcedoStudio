//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

/**
 * Live run progress events for the Phase 3 dashboard. The runner core emits these
 * while a scenario walks; the process manager fans them out over WebSocket.
 */

import type { Op } from "./scenario.js";
import type { StepRecord, Verdict } from "./walker.js";
import type { RunResult } from "./run.js";

/** Lifecycle of a managed dashboard run. */
export type RunSessionStatus = "idle" | "starting" | "running" | "stopping" | "finished";

/** Snapshot the dashboard polls / receives after every meaningful change. */
export interface ActiveRunSnapshot {
  readonly status: RunSessionStatus;
  readonly runId: string | null;
  readonly scenarioPath: string | null;
  readonly scenarioName: string | null;
  readonly seed: number;
  readonly maxSteps: number;
  readonly maxDurationMs: number;
  readonly livenessThresholdMs: number;
  readonly currentNodeId: string | null;
  readonly currentOp: Op | null;
  readonly stepCounter: number;
  readonly startedAt: number | null;
  readonly elapsedMs: number;
  readonly heartbeat: {
    readonly counter: number;
    readonly guiTimeMs: number;
    readonly lastSeenAt: number;
  } | null;
  readonly heartbeatAlive: boolean;
  readonly hostPid: number | null;
  readonly probeSocket: string | null;
  readonly verdict: Verdict | null;
  readonly failureReason: string | null;
  readonly error: string | null;
  /** SQLite run id after archival (Phase 4); null while running or when store unset. */
  readonly persistedRunId: string | null;
  /** When this session is a replay, the original failure run id. */
  readonly parentRunId: string | null;
}

/** Discriminated union streamed to browser clients. */
export type RunManagerEvent =
  | { readonly type: "status"; readonly snapshot: ActiveRunSnapshot }
  | { readonly type: "log"; readonly line: string; readonly stream: "stdout" | "stderr"; readonly at: number }
  | { readonly type: "heartbeat"; readonly counter: number; readonly guiTimeMs: number; readonly at: number }
  | { readonly type: "stepStart"; readonly nodeId: string; readonly op: Op; readonly seq: number; readonly at: number }
  | { readonly type: "stepEnd"; readonly step: StepRecord; readonly at: number }
  | { readonly type: "finished"; readonly result: RunResult; readonly snapshot: ActiveRunSnapshot };

/** Request body for starting a managed run from the dashboard. */
export interface StartRunRequest {
  readonly scenarioPath: string;
  readonly hostPath: string;
  readonly projectPath?: string;
  readonly importDir?: string;
  readonly seed?: number;
  readonly maxSteps?: number;
  readonly maxDurationMs?: number;
  readonly livenessThresholdMs?: number;
  readonly startupTimeoutMs?: number;
  readonly reuseProject?: boolean;
  readonly outDir?: string;
  /** When set, the archived run records this as `parent_run_id` (replay provenance). */
  readonly parentRunId?: string;
}

/** Builds the idle snapshot shown when no run is active. */
export function idleSnapshot(partial?: Partial<ActiveRunSnapshot>): ActiveRunSnapshot {
  return {
    status: "idle",
    runId: null,
    scenarioPath: null,
    scenarioName: null,
    seed: 0,
    maxSteps: 1000,
    maxDurationMs: 300_000,
    livenessThresholdMs: 5000,
    currentNodeId: null,
    currentOp: null,
    stepCounter: 0,
    startedAt: null,
    elapsedMs: 0,
    heartbeat: null,
    heartbeatAlive: false,
    hostPid: null,
    probeSocket: null,
    verdict: null,
    failureReason: null,
    error: null,
    persistedRunId: null,
    parentRunId: null,
    ...partial,
  };
}
