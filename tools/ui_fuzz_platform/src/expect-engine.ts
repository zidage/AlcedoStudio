//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

/**
 * Expect engine and op compiler: translates scenario DSL nodes into probe
 * requests. {@link compileExpect} is the expect engine — it compiles an
 * {@link Expect} into a `wait` request, applying the per-expect `timeoutMs`
 * override then the scenario default then the built-in fallback. {@link compileOp}
 * compiles an {@link Op} into the matching input/read/screenshot request.
 *
 * Both return id-less requests; the probe client owns id assignment so a single
 * monotonic counter correlates every request with its reply.
 */

import type { ProbeRequest } from "./protocol.js";
import type { Expect, Op, ScenarioDefaults } from "./scenario.js";

/** A probe request without the client-assigned `id`. */
export type OutgoingRequest = Omit<ProbeRequest, "id">;

/** Default wait timeout when neither the expect nor the scenario specifies one. */
export const DEFAULT_EXPECT_TIMEOUT_MS = 8000;

/** Resolves the effective wait timeout for an expect: per-expect, then default, then fallback. */
export function resolveExpectTimeoutMs(
  expect: Pick<Expect, "timeoutMs">,
  defaults: ScenarioDefaults,
): number {
  if (expect.timeoutMs !== undefined) return expect.timeoutMs;
  if (defaults.expectTimeoutMs !== undefined) return defaults.expectTimeoutMs;
  return DEFAULT_EXPECT_TIMEOUT_MS;
}

/**
 * Compiles an expect into a `wait` request body.
 *
 * The matcher key carries its expected value (omitted for `truthy`, which the
 * probe treats as a presence/truth check with no expected value).
 */
export function compileExpect(
  expect: Expect,
  defaults: ScenarioDefaults,
): OutgoingRequest {
  const timeoutMs = resolveExpectTimeoutMs(expect, defaults);
  const request: OutgoingRequest = {
    method: "wait",
    target: expect.target,
    property: expect.property,
    timeoutMs,
  };
  if (expect.matcher !== "truthy") request[expect.matcher] = expect.expected;
  return request;
}

/** Requires a string field, throwing a descriptive error when it is absent. */
function requireString(op: Op, field: string): string {
  const value = op[field as keyof Op];
  if (typeof value !== "string" || value.length === 0) {
    throw new Error(
      `Op action '${op.action}' requires a non-empty '${field}'.`,
    );
  }
  return value;
}

/** Compiles an op into the matching probe request body. `waitMs` yields `undefined`
 *  because it is a runner-side pause with no probe round-trip. */
export function compileOp(op: Op): OutgoingRequest | undefined {
  switch (op.action) {
    case "click":
      return {
        method: "click",
        target: requireString(op, "target"),
        ...(op.readyTimeoutMs !== undefined && {
          readyTimeoutMs: op.readyTimeoutMs,
        }),
      };
    case "doubleClick":
      return {
        method: "doubleClick",
        target: requireString(op, "target"),
        ...(op.readyTimeoutMs !== undefined && {
          readyTimeoutMs: op.readyTimeoutMs,
        }),
      };
    case "rightClick":
      return {
        method: "rightClick",
        target: requireString(op, "target"),
        ...(op.readyTimeoutMs !== undefined && {
          readyTimeoutMs: op.readyTimeoutMs,
        }),
      };
    case "drag": {
      const request: OutgoingRequest = {
        method: "drag",
        target: requireString(op, "target"),
      };
      if (op.readyTimeoutMs !== undefined)
        request.readyTimeoutMs = op.readyTimeoutMs;
      if (op.fromNx !== undefined) request.fromNx = op.fromNx;
      if (op.toNx !== undefined) request.toNx = op.toNx;
      if (op.ny !== undefined) request.ny = op.ny;
      if (op.steps !== undefined) request.steps = op.steps;
      return request;
    }
    case "key": {
      if (op.key === undefined)
        throw new Error("Op action 'key' requires an integer 'key'.");
      const request: OutgoingRequest = { method: "key", key: op.key };
      if (op.text !== undefined) request.text = op.text;
      if (op.ctrl) request.ctrl = true;
      if (op.shift) request.shift = true;
      if (op.alt) request.alt = true;
      return request;
    }
    case "typeText":
      return { method: "typeText", text: requireString(op, "text") };
    case "wait": {
      if (op.matcher === undefined)
        throw new Error("Op action 'wait' requires a matcher.");
      const request: OutgoingRequest = {
        method: "wait",
        target: requireString(op, "target"),
        property: requireString(op, "property"),
        timeoutMs: op.timeoutMs ?? DEFAULT_EXPECT_TIMEOUT_MS,
      };
      if (op.matcher !== "truthy") request[op.matcher] = op.expected;
      return request;
    }
    case "screenshot":
      return { method: "screenshot" };
    case "waitMs":
      // Runner-side pause: no probe request is dispatched.
      return undefined;
  }
}
