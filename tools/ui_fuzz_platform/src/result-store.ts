//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

/**
 * SQLite result store for completed UI fuzz runs. One file under the platform
 * data directory holds every archived run: seed, config, ordered steps, and
 * (on non-pass verdicts) failure detail with operation history and log tail.
 *
 * Schema (v1):
 * - runs(id, seed, scenario, scenario_path, started_at, ended_at, verdict,
 *   config_json, parent_run_id, out_dir, failure_reason)
 * - steps(id, run_id, seq, node_id, op_json, expect_results_json, started_at,
 *   ended_at, op_ok)
 * - failures(run_id, kind, detail_json, op_history_json, log_tail,
 *   tree_snapshot, screenshot_path)
 */

import { mkdirSync } from "node:fs";
import { dirname } from "node:path";
import { randomUUID } from "node:crypto";

import Database from "better-sqlite3";

import type { RunConfig } from "./scenario.js";
import type { RunResult } from "./run.js";
import type { StepRecord, Verdict } from "./walker.js";

/** Row stored in the `runs` table. */
export interface StoredRun {
  readonly id: string;
  readonly seed: number;
  readonly scenario: string;
  readonly scenarioPath: string | null;
  readonly startedAt: number;
  readonly endedAt: number;
  readonly verdict: Verdict;
  readonly config: RunConfig;
  readonly parentRunId: string | null;
  readonly outDir: string | null;
  readonly failureReason: string | null;
}

/** One step row reconstructed from SQLite. */
export interface StoredStep {
  readonly id: number;
  readonly runId: string;
  readonly seq: number;
  readonly nodeId: string;
  readonly op: unknown;
  readonly expectResults: unknown;
  readonly startedAt: number;
  readonly endedAt: number;
  readonly opOk: boolean;
}

/** Failure artifacts for a non-pass run. */
export interface StoredFailure {
  readonly runId: string;
  readonly kind: string;
  readonly detail: unknown;
  readonly opHistory: unknown;
  readonly logTail: string;
  readonly treeSnapshot: unknown | null;
  readonly screenshotPath: string | null;
}

/** Full run detail for the results browser. */
export interface StoredRunDetail {
  readonly run: StoredRun;
  readonly steps: readonly StoredStep[];
  readonly failure: StoredFailure | null;
}

/** Inputs for archiving a finished run. */
export interface ArchiveRunInput {
  readonly result: RunResult;
  readonly config: RunConfig;
  readonly scenarioPath?: string;
  readonly runId?: string;
  readonly parentRunId?: string;
  /** Qt log tail (last N KiB). Required for non-pass verdicts. */
  readonly logTail?: string;
  readonly treeSnapshot?: unknown;
  readonly screenshotPath?: string;
}

/** Compact step fingerprint used to compare original vs replay sequences. */
export interface StepFingerprint {
  readonly seq: number;
  readonly nodeId: string;
  readonly action: string;
  readonly target?: string;
}

const SCHEMA_SQL = `
CREATE TABLE IF NOT EXISTS runs (
  id TEXT PRIMARY KEY NOT NULL,
  seed INTEGER NOT NULL,
  scenario TEXT NOT NULL,
  scenario_path TEXT,
  started_at INTEGER NOT NULL,
  ended_at INTEGER NOT NULL,
  verdict TEXT NOT NULL,
  config_json TEXT NOT NULL,
  parent_run_id TEXT,
  out_dir TEXT,
  failure_reason TEXT
);

CREATE TABLE IF NOT EXISTS steps (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  run_id TEXT NOT NULL REFERENCES runs(id) ON DELETE CASCADE,
  seq INTEGER NOT NULL,
  node_id TEXT NOT NULL,
  op_json TEXT NOT NULL,
  expect_results_json TEXT NOT NULL,
  started_at INTEGER NOT NULL,
  ended_at INTEGER NOT NULL,
  op_ok INTEGER NOT NULL,
  UNIQUE (run_id, seq)
);

CREATE TABLE IF NOT EXISTS failures (
  run_id TEXT PRIMARY KEY NOT NULL REFERENCES runs(id) ON DELETE CASCADE,
  kind TEXT NOT NULL,
  detail_json TEXT NOT NULL,
  op_history_json TEXT NOT NULL,
  log_tail TEXT NOT NULL,
  tree_snapshot TEXT,
  screenshot_path TEXT
);

CREATE INDEX IF NOT EXISTS idx_runs_started_at ON runs(started_at DESC);
CREATE INDEX IF NOT EXISTS idx_steps_run_id ON steps(run_id, seq);
`;

/**
 * Opens (or creates) the SQLite result database at `dbPath`. Parent directories
 * are created when missing. Call {@link ResultStore.close} when done.
 */
