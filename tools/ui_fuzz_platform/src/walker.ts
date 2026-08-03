//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

/**
 * Sequential DAG walker. Phase 2 walks a scenario in declaration order: dispatch
 * each node's op, evaluate its expects (compiled to probe `wait`), then follow the
 * first `next` edge. Edge weights are parsed and stored but not consulted for
 * selection — the seeded weighted random walk is Phase 6.
 *
 * The walker depends only on a {@link ProbePort} (one `request` method) so unit
 * tests can drive it with a controllable fake probe instead of a real socket.
 */

import type { ProbeReply } from "./protocol.js";
import { compileExpect, compileOp, type OutgoingRequest } from "./expect-engine.js";
import { waitActual } from "./protocol.js";
import type { Expect, Op, Scenario } from "./scenario.js";

/** The minimal probe surface the walker needs. The real ProbeClient satisfies this. */
export interface ProbePort {
  request(req: OutgoingRequest, timeoutMs?: number): Promise<ProbeReply>;
}

/** Run verdict reported by the walker. */
export type Verdict = "pass" | "correctness" | "deadlock" | "crash";

/** Result of evaluating one expect against the probe. */
export interface ExpectResult {
  readonly expect: Expect;
  readonly ok: boolean;
  readonly actual: unknown;
  readonly errorCode?: string;
  readonly errorMessage?: string;
}

/** One recorded step in the walk. */
export interface StepRecord {
  readonly seq: number;
  readonly nodeId: string;
  readonly op: Op;
  readonly opOk: boolean;
  readonly opErrorCode?: string;
  readonly opErrorMessage?: string;
  readonly expectResults: readonly ExpectResult[];
  readonly startedAt: number;
  readonly endedAt: number;
}

/** The full walk outcome. */
export interface WalkResult {
  readonly verdict: Verdict;
  readonly steps: readonly StepRecord[];
  readonly startedAt: number;
  readonly endedAt: number;
  readonly failure?: WalkFailure;
}

/** Details of the step that ended the walk with a non-pass verdict. */
export interface WalkFailure {
  readonly nodeId: string;
  readonly kind: "op" | "expect";
  readonly expectIndex?: number;
  readonly reason: string;
  readonly lastSnapshot?: unknown;
}

/** Grace added to a wait timeout so the probe's timeout reply beats the client's. */
export const WAIT_GRACE_MS = 2000;

/** Walk configuration. */
export interface WalkConfig {
  readonly maxSteps: number;
  readonly maxDurationMs: number;
  /** Injectable sleep for `waitMs` ops; defaults to real setTimeout. */
  readonly sleep?: (ms: number) => Promise<void>;
}

function defaultSleep(ms: number): Promise<void> {
  const { promise, resolve } = Promise.withResolvers<void>();
  setTimeout(resolve, ms);
  return promise;
}

/**
 * Runs a scenario sequentially against `probe`.
 *
 * Stops with `pass` when a terminal node (no `next`) is reached or the step/duration
 * bounds are hit; stops with `correctness` when an op or expect fails. Deadlock and
 * crash verdicts are set by the run orchestrator (liveness watchdog and child exit),
 * not by the walker itself.
 */
