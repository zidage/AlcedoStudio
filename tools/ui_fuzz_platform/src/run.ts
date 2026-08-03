//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

/**
 * Run orchestrator. Ties together the host process, probe client, liveness
 * watchdog, and walker into one `runScenario` call. It spawns the test host,
 * connects the JSON Lines channel, awaits `ready`, walks the scenario, and on
 * any non-pass verdict captures a failure bundle (operation history, log tail,
 * last tree snapshot, screenshot when the window is still responsive).
 *
 * Verdict mapping: the walker reports `pass` or `correctness`; this orchestrator
 * overrides to `deadlock` when the heartbeat gap exceeds the liveness threshold
 * and to `crash` when the child process exits before the walk completes.
 */

import { connectProbe, type ProbeClient } from "./probe-client.js";
import { captureFailureBundle, type FailureBundle } from "./failure-bundle.js";
import { spawnHost, type HostHandle } from "./host-process.js";
import { LivenessWatchdog } from "./liveness.js";
import type { ProbeEvent } from "./protocol.js";
import type { Op, RunConfig, Scenario } from "./scenario.js";
import { walk, type StepRecord, type Verdict, type WalkResult } from "./walker.js";

/** Size of the Qt log tail kept in the failure bundle, in KiB. */
export const FAILURE_LOG_TAIL_KIB = 64;

/** Live progress hooks used by the Phase 3 process manager / dashboard. */
export interface RunProgressHooks {
  /** When aborted, the walker stops promptly (including mid-`waitMs`). */
  readonly signal?: AbortSignal;
  /** Called once the host has printed `PROBE_SOCKET=` and the handle is ready. */
  readonly onHostReady?: (host: HostHandle) => void;
  /** Forwarded child log lines (stdout/stderr). */
  readonly onLog?: (line: string, stream: "stdout" | "stderr") => void;
  /** Forwarded probe heartbeat events. */
  readonly onHeartbeat?: (counter: number, guiTimeMs: number) => void;
  /** Fired when a walk node begins. */
  readonly onStepStart?: (info: { readonly nodeId: string; readonly op: Op; readonly seq: number }) => void;
  /** Fired after each completed walk step. */
  readonly onStepEnd?: (step: StepRecord) => void;
}

/** The outcome of a full run. */
export interface RunResult {
  readonly scenarioName: string;
  readonly seed: number;
  readonly verdict: Verdict;
  readonly steps: readonly StepRecord[];
  readonly startedAt: number;
  readonly endedAt: number;
  readonly failure?: { readonly kind: "op" | "expect"; readonly nodeId: string; readonly reason: string };
  readonly bundle?: FailureBundle;
  readonly probeSocket?: string;
  readonly exitCode?: number | null;
  /**
   * Last N KiB of Qt stdout/stderr for SQLite failure rows and results browser.
   * Empty string on pass or when the host produced no output.
   */
  readonly logTail: string;
  /** Last tree snapshot captured into the failure bundle, when available. */
  readonly treeSnapshot?: unknown;
}

/**
 * Runs `scenario` against a spawned test host described by `config`.
 *
 * @throws when the host cannot start or never signals `ready`.
 */
export async function runScenario(
  scenario: Scenario,
  config: RunConfig,
  hooks: RunProgressHooks = {},
): Promise<RunResult> {
  const startedAt = Date.now();
  const host = await spawnHost({
    hostPath: config.hostPath ?? "",
    command: config.hostCommand,
    projectPath: config.projectPath,
    importDir: config.importDir,
    reuseProject: config.reuseProject,
    startupTimeoutMs: config.startupTimeoutMs,
    onLog: hooks.onLog,
  });
  hooks.onHostReady?.(host);

  let probe: ProbeClient | undefined;
  try {
    probe = await connectProbe(host.probeSocket, config.startupTimeoutMs);

    // Attach heartbeat forwarding before awaiting ready so early heartbeats are not lost.
    const earlyHeartbeatListener = (event: ProbeEvent) => {
      if (event.event === "heartbeat") {
        const counter = typeof event.counter === "number" ? event.counter : 0;
        const guiTimeMs = typeof event.guiTimeMs === "number" ? event.guiTimeMs : 0;
        hooks.onHeartbeat?.(counter, guiTimeMs);
      }
    };
    probe.on("event", earlyHeartbeatListener);

    const ready = await awaitReady(probe, config.startupTimeoutMs);
    if (!ready) {
      probe.off("event", earlyHeartbeatListener);
      throw new Error(`Test host did not emit 'ready' within ${config.startupTimeoutMs} ms.`);
    }

    const result = await driveWalk(scenario, config, host, probe, hooks, earlyHeartbeatListener);
    return {
      scenarioName: scenario.name,
      seed: config.seed,
      verdict: result.verdict,
      steps: result.walkResult.steps,
      startedAt,
      endedAt: Date.now(),
      failure: result.failure,
      bundle: result.bundle,
      probeSocket: host.probeSocket,
      exitCode: result.exitCode,
      logTail: result.logTail,
      treeSnapshot: result.treeSnapshot,
    };
  } finally {
    probe?.close();
    host.kill();
  }
}

interface DriveResult {
  verdict: Verdict;
  walkResult: WalkResult;
  failure?: { kind: "op" | "expect"; nodeId: string; reason: string };
  bundle?: FailureBundle;
  exitCode?: number | null;
  logTail: string;
  treeSnapshot?: unknown;
}

