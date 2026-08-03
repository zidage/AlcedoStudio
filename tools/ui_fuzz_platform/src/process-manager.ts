//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

/**
 * Dashboard process manager. Owns at most one active run: starts a scenario via
 * {@link runScenario}, streams live log/heartbeat/step events to subscribers,
 * and on stop kills the entire host process tree so no orphans remain.
 *
 * One manager instance is expected per dashboard server process. Concurrent
 * multi-run fuzzing is Phase 6+ scope.
 */

import { EventEmitter } from "node:events";
import { join, resolve } from "node:path";
import { randomUUID } from "node:crypto";

import { loadScenario } from "./loader.js";
import { isProcessAlive, killProcessTree } from "./process-tree.js";
import {
  idleSnapshot,
  type ActiveRunSnapshot,
  type RunManagerEvent,
  type StartRunRequest,
} from "./run-events.js";
import { runScenario, type RunProgressHooks, type RunResult } from "./run.js";
import { DEFAULT_RUN_CONFIG, type Op, type RunConfig } from "./scenario.js";
import type { HostHandle } from "./host-process.js";
import type { StepRecord } from "./walker.js";

/** Optional overrides used by tests to spawn a fake host instead of a Qt binary. */
export interface ProcessManagerOptions {
  /** When set, replaces `hostPath` with this command (e.g. node + fake-host.mjs). */
  readonly hostCommandOverride?: readonly string[];
  /** Injectable clock for elapsed-time snapshots. */
  readonly clock?: () => number;
}

/**
 * Manages the single active dashboard run and fans {@link RunManagerEvent}s out
 * to WebSocket / SSE subscribers.
 */
export class ProcessManager extends EventEmitter {
  private snapshot: ActiveRunSnapshot = idleSnapshot();
  private host: HostHandle | undefined;
  private abortRequested = false;
  private abortController: AbortController | undefined;
  private runPromise: Promise<void> | undefined;
  private readonly clock: () => number;

  constructor(private readonly options: ProcessManagerOptions = {}) {
    super();
    this.clock = options.clock ?? (() => Date.now());
  }

  /** Current run snapshot (safe to serialize to JSON for GET /api/runs/active). */
  getSnapshot(): ActiveRunSnapshot {
    return this.withElapsed(this.snapshot);
  }

  /** True when a start or stop is in flight, or a walk is active. */
  isBusy(): boolean {
    const status = this.snapshot.status;
    return status === "starting" || status === "running" || status === "stopping";
  }

  /**
   * Starts a managed run. Rejects when another run is already active.
   * Returns the assigned `runId` once the session has entered `starting`.
   */
  async start(request: StartRunRequest): Promise<string> {
    if (this.isBusy()) {
      throw new Error(`Cannot start a run while status is '${this.snapshot.status}'.`);
    }

    const runId = randomUUID();
    const scenarioPath = resolve(request.scenarioPath);
    const seed = request.seed ?? DEFAULT_RUN_CONFIG.seed;
    const maxSteps = request.maxSteps ?? DEFAULT_RUN_CONFIG.maxSteps;
    const maxDurationMs = request.maxDurationMs ?? DEFAULT_RUN_CONFIG.maxDurationMs;
    const livenessThresholdMs = request.livenessThresholdMs ?? DEFAULT_RUN_CONFIG.livenessThresholdMs;
    const startupTimeoutMs = request.startupTimeoutMs ?? DEFAULT_RUN_CONFIG.startupTimeoutMs;
    const startedAt = this.clock();
    const outDir =
      request.outDir !== undefined
        ? resolve(request.outDir)
        : resolve(join("build", "tmp", "ui_fuzz_platform", `dashboard-${runId}`));

    this.abortRequested = false;
    this.abortController = new AbortController();
    this.host = undefined;
    this.snapshot = idleSnapshot({
      status: "starting",
      runId,
      scenarioPath,
      seed,
      maxSteps,
      maxDurationMs,
      livenessThresholdMs,
      startedAt,
      elapsedMs: 0,
    });
    this.emitEvent({ type: "status", snapshot: this.getSnapshot() });

    this.runPromise = this.execute(runId, scenarioPath, request, {
      seed,
      maxSteps,
      maxDurationMs,
      livenessThresholdMs,
      startupTimeoutMs,
      outDir,
    }).finally(() => {
      this.runPromise = undefined;
    });

    // Surface start failures to the caller without blocking the HTTP response on
    // the full walk: await only until the promise is scheduled.
    void this.runPromise;
    return runId;
  }

  /**
   * Stops the active run: marks status `stopping`, kills the host process tree,
   * and waits for the walk promise to settle. Idempotent when idle/finished.
   */
  async stop(): Promise<ActiveRunSnapshot> {
    if (!this.isBusy() && this.host === undefined) {
      return this.getSnapshot();
    }

    this.abortRequested = true;
    this.abortController?.abort();
    this.patchSnapshot({ status: "stopping" });
    this.emitEvent({ type: "status", snapshot: this.getSnapshot() });

    const pid = this.host?.child.pid ?? this.snapshot.hostPid ?? undefined;
    killProcessTree(pid ?? undefined);
    this.host = undefined;

    if (this.runPromise !== undefined) {
      try {
        await this.runPromise;
      } catch {
        // runScenario may reject after forced kill; snapshot already carries error.
      }
    }

    if (this.snapshot.status !== "finished") {
      this.patchSnapshot({
        status: "finished",
        hostPid: null,
        heartbeatAlive: false,
        failureReason: this.snapshot.failureReason ?? "Run stopped by operator.",
      });
      this.emitEvent({ type: "status", snapshot: this.getSnapshot() });
    }

    return this.getSnapshot();
  }

