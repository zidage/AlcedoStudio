//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

/**
 * Scenario loader: parses a YAML file, validates it against the JSON Schema and
 * semantic rules, and normalizes inline matcher keys into typed {@link Scenario}
 * fields with an insertion-ordered node map.
 */

import { readFile } from "node:fs/promises";

import yaml from "js-yaml";

import { MATCHERS, type Matcher } from "./protocol.js";
import { validateScenario } from "./schema.js";
import type { Expect, Op, Scenario, ScenarioDefaults, ScenarioNode } from "./scenario.js";

/** Thrown when a scenario file is malformed or fails validation. */
export class ScenarioError extends Error {
  constructor(message: string) {
    super(message);
    this.name = "ScenarioError";
  }
}

interface RawExpect {
  target: string;
  property: string;
  timeoutMs?: number;
  [key: string]: unknown;
}

interface RawOp {
  action: string;
  target?: string;
  property?: string;
  timeoutMs?: number;
  text?: string;
  key?: number;
  ctrl?: boolean;
  shift?: boolean;
  alt?: boolean;
  fromNx?: number;
  toNx?: number;
  ny?: number;
  steps?: number;
  ms?: number;
  [key: string]: unknown;
}

interface RawEdge {
  to: string;
  weight: number;
}

interface RawNode {
  op: RawOp;
  expect?: RawExpect[];
  next?: RawEdge[];
}

interface RawScenario {
  name: string;
  start: string;
  defaults?: ScenarioDefaults;
  nodes: Record<string, RawNode>;
}

/**
 * Loads and validates a scenario file from disk.
 *
 * @throws {ScenarioError} when the YAML is malformed or validation fails.
 */
export async function loadScenario(path: string): Promise<Scenario> {
  const text = await readFile(path, "utf8");
  return parseScenario(text);
}

/** Parses and validates a scenario from a YAML string. */
export function parseScenario(text: string): Scenario {
  let raw: unknown;
  try {
    raw = yaml.load(text);
  } catch (error) {
    throw new ScenarioError(`YAML parse failed: ${(error as Error).message}`);
  }

  const result = validateScenario(raw);
  if (!result.valid) {
    throw new ScenarioError(`Invalid scenario:\n  - ${result.errors.join("\n  - ")}`);
  }

  return normalize(raw as RawScenario);
}

function normalize(raw: RawScenario): Scenario {
  const nodes = new Map<string, ScenarioNode>();
  for (const [id, node] of Object.entries(raw.nodes)) {
    nodes.set(id, normalizeNode(node, raw.defaults ?? {}));
  }
  return {
    name: raw.name,
    start: raw.start,
    defaults: raw.defaults ?? {},
    nodes,
  };
}

function normalizeNode(raw: RawNode, defaults: ScenarioDefaults): ScenarioNode {
  const node: ScenarioNode = { op: normalizeOp(raw.op) };
  if (raw.expect !== undefined) {
    node.expect = raw.expect.map((expect) => normalizeExpect(expect, defaults));
  }
  if (raw.next !== undefined) {
    node.next = raw.next.map((edge) => ({ to: edge.to, weight: edge.weight }));
  }
  return node;
}

function normalizeOp(raw: RawOp): Op {
  const { matcher, expected } = extractMatcher(raw);
  const op: Op = { action: raw.action as Op["action"] };
  if (raw.target !== undefined) op.target = raw.target;
  if (raw.property !== undefined) op.property = raw.property;
  if (matcher !== undefined) op.matcher = matcher;
  if (expected !== undefined) op.expected = expected;
  if (raw.timeoutMs !== undefined) op.timeoutMs = raw.timeoutMs;
  if (raw.text !== undefined) op.text = raw.text;
  if (raw.key !== undefined) op.key = raw.key;
  if (raw.ctrl !== undefined) op.ctrl = raw.ctrl;
  if (raw.shift !== undefined) op.shift = raw.shift;
  if (raw.alt !== undefined) op.alt = raw.alt;
  if (raw.fromNx !== undefined) op.fromNx = raw.fromNx;
  if (raw.toNx !== undefined) op.toNx = raw.toNx;
  if (raw.ny !== undefined) op.ny = raw.ny;
  if (raw.steps !== undefined) op.steps = raw.steps;
  if (raw.ms !== undefined) op.ms = raw.ms;
  return op;
}

function normalizeExpect(raw: RawExpect, defaults: ScenarioDefaults): Expect {
  const { matcher, expected } = extractMatcher(raw);
  const expect: Expect = {
    target: raw.target,
    property: raw.property,
    matcher: matcher ?? "truthy",
    expected: expected ?? true,
  };
  if (raw.timeoutMs !== undefined) {
    expect.timeoutMs = raw.timeoutMs;
  } else if (defaults.expectTimeoutMs !== undefined) {
    expect.timeoutMs = defaults.expectTimeoutMs;
  }
  return expect;
}

/**
 * Extracts the single inline matcher from a raw op/expect object.
 *
 * @returns the matcher name and its expected value. `truthy` yields `expected = true`
 * because the probe ignores any value for `truthy`.
 */
function extractMatcher(raw: Record<string, unknown>): {
  matcher: Matcher | undefined;
  expected: unknown;
} {
  for (const matcher of MATCHERS) {
    if (Object.prototype.hasOwnProperty.call(raw, matcher)) {
      return { matcher, expected: matcher === "truthy" ? true : raw[matcher] };
    }
  }
  return { matcher: undefined, expected: undefined };
}