async function driveWalk(
  scenario: Scenario,
  config: RunConfig,
  host: HostHandle,
  probe: ProbeClient,
  hooks: RunProgressHooks,
  earlyHeartbeatListener: (event: ProbeEvent) => void,
): Promise<DriveResult> {
  let deadlockDetected = false;
  let exitCode: number | null | undefined;

  const watchdog = new LivenessWatchdog(config.livenessThresholdMs, () => {
    deadlockDetected = true;
    probe.close();
  });
  // Replace the early-only listener with one that also feeds the watchdog.
  probe.off("event", earlyHeartbeatListener);
  const eventListener = (event: ProbeEvent) => {
    if (event.event === "heartbeat") {
      const counter = typeof event.counter === "number" ? event.counter : 0;
      const guiTimeMs = typeof event.guiTimeMs === "number" ? event.guiTimeMs : 0;
      watchdog.observeHeartbeat();
      hooks.onHeartbeat?.(counter, guiTimeMs);
    }
  };
  probe.on("event", eventListener);

  let walkResult: WalkResult | undefined;
  const exitListener = (code: number | null) => {
    if (walkResult === undefined) {
      exitCode = code;
      probe.close();
    }
  };
  host.child.on("exit", exitListener);

  watchdog.start();
  try {
    walkResult = await walk(scenario, probe, {
      maxSteps: config.maxSteps,
      maxDurationMs: config.maxDurationMs,
      signal: hooks.signal,
      onStepStart: hooks.onStepStart,
      onStepEnd: hooks.onStepEnd,
    });
  } finally {
    watchdog.stop();
    probe.off("event", eventListener);
    host.child.removeListener("exit", exitListener);
  }

  const result = walkResult!;
  let verdict = result.verdict;
  let failure = result.failure;
  if (hooks.signal?.aborted) {
    verdict = "correctness";
    failure = {
      kind: "op",
      nodeId: result.steps.at(-1)?.nodeId ?? scenario.start,
      reason: "Run aborted by operator.",
    };
  } else if (deadlockDetected) {
    verdict = "deadlock";
    failure = {
      kind: "op",
      nodeId: result.steps.at(-1)?.nodeId ?? scenario.start,
      reason: `GUI thread heartbeat gap exceeded ${config.livenessThresholdMs} ms.`,
    };
  } else if (exitCode !== undefined) {
    verdict = "crash";
    failure = {
      kind: "op",
      nodeId: result.steps.at(-1)?.nodeId ?? scenario.start,
      reason: `Test host process exited (code=${exitCode}).`,
    };
  }

  const logLines = host.logs.text().split("\n");
  const logTail = extractLogTail(logLines, FAILURE_LOG_TAIL_KIB);

  const skipBundle = hooks.signal?.aborted === true || verdict === "pass";
  if (skipBundle) {
    return { verdict, walkResult: result, failure, exitCode, logTail };
  }

  const captured = await captureBundle(scenario, config, host, probe, result, verdict, failure, logLines);
  return {
    verdict,
    walkResult: result,
    failure,
    bundle: captured.bundle,
    exitCode,
    logTail,
    treeSnapshot: captured.treeSnapshot,
  };
}

async function captureBundle(
  scenario: Scenario,
  config: RunConfig,
  host: HostHandle,
  probe: ProbeClient,
  walkResult: WalkResult,
  verdict: Verdict,
  failure: { kind: "op" | "expect"; nodeId: string; reason: string } | undefined,
  logLines: readonly string[],
): Promise<{ bundle: FailureBundle; treeSnapshot?: unknown }> {
  let treeSnapshot: unknown;
  let screenshotPng: Buffer | undefined;
  try {
    const snapshotReply = await probe.request({ method: "snapshot" });
    if (snapshotReply.ok) treeSnapshot = snapshotReply.result;
  } catch {
    // Host may be unresponsive on deadlock/crash; the bundle still carries the log tail.
  }
  try {
    const screenshotReply = await probe.request({ method: "screenshot" });
    if (screenshotReply.ok) {
      const result = screenshotReply.result as { pngBase64?: string } | undefined;
      if (typeof result?.pngBase64 === "string") {
        screenshotPng = Buffer.from(result.pngBase64, "base64");
      }
    }
  } catch {
    // Screenshot is best-effort; absence is recorded in the bundle.
  }

  const bundle = await captureFailureBundle({
    outDir: config.outDir,
    scenarioName: scenario.name,
    seed: config.seed,
    verdict,
    walkResult,
    failure,
    logTailKib: FAILURE_LOG_TAIL_KIB,
    logLines,
    screenshotPng,
    treeSnapshot,
    config,
  });
  return { bundle, treeSnapshot };
}

/** Returns the last `kib` KiB of log lines joined by newlines. */
export function extractLogTail(lines: readonly string[], kib: number): string {
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

/** Waits for the probe `ready` event, resolving `true` on receipt or `false` on timeout. */
function awaitReady(probe: ProbeClient, timeoutMs: number): Promise<boolean> {
  const { promise, resolve } = Promise.withResolvers<boolean>();
  const timer = setTimeout(() => {
    probe.off("event", listener);
    resolve(false);
  }, timeoutMs);
  const listener = (event: ProbeEvent) => {
    if (event.event === "ready") {
      clearTimeout(timer);
      probe.off("event", listener);
      resolve(true);
    }
  };
  probe.on("event", listener);
  return promise;
}