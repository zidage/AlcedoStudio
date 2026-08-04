//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

/**
 * JSON Schema for the scenario YAML DSL, plus an ajv validator.
 *
 * The schema validates the raw YAML shape (inline matcher keys, action enum,
 * required node/op/expect/edge fields). Semantic checks that JSON Schema cannot
 * express — exactly one matcher per wait/expect, action/field compatibility, and
 * node-reference integrity (`start` and every `next.to` must name a real node) —
 * live in {@link validateScenario}, which layers them on top of the schema result.
 *
 * The flow editor (Phase 5) reuses {@link SCENARIO_SCHEMA} so hand-authored and
 * editor-assembled files validate identically.
 */

import { Ajv } from "ajv";

import { MATCHERS, type Matcher } from "./protocol.js";
import type { Action } from "./scenario.js";

const ACTIONS: readonly Action[] = [
  "click",
  "rightClick",
  "doubleClick",
  "key",
  "typeText",
  "drag",
  "wait",
  "waitMs",
  "screenshot",
] as const;

/** Matcher keys as a plain string list for JSON Schema `enum`/`properties`. */
const MATCHER_KEYS = MATCHERS as readonly string[];

/**
 * Property entries that admit any JSON value. Used so matcher keys and action
 * parameters are allowed while `additionalProperties: false` rejects typos.
 */
const anyValue = {} as const;

/** JSON Schema (draft-07) for the scenario DSL. */
export const SCENARIO_SCHEMA = {
  $schema: "http://json-schema.org/draft-07/schema#",
  $id: "https://alcedo.app/ui-fuzz/scenario.schema.json",
  type: "object",
  additionalProperties: false,
  required: ["name", "start", "nodes"],
  properties: {
    name: { type: "string", minLength: 1 },
    start: { type: "string", minLength: 1 },
    defaults: {
      type: "object",
      additionalProperties: false,
      properties: { expectTimeoutMs: { type: "number", minimum: 0 } },
    },
    nodes: {
      type: "object",
      minProperties: 1,
      additionalProperties: { $ref: "#/definitions/node" },
    },
  },
  definitions: {
    node: {
      type: "object",
      additionalProperties: false,
      required: ["op"],
      properties: {
        op: { $ref: "#/definitions/op" },
        expect: {
          type: "array",
          items: { $ref: "#/definitions/expect" },
        },
        next: {
          type: "array",
          items: { $ref: "#/definitions/edge" },
        },
      },
    },
    op: {
      type: "object",
      additionalProperties: false,
      required: ["action"],
      properties: {
        action: { enum: ACTIONS as string[] },
        target: { type: "string" },
        property: { type: "string" },
        timeoutMs: { type: "number", minimum: 0 },
        text: { type: "string" },
        key: { type: "integer", minimum: 0 },
        ctrl: { type: "boolean" },
        shift: { type: "boolean" },
        alt: { type: "boolean" },
        fromNx: { type: "number" },
        toNx: { type: "number" },
        ny: { type: "number" },
        steps: { type: "integer", minimum: 1 },
        ms: { type: "number", minimum: 0 },
        ...Object.fromEntries(MATCHER_KEYS.map((m) => [m, anyValue])),
      },
    },
    expect: {
      type: "object",
      additionalProperties: false,
      required: ["target", "property"],
      properties: {
        target: { type: "string", minLength: 1 },
        property: { type: "string", minLength: 1 },
        timeoutMs: { type: "number", minimum: 0 },
        ...Object.fromEntries(MATCHER_KEYS.map((m) => [m, anyValue])),
      },
      // Exactly one matcher key must be present.
      anyOf: MATCHER_KEYS.map((m) => ({ required: [m] })),
    },
    edge: {
      type: "object",
      additionalProperties: false,
      required: ["to", "weight"],
      properties: {
        to: { type: "string", minLength: 1 },
        weight: { type: "number", minimum: 0 },
      },
    },
  },
} as const;

export interface ScenarioValidationResult {
  readonly valid: boolean;
  readonly errors: readonly string[];
}

const ajv = new Ajv({ allErrors: true, strict: false });
const compiled = ajv.compile(SCENARIO_SCHEMA);

/** Names of the matcher keys present on a raw op/expect object. */
function presentMatchers(raw: Record<string, unknown>): Matcher[] {
  return MATCHERS.filter((m) => Object.prototype.hasOwnProperty.call(raw, m));
}

/**
 * Validates a raw parsed YAML object against the schema and the semantic rules.
 *
 * @returns `valid` plus a flat list of human-readable error strings. On success
 * the caller may hand the object to the loader for normalization.
 */
export function validateScenario(raw: unknown): ScenarioValidationResult {
  if (!compiled(raw)) {
    const errors = (compiled.errors ?? []).map(formatAjvError);
    return { valid: false, errors };
  }

  const semantic: string[] = [];

  const root = raw as {
    start: string;
    nodes: Record<
      string,
      {
        op: Record<string, unknown>;
        expect?: Record<string, unknown>[];
        next?: { to: string; weight: number }[];
      }
    >;
  };
  const nodeIds = new Set(Object.keys(root.nodes));

  if (!nodeIds.has(root.start)) {
    semantic.push(`Scenario 'start' references unknown node '${root.start}'.`);
  }

  for (const [nodeId, node] of Object.entries(root.nodes)) {
    const opMatchers = presentMatchers(node.op);
    if (node.op.action === "wait" && opMatchers.length === 0) {
      semantic.push(`Node '${nodeId}' op action 'wait' requires one matcher.`);
    }
    if (opMatchers.length > 1) {
      semantic.push(`Node '${nodeId}' op has ${opMatchers.length} matchers; exactly one is allowed.`);
    }
    for (const [index, expect] of (node.expect ?? []).entries()) {
      const expectMatchers = presentMatchers(expect);
      if (expectMatchers.length !== 1) {
        semantic.push(
          `Node '${nodeId}' expect[${index}] has ${expectMatchers.length} matchers; exactly one is required.`,
        );
      }
    }
    for (const [index, edge] of (node.next ?? []).entries()) {
      if (!nodeIds.has(edge.to)) {
        semantic.push(`Node '${nodeId}' next[${index}] references unknown node '${edge.to}'.`);
      }
    }
  }

  return semantic.length === 0
    ? { valid: true, errors: [] }
    : { valid: false, errors: semantic };
}

function formatAjvError(error: { instancePath: string; message?: string; params?: unknown }): string {
  const path = error.instancePath || "(root)";
  const detail = error.message ?? JSON.stringify(error.params ?? {});
  return `${path}: ${detail}`;
}