export class ResultStore {
  private readonly db: Database.Database;

  constructor(dbPath: string) {
    if (dbPath !== ":memory:") {
      mkdirSync(dirname(dbPath), { recursive: true });
    }
    this.db = new Database(dbPath);
    this.db.pragma("journal_mode = WAL");
    this.db.pragma("foreign_keys = ON");
    this.db.exec(SCHEMA_SQL);
  }

  /** Absolute or special path this store was opened with. */
  get path(): string {
    return this.db.name;
  }

  /**
   * Archives a finished run: always writes `runs` + `steps`; on non-pass
   * verdicts also writes `failures` with seed-linked operation history and log
   * tail. Returns the stored run id.
   */
  archiveRun(input: ArchiveRunInput): string {
    const runId = input.runId ?? randomUUID();
    const { result, config } = input;
    const failureReason = result.failure?.reason ?? null;

    const insertRun = this.db.transaction(() => {
      this.db
        .prepare(
          `INSERT INTO runs (
            id, seed, scenario, scenario_path, started_at, ended_at, verdict,
            config_json, parent_run_id, out_dir, failure_reason
          ) VALUES (
            @id, @seed, @scenario, @scenarioPath, @startedAt, @endedAt, @verdict,
            @configJson, @parentRunId, @outDir, @failureReason
          )`,
        )
        .run({
          id: runId,
          seed: result.seed,
          scenario: result.scenarioName,
          scenarioPath: input.scenarioPath ?? null,
          startedAt: result.startedAt,
          endedAt: result.endedAt,
          verdict: result.verdict,
          configJson: JSON.stringify(config),
          parentRunId: input.parentRunId ?? null,
          outDir: result.bundle?.dir ?? config.outDir ?? null,
          failureReason,
        });

      const insertStep = this.db.prepare(
        `INSERT INTO steps (
          run_id, seq, node_id, op_json, expect_results_json,
          started_at, ended_at, op_ok
        ) VALUES (
          @runId, @seq, @nodeId, @opJson, @expectResultsJson,
          @startedAt, @endedAt, @opOk
        )`,
      );

      for (const step of result.steps) {
        insertStep.run({
          runId,
          seq: step.seq,
          nodeId: step.nodeId,
          opJson: JSON.stringify(step.op),
          expectResultsJson: JSON.stringify(step.expectResults),
          startedAt: step.startedAt,
          endedAt: step.endedAt,
          opOk: step.opOk ? 1 : 0,
        });
      }

      if (result.verdict !== "pass") {
        const logTail = input.logTail ?? "";
        const kind = result.failure?.kind ?? result.verdict;
        this.db
          .prepare(
            `INSERT INTO failures (
              run_id, kind, detail_json, op_history_json, log_tail,
              tree_snapshot, screenshot_path
            ) VALUES (
              @runId, @kind, @detailJson, @opHistoryJson, @logTail,
              @treeSnapshot, @screenshotPath
            )`,
          )
          .run({
            runId,
            kind,
            detailJson: JSON.stringify(result.failure ?? { reason: result.verdict }),
            opHistoryJson: JSON.stringify(result.steps),
            logTail,
            treeSnapshot:
              input.treeSnapshot !== undefined ? JSON.stringify(input.treeSnapshot) : null,
            screenshotPath: input.screenshotPath ?? result.bundle?.screenshotPath ?? null,
          });
      }
    });

    insertRun();
    return runId;
  }

  /** Lists runs newest-first. Optional `limit` defaults to 100. */
  listRuns(limit = 100): readonly StoredRun[] {
    const rows = this.db
      .prepare(
        `SELECT id, seed, scenario, scenario_path, started_at, ended_at, verdict,
                config_json, parent_run_id, out_dir, failure_reason
         FROM runs
         ORDER BY started_at DESC
         LIMIT ?`,
      )
      .all(limit) as RunRow[];
    return rows.map(mapRunRow);
  }

  /** Loads one run header or `undefined` when missing. */
  getRun(runId: string): StoredRun | undefined {
    const row = this.db
      .prepare(
        `SELECT id, seed, scenario, scenario_path, started_at, ended_at, verdict,
                config_json, parent_run_id, out_dir, failure_reason
         FROM runs WHERE id = ?`,
      )
      .get(runId) as RunRow | undefined;
    return row === undefined ? undefined : mapRunRow(row);
  }

  /** Ordered steps for a run (empty when the run has none). */
  getSteps(runId: string): readonly StoredStep[] {
    const rows = this.db
      .prepare(
        `SELECT id, run_id, seq, node_id, op_json, expect_results_json,
                started_at, ended_at, op_ok
         FROM steps WHERE run_id = ? ORDER BY seq ASC`,
      )
      .all(runId) as StepRow[];
    return rows.map(mapStepRow);
  }