export async function walk(scenario: Scenario, probe: ProbePort, config: WalkConfig): Promise<WalkResult> {
  const sleep = config.sleep ?? defaultSleep;
  const startedAt = Date.now();
  const steps: StepRecord[] = [];
  let currentId = scenario.start;

  while (true) {
    if (steps.length >= config.maxSteps) {
      return passResult(steps, startedAt);
    }
    if (Date.now() - startedAt > config.maxDurationMs) {
      return passResult(steps, startedAt);
    }

    const node = scenario.nodes.get(currentId);
    if (node === undefined) {
      return failResult(steps, startedAt, {
        nodeId: currentId,
        kind: "op",
        reason: `Scenario referenced unknown node '${currentId}'.`,
      });
    }

    const stepStartedAt = Date.now();
    const opOutcome = await dispatchOp(node.op, probe, sleep);
    if (!opOutcome.ok) {
      const step: StepRecord = {
        seq: steps.length + 1,
        nodeId: currentId,
        op: node.op,
        opOk: false,
        opErrorCode: opOutcome.errorCode,
        opErrorMessage: opOutcome.errorMessage,
        expectResults: [],
        startedAt: stepStartedAt,
        endedAt: Date.now(),
      };
      steps.push(step);
      return failResult(steps, startedAt, {
        nodeId: currentId,
        kind: "op",
        reason: `op '${node.op.action}' failed: ${opOutcome.errorCode ?? "unknown"} (${opOutcome.errorMessage ?? ""})`,
      });
    }

    const expectResults = await evaluateExpects(node.expect ?? [], scenario.defaults, probe);
    const failingExpect = expectResults.findIndex((result) => !result.ok);
    const step: StepRecord = {
      seq: steps.length + 1,
      nodeId: currentId,
      op: node.op,
      opOk: true,
      expectResults,
      startedAt: stepStartedAt,
      endedAt: Date.now(),
    };
    steps.push(step);

    if (failingExpect >= 0) {
      const failed = expectResults[failingExpect]!;
      return failResult(steps, startedAt, {
        nodeId: currentId,
        kind: "expect",
        expectIndex: failingExpect,
        reason: describeExpectFailure(failed),
      });
    }

    const edges = node.next;
    if (edges === undefined || edges.length === 0) {
      return passResult(steps, startedAt);
    }
    // Phase 2: first edge in declaration order. The seeded weighted walk is Phase 6.
    currentId = edges[0]!.to;
  }
}

interface OpOutcome {
  ok: boolean;
  errorCode?: string;
  errorMessage?: string;
}

async function dispatchOp(op: Op, probe: ProbePort, sleep: (ms: number) => Promise<void>): Promise<OpOutcome> {
  if (op.action === "waitMs") {
    await sleep(op.ms ?? 0);
    return { ok: true };
  }
  const request = compileOp(op);
  if (request === undefined) {
    return { ok: false, errorCode: "uncompilable_op", errorMessage: `op '${op.action}' could not be compiled` };
  }
  const timeoutMs = op.action === "wait" ? (op.timeoutMs ?? 8000) + WAIT_GRACE_MS : undefined;
  let reply: ProbeReply;
  try {
    reply = await probe.request(request, timeoutMs);
  } catch (error) {
    return { ok: false, errorCode: "request_error", errorMessage: (error as Error).message };
  }
  if (reply.ok) return { ok: true };
  const error = reply.error;
  return {
    ok: false,
    errorCode: typeof error?.code === "string" ? error.code : undefined,
    errorMessage: typeof error?.message === "string" ? error.message : undefined,
  };
}

async function evaluateExpects(
  expects: readonly Expect[],
  defaults: Scenario["defaults"],
  probe: ProbePort,
): Promise<ExpectResult[]> {
  const results: ExpectResult[] = [];
  for (const expect of expects) {
    const request = compileExpect(expect, defaults);
    const timeoutMs = (expect.timeoutMs ?? defaults.expectTimeoutMs ?? 8000) + WAIT_GRACE_MS;
    let reply: ProbeReply;
    try {
      reply = await probe.request(request, timeoutMs);
    } catch (error) {
      results.push({
        expect,
        ok: false,
        actual: undefined,
        errorCode: "request_error",
        errorMessage: (error as Error).message,
      });
      continue;
    }
    const actual = waitActual(reply);
    if (reply.ok) {
      results.push({ expect, ok: true, actual });
    } else {
      results.push({
        expect,
        ok: false,
        actual,
        errorCode: typeof reply.error?.code === "string" ? reply.error.code : undefined,
        errorMessage: typeof reply.error?.message === "string" ? reply.error.message : undefined,
      });
    }
  }
  return results;
}

function describeExpectFailure(result: ExpectResult): string {
  const { expect } = result;
  const target = `${expect.target}.${expect.property}`;
  if (result.errorCode === "wait_timeout") {
    return `expect ${target} ${expect.matcher} ${JSON.stringify(expect.expected)} timed out; last observed: ${JSON.stringify(result.actual)}`;
  }
  return `expect ${target} ${expect.matcher} ${JSON.stringify(expect.expected)} failed: ${result.errorCode ?? "unknown"} (${result.errorMessage ?? ""})`;
}

function passResult(steps: StepRecord[], startedAt: number): WalkResult {
  return { verdict: "pass", steps, startedAt, endedAt: Date.now() };
}

function failResult(steps: StepRecord[], startedAt: number, failure: WalkFailure): WalkResult {
  return { verdict: "correctness", steps, startedAt, endedAt: Date.now(), failure };
}