  /**
   * Returns false when any previously tracked host pid is still alive. Used by
   * the stop acceptance check and by tests.
   */
  hasOrphanHost(): boolean {
    const pid = this.snapshot.hostPid;
    if (pid === null || pid === undefined) return false;
    return isProcessAlive(pid);
  }

  private async execute(
    runId: string,
    scenarioPath: string,
    request: StartRunRequest,
    bounds: {
      seed: number;
      maxSteps: number;
      maxDurationMs: number;
      livenessThresholdMs: number;
      startupTimeoutMs: number;
      outDir: string;
    },
  ): Promise<void> {
    try {
      const scenario = await loadScenario(scenarioPath);
      this.patchSnapshot({ scenarioName: scenario.name });
      this.emitEvent({ type: "status", snapshot: this.getSnapshot() });

      const config: RunConfig = {
        seed: bounds.seed,
        maxSteps: bounds.maxSteps,
        maxDurationMs: bounds.maxDurationMs,
        livenessThresholdMs: bounds.livenessThresholdMs,
        startupTimeoutMs: bounds.startupTimeoutMs,
        hostPath: request.hostPath,
        hostCommand: this.options.hostCommandOverride,
        projectPath: request.projectPath,
        importDir: request.importDir,
        reuseProject: request.reuseProject ?? false,
        outDir: bounds.outDir,
      };

      const hooks = this.buildHooks();
      const result = await runScenario(scenario, config, hooks);
      if (this.abortRequested && result.verdict !== "pass") {
        // Forced kill often surfaces as crash/correctness; keep operator reason.
        this.finish(result, "Run stopped by operator.");
      } else {
        this.finish(result);
      }
    } catch (error) {
      const message = error instanceof Error ? error.message : String(error);
      this.patchSnapshot({
        status: "finished",
        hostPid: null,
        probeSocket: null,
        heartbeatAlive: false,
        error: message,
        failureReason: message,
      });
      this.emitEvent({ type: "status", snapshot: this.getSnapshot() });
    }
  }

  private buildHooks(): RunProgressHooks {
    return {
      signal: this.abortController?.signal,
      onHostReady: (host) => {
        this.host = host;
        this.patchSnapshot({
          status: this.abortRequested ? "stopping" : "running",
          hostPid: host.child.pid ?? null,
          probeSocket: host.probeSocket,
        });
        this.emitEvent({ type: "status", snapshot: this.getSnapshot() });
        if (this.abortRequested) {
          killProcessTree(host.child.pid);
        }
      },
      onLog: (line, stream) => {
        const at = this.clock();
        this.emitEvent({ type: "log", line, stream, at });
      },
      onHeartbeat: (counter, guiTimeMs) => {
        const at = this.clock();
        this.patchSnapshot({
          heartbeat: { counter, guiTimeMs, lastSeenAt: at },
          heartbeatAlive: true,
        });
        this.emitEvent({ type: "heartbeat", counter, guiTimeMs, at });
        this.emitEvent({ type: "status", snapshot: this.getSnapshot() });
      },
      onStepStart: (info) => {
        const at = this.clock();
        this.patchSnapshot({
          currentNodeId: info.nodeId,
          currentOp: info.op,
          stepCounter: info.seq,
        });
        this.emitEvent({ type: "stepStart", ...info, at });
        this.emitEvent({ type: "status", snapshot: this.getSnapshot() });
      },
      onStepEnd: (step: StepRecord) => {
        const at = this.clock();
        this.patchSnapshot({
          currentNodeId: step.nodeId,
          currentOp: step.op,
          stepCounter: step.seq,
        });
        this.emitEvent({ type: "stepEnd", step, at });
        this.emitEvent({ type: "status", snapshot: this.getSnapshot() });
      },
    };
  }

  private finish(result: RunResult, overrideReason?: string): void {
    this.host = undefined;
    this.patchSnapshot({
      status: "finished",
      hostPid: null,
      probeSocket: result.probeSocket ?? this.snapshot.probeSocket,
      verdict: result.verdict,
      failureReason: overrideReason ?? result.failure?.reason ?? null,
      stepCounter: result.steps.length,
      currentNodeId: result.steps.at(-1)?.nodeId ?? this.snapshot.currentNodeId,
      currentOp: (result.steps.at(-1)?.op as Op | undefined) ?? this.snapshot.currentOp,
      heartbeatAlive: false,
    });
    const snapshot = this.getSnapshot();
    this.emitEvent({ type: "finished", result, snapshot });
    this.emitEvent({ type: "status", snapshot });
  }

  private patchSnapshot(partial: Partial<ActiveRunSnapshot>): void {
    this.snapshot = this.withElapsed({ ...this.snapshot, ...partial });
  }

  private withElapsed(snapshot: ActiveRunSnapshot): ActiveRunSnapshot {
    const elapsedMs =
      snapshot.startedAt === null ? 0 : Math.max(0, this.clock() - snapshot.startedAt);
    if (elapsedMs === snapshot.elapsedMs) return snapshot;
    return { ...snapshot, elapsedMs };
  }

  private emitEvent(event: RunManagerEvent): void {
    this.emit("event", event);
  }
}