  /** Failure row for a non-pass run, or `null`. */
  getFailure(runId: string): StoredFailure | null {
    const row = this.db
      .prepare(
        `SELECT run_id, kind, detail_json, op_history_json, log_tail,
                tree_snapshot, screenshot_path
         FROM failures WHERE run_id = ?`,
      )
      .get(runId) as FailureRow | undefined;
    return row === undefined ? null : mapFailureRow(row);
  }

  /** Run header + steps + optional failure for the detail page. */
  getRunDetail(runId: string): StoredRunDetail | undefined {
    const run = this.getRun(runId);
    if (run === undefined) return undefined;
    return {
      run,
      steps: this.getSteps(runId),
      failure: this.getFailure(runId),
    };
  }

  /**
   * Step fingerprints for sequence comparison (replay acceptance). Compares
   * node id + op action/target order, ignoring expect actuals and timestamps.
   */
  getStepFingerprints(runId: string): readonly StepFingerprint[] {
    return this.getSteps(runId).map((step) => fingerprintFromStored(step));
  }

  close(): void {
    this.db.close();
  }
}

/**
 * Builds step fingerprints from in-memory {@link StepRecord}s so a live
 * {@link RunResult} can be compared to SQLite rows without re-reading disk.
 */
export function stepFingerprintsFromRecords(
  steps: readonly StepRecord[],
): readonly StepFingerprint[] {
  return steps.map((step) => ({
    seq: step.seq,
    nodeId: step.nodeId,
    action: step.op.action,
    ...(step.op.target !== undefined ? { target: step.op.target } : {}),
  }));
}

/** True when both sequences list the same node/op order (same length and values). */
export function stepSequencesEqual(
  a: readonly StepFingerprint[],
  b: readonly StepFingerprint[],
): boolean {
  if (a.length !== b.length) return false;
  for (let index = 0; index < a.length; index++) {
    const left = a[index]!;
    const right = b[index]!;
    if (
      left.seq !== right.seq ||
      left.nodeId !== right.nodeId ||
      left.action !== right.action ||
      left.target !== right.target
    ) {
      return false;
    }
  }
  return true;
}

interface RunRow {
  id: string;
  seed: number;
  scenario: string;
  scenario_path: string | null;
  started_at: number;
  ended_at: number;
  verdict: string;
  config_json: string;
  parent_run_id: string | null;
  out_dir: string | null;
  failure_reason: string | null;
}

interface StepRow {
  id: number;
  run_id: string;
  seq: number;
  node_id: string;
  op_json: string;
  expect_results_json: string;
  started_at: number;
  ended_at: number;
  op_ok: number;
}

interface FailureRow {
  run_id: string;
  kind: string;
  detail_json: string;
  op_history_json: string;
  log_tail: string;
  tree_snapshot: string | null;
  screenshot_path: string | null;
}

function mapRunRow(row: RunRow): StoredRun {
  return {
    id: row.id,
    seed: row.seed,
    scenario: row.scenario,
    scenarioPath: row.scenario_path,
    startedAt: row.started_at,
    endedAt: row.ended_at,
    verdict: row.verdict as Verdict,
    config: JSON.parse(row.config_json) as RunConfig,
    parentRunId: row.parent_run_id,
    outDir: row.out_dir,
    failureReason: row.failure_reason,
  };
}

function mapStepRow(row: StepRow): StoredStep {
  return {
    id: row.id,
    runId: row.run_id,
    seq: row.seq,
    nodeId: row.node_id,
    op: JSON.parse(row.op_json) as unknown,
    expectResults: JSON.parse(row.expect_results_json) as unknown,
    startedAt: row.started_at,
    endedAt: row.ended_at,
    opOk: row.op_ok === 1,
  };
}

function mapFailureRow(row: FailureRow): StoredFailure {
  return {
    runId: row.run_id,
    kind: row.kind,
    detail: JSON.parse(row.detail_json) as unknown,
    opHistory: JSON.parse(row.op_history_json) as unknown,
    logTail: row.log_tail,
    treeSnapshot:
      row.tree_snapshot !== null ? (JSON.parse(row.tree_snapshot) as unknown) : null,
    screenshotPath: row.screenshot_path,
  };
}

function fingerprintFromStored(step: StoredStep): StepFingerprint {
  const op = step.op as { action?: string; target?: string };
  return {
    seq: step.seq,
    nodeId: step.nodeId,
    action: typeof op.action === "string" ? op.action : "unknown",
    ...(typeof op.target === "string" ? { target: op.target } : {}),
  };
}
