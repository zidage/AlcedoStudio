//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

import { describe, expect, it } from "vitest";

import type { OutgoingRequest } from "../src/expect-engine.js";
import type { ProbeReply } from "../src/protocol.js";
import type { Expect, Op, Scenario, ScenarioNode } from "../src/scenario.js";
import { walk, type ProbePort, type Verdict } from "../src/walker.js";

/** A probe that returns scripted replies in order and records every request. */
class ScriptedProbe implements ProbePort {
  readonly requests: OutgoingRequest[] = [];
  private readonly replies: ProbeReply[];

  constructor(replies: ProbeReply[]) {
    this.replies = [...replies];
  }

  async request(req: OutgoingRequest): Promise<ProbeReply> {
    this.requests.push(req);
    const reply = this.replies.shift();
    if (reply === undefined) throw new Error("ScriptedProbe ran out of replies");
    return reply;
  }
}

function makeScenario(start: string, nodes: Record<string, ScenarioNode>): Scenario {
  return { name: "t", start, defaults: {}, nodes: new Map(Object.entries(nodes)) };
}

function node(op: Op, expect?: Expect[], next?: { to: string; weight: number }[]): ScenarioNode {
  return { op, expect, next };
}

const ok = (): ProbeReply => ({ ok: true, result: "ok" });
const waitOk = (actual: unknown): ProbeReply => ({ ok: true, result: "ok", actual });
const waitTimeout = (actual: unknown): ProbeReply => ({
  ok: false,
  error: { code: "wait_timeout", message: "timed out", actual, target: "t", property: "p" },
});
const opError = (code: string): ProbeReply => ({ ok: false, error: { code, message: code } });

describe("walk", () => {
  it("walks a linear scenario and passes at the terminal node", async () => {
    const scenario = makeScenario("a", {
      a: node({ action: "click", target: "btn" }, [{ target: "panel", property: "visible", matcher: "eq", expected: true }], [{ to: "b", weight: 1 }]),
      b: node({ action: "wait", target: "panel", property: "count", matcher: "gte", expected: 1 }),
    });
    const probe = new ScriptedProbe([ok(), waitOk(true), ok()]);

    const result = await walk(scenario, probe, { maxSteps: 100, maxDurationMs: 60_000 });

    expect(result.verdict).toBe("pass");
    expect(result.steps).toHaveLength(2);
    expect(result.steps[0]!.nodeId).toBe("a");
    expect(result.steps[1]!.nodeId).toBe("b");
  });

  it("records each step with its op, op result, and expect results", async () => {
    const scenario = makeScenario("a", {
      a: node({ action: "click", target: "btn" }, [{ target: "panel", property: "visible", matcher: "eq", expected: true }], [{ to: "b", weight: 1 }]),
      b: node({ action: "screenshot" }),
    });
    const probe = new ScriptedProbe([ok(), waitOk(true), ok()]);

    const result = await walk(scenario, probe, { maxSteps: 100, maxDurationMs: 60_000 });

    expect(result.steps[0]!.opOk).toBe(true);
    expect(result.steps[0]!.expectResults).toHaveLength(1);
    expect(result.steps[0]!.expectResults[0]!.ok).toBe(true);
    expect(result.steps[0]!.expectResults[0]!.actual).toBe(true);
  });

  it("follows the first next edge in declaration order", async () => {
    const scenario = makeScenario("a", {
      a: node({ action: "click", target: "btn" }, undefined, [
        { to: "first", weight: 1 },
        { to: "second", weight: 9 },
      ]),
      first: node({ action: "screenshot" }),
      second: node({ action: "typeText", text: "x" }),
    });
    const probe = new ScriptedProbe([ok(), ok()]);

    const result = await walk(scenario, probe, { maxSteps: 100, maxDurationMs: 60_000 });

    expect(result.verdict).toBe("pass");
    expect(result.steps[1]!.nodeId).toBe("first");
    expect(result.steps[1]!.op.action).toBe("screenshot");
  });

  it("stops with correctness when an op fails", async () => {
    const scenario = makeScenario("a", {
      a: node({ action: "click", target: "missing" }, [{ target: "panel", property: "visible", matcher: "eq", expected: true }]),
    });
    const probe = new ScriptedProbe([opError("target_not_found")]);

    const result = await walk(scenario, probe, { maxSteps: 100, maxDurationMs: 60_000 });

    expect(result.verdict).toBe("correctness");
    expect(result.failure?.kind).toBe("op");
    expect(result.failure?.reason).toContain("target_not_found");
  });

  it("stops with correctness when an expect times out and records the last observed value", async () => {
    const scenario = makeScenario("a", {
      a: node(
        { action: "doubleClick", target: "card" },
        [
          { target: "editorWorkspace", property: "visible", matcher: "eq", expected: true },
          { target: "editorSessionStatus", property: "text", matcher: "contains", expected: "Ready", timeoutMs: 100 },
        ],
      ),
    });
    const probe = new ScriptedProbe([ok(), waitOk(true), waitTimeout("Running")]);

    const result = await walk(scenario, probe, { maxSteps: 100, maxDurationMs: 60_000 });

    expect(result.verdict).toBe("correctness");
    expect(result.failure?.kind).toBe("expect");
    expect(result.failure?.reason).toContain("Running");
  });

  it("sleeps for waitMs ops without a probe round-trip", async () => {
    const sleeps: number[] = [];
    const sleep = (ms: number): Promise<void> => {
      sleeps.push(ms);
      return Promise.resolve();
    };
    const scenario = makeScenario("a", {
      a: node({ action: "waitMs", ms: 250 }, undefined, [{ to: "b", weight: 1 }]),
      b: node({ action: "screenshot" }),
    });
    const probe = new ScriptedProbe([ok()]);

    const result = await walk(scenario, probe, { maxSteps: 100, maxDurationMs: 60_000, sleep });

    expect(sleeps).toEqual([250]);
    // Only the screenshot op reached the probe; the waitMs op did not.
    expect(probe.requests.map((r) => r.method)).toEqual(["screenshot"]);
    expect(result.verdict).toBe("pass");
  });

  it("stops with pass when maxSteps is reached before a terminal node", async () => {
    const scenario = makeScenario("a", {
      a: node({ action: "click", target: "btn" }, undefined, [{ to: "b", weight: 1 }]),
      b: node({ action: "click", target: "btn2" }, undefined, [{ to: "a", weight: 1 }]),
    });
    const probe = new ScriptedProbe([ok(), ok()]);

    const result = await walk(scenario, probe, { maxSteps: 1, maxDurationMs: 60_000 });

    expect(result.verdict).toBe("pass");
    expect(result.steps).toHaveLength(1);
  });

  it("reports correctness when a node references an unknown successor", async () => {
    const scenario = makeScenario("a", {
      a: node({ action: "click", target: "btn" }, undefined, [{ to: "missing", weight: 1 }]),
    });
    const probe = new ScriptedProbe([ok()]);

    const result = await walk(scenario, probe, { maxSteps: 100, maxDurationMs: 60_000 });

    expect(result.verdict).toBe("correctness");
    expect(result.failure?.reason).toContain("missing");
  });
});