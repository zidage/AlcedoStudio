//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

/** Client-side mirror of ActiveRunSnapshot from the runner core. */
export type RunSessionStatus = "idle" | "starting" | "running" | "stopping" | "finished";

export interface ActiveRunSnapshot {
  status: RunSessionStatus;
  runId: string | null;
  scenarioPath: string | null;
  scenarioName: string | null;
  seed: number;
  maxSteps: number;
  maxDurationMs: number;
  livenessThresholdMs: number;
  currentNodeId: string | null;
  currentOp: { action: string; target?: string } | null;
  stepCounter: number;
  startedAt: number | null;
  elapsedMs: number;
  heartbeat: {
    counter: number;
    guiTimeMs: number;
    lastSeenAt: number;
  } | null;
  heartbeatAlive: boolean;
  hostPid: number | null;
  probeSocket: string | null;
  verdict: string | null;
  failureReason: string | null;
  error: string | null;
  persistedRunId: string | null;
  parentRunId: string | null;
}

export interface StartRunFormValues {
  scenarioPath: string;
  hostPath: string;
  projectPath?: string;
  importDir?: string;
  seed: number;
  maxSteps: number;
  maxDurationMs: number;
  livenessThresholdMs: number;
  reuseProject: boolean;
}

export interface LogLine {
  key: string;
  line: string;
  stream: "stdout" | "stderr";
  at: number;
}

export type DashboardWsEvent =
  | { type: "status"; snapshot: ActiveRunSnapshot }
  | { type: "log"; line: string; stream: "stdout" | "stderr"; at: number }
  | { type: "heartbeat"; counter: number; guiTimeMs: number; at: number }
  | { type: "stepStart"; nodeId: string; op: { action: string; target?: string }; seq: number; at: number }
  | { type: "stepEnd"; step: unknown; at: number }
  | { type: "finished"; result: unknown; snapshot: ActiveRunSnapshot };

/** Archived run header from GET /api/runs. */
export interface StoredRunSummary {
  id: string;
  seed: number;
  scenario: string;
  scenarioPath: string | null;
  startedAt: number;
  endedAt: number;
  verdict: string;
  config: Record<string, unknown>;
  parentRunId: string | null;
  outDir: string | null;
  failureReason: string | null;
}

export interface StoredStep {
  id: number;
  runId: string;
  seq: number;
  nodeId: string;
  op: { action?: string; target?: string; [key: string]: unknown };
  expectResults: unknown;
  startedAt: number;
  endedAt: number;
  opOk: boolean;
}

export interface StoredFailure {
  runId: string;
  kind: string;
  detail: unknown;
  opHistory: unknown;
  logTail: string;
  treeSnapshot: unknown | null;
  screenshotPath: string | null;
}

export interface StoredRunDetail {
  run: StoredRunSummary;
  steps: StoredStep[];
  failure: StoredFailure | null;
}

/** One scanned `objectName` binding from the QML catalog. */
export interface CatalogEntry {
  objectName: string | null;
  candidates: string[];
  pattern: string | null;
  dynamic: boolean;
  expression: string;
  component: string;
  opKinds: string[];
  source: string;
  line: number;
}

export interface QmlCatalog {
  root: string;
  generatedAt: number;
  filesScanned: number;
  entries: CatalogEntry[];
}

export interface StalenessEntry {
  entry: CatalogEntry;
  status: "present" | "stale" | "dynamic";
  matchedBy: string | null;
}

export interface StalenessReport {
  entries: StalenessEntry[];
  present: number;
  stale: number;
  dynamic: number;
  unmatchedRuntimeNames: string[];
}

export interface WorkflowSummary {
  name: string;
  path: string;
  scenarioName: string | null;
  start: string | null;
  errors: string[] | null;
}

export interface WorkflowDocument {
  name: string;
  path: string;
  yaml: string;
}
