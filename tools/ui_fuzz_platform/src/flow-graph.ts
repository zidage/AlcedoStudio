//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

/**
 * Lossless round-trip between a validated {@link Scenario} and the flow-editor
 * graph model. The model is renderer-agnostic: the React Flow page maps it
 * onto React Flow nodes/edges, and every conversion back goes through the same
 * code path so an editor-assembled workflow serializes to schema-valid YAML
 * that is semantically identical to a hand-authored file.
 *
 * Round-trip guarantees:
 * - node insertion order, `expect` declaration order, and `next` edge
 *   declaration order are preserved (via explicit `expectIndex` / `nextIndex`
 *   carried from load time; editor-created items append);
 * - `expect.timeoutMs` equal to `defaults.expectTimeoutMs` is omitted on save
 *   so files that rely on the default keep their hand-authored shape;
 * - positions are layout metadata only and never affect the serialized YAML.
 *
 * This module is free of Node.js dependencies so the browser editor imports it
 * directly; validation reuses {@link ./schema.js} so hand-authored and
 * editor-assembled files validate identically.
 */

import yaml from "js-yaml";

import { ScenarioError } from "./scenario-parse.js";
import { validateScenario } from "./schema.js";
import type { Expect, Op, Scenario, ScenarioDefaults } from "./scenario.js";

export interface FlowPosition {
  readonly x: number;
  readonly y: number;
}

/** An operation node: one scenario node with its op payload. */
export interface OpNodeModel {
  readonly kind: "op";
  readonly id: string;
  readonly position: FlowPosition;
  readonly op: Op;
  /**
   * Set by {@link scenarioToFlow} when the source node declared `next` (even
   * as an empty array), so serialization keeps the explicit declaration.
   */
  readonly explicitNext?: boolean;
  /** Same as {@link OpNodeModel.explicitNext} but for the `expect` list. */
  readonly explicitExpect?: boolean;
}

/** An assertion node hanging off an operation node via an expect edge. */
export interface ExpectNodeModel {
  readonly kind: "expect";
  readonly id: string;
  readonly position: FlowPosition;
  /** Owning operation node id. */
  readonly owner: string;
  /** Declaration order within the owner's `expect` list at load time. */
  readonly expectIndex?: number;
  readonly expect: Expect;
}

export type FlowNodeModel = OpNodeModel | ExpectNodeModel;

/** A weighted `next` edge between two operation nodes. */
export interface NextEdgeModel {
  readonly kind: "next";
  readonly id: string;
  readonly from: string;
  readonly to: string;
  readonly weight: number;
  /** Declaration order within the source node's `next` list at load time. */
  readonly nextIndex?: number;
}

/** An edge from an operation node to one of its assertion nodes. */
export interface ExpectEdgeModel {
  readonly kind: "expect";
  readonly id: string;
  readonly from: string;
  readonly to: string;
}

export type FlowEdgeModel = NextEdgeModel | ExpectEdgeModel;

/** The complete editor graph: operations, assertions, and both outlet kinds. */
export interface FlowGraphModel {
  readonly name: string;
  readonly startNodeId: string;
  readonly defaults: ScenarioDefaults;
  readonly nodes: readonly FlowNodeModel[];
  readonly edges: readonly FlowEdgeModel[];
}

/** Thrown when the graph cannot be converted into a valid scenario. */
export class FlowGraphError extends Error {
  constructor(message: string) {
    super(message);
    this.name = "FlowGraphError";
  }
}

const COLUMN_WIDTH = 320;
const ROW_HEIGHT = 150;
const EXPECT_OFFSET_X = 300;
const EXPECT_OFFSET_Y = 110;

/**
 * Converts a scenario into the editor graph with a deterministic layered
 * layout: BFS depth from the start node sets the column, declaration order
 * sets the row; unreached nodes trail in their own columns.
 */
export function scenarioToFlow(scenario: Scenario): FlowGraphModel {
  const depths = computeDepths(scenario);
  const nodes: FlowNodeModel[] = [];
  const edges: FlowEdgeModel[] = [];
  const rowCounter = new Map<number, number>();

  const positionFor = (depth: number): FlowPosition => {
    const row = rowCounter.get(depth) ?? 0;
    rowCounter.set(depth, row + 1);
    return { x: depth * COLUMN_WIDTH, y: row * ROW_HEIGHT };
  };

  for (const [nodeId, node] of scenario.nodes) {
    const position = positionFor(depths.get(nodeId) ?? 0);
    nodes.push({
      kind: "op",
      id: nodeId,
      position,
      op: node.op,
      explicitNext: node.next !== undefined,
      explicitExpect: node.expect !== undefined,
    });

    for (const [expectIndex, expect] of (node.expect ?? []).entries()) {
      const expectId = expectNodeId(nodeId, expectIndex);
      nodes.push({
        kind: "expect",
        id: expectId,
        owner: nodeId,
        expectIndex,
        expect,
        position: {
          x: position.x + EXPECT_OFFSET_X,
          y: position.y + (expectIndex + 1) * EXPECT_OFFSET_Y,
        },
      });
      edges.push({ kind: "expect", id: `${nodeId}=>${expectId}`, from: nodeId, to: expectId });
    }

    for (const [nextIndex, edge] of (node.next ?? []).entries()) {
      edges.push({
        kind: "next",
        id: `${nodeId}->${edge.to}#${nextIndex}`,
        from: nodeId,
        to: edge.to,
        weight: edge.weight,
        nextIndex,
      });
    }
  }

  return {
    name: scenario.name,
    startNodeId: scenario.start,
    defaults: scenario.defaults,
    nodes,
    edges,
  };
}

function computeDepths(scenario: Scenario): Map<string, number> {
  const depths = new Map<string, number>();
  if (!scenario.nodes.has(scenario.start)) return depths;
  depths.set(scenario.start, 0);
  const queue = [scenario.start];
  while (queue.length > 0) {
    const current = queue.shift()!;
    const depth = depths.get(current)!;
    for (const edge of scenario.nodes.get(current)?.next ?? []) {
      if (!scenario.nodes.has(edge.to)) continue;
      const known = depths.get(edge.to);
      if (known === undefined || depth + 1 < known) {
        depths.set(edge.to, depth + 1);
        queue.push(edge.to);
      }
    }
  }
  // Unreached nodes trail behind the deepest reached column, in declaration order.
  let trailing = depths.size > 0 ? Math.max(...depths.values()) + 1 : 0;
  for (const nodeId of scenario.nodes.keys()) {
    if (!depths.has(nodeId)) {
      depths.set(nodeId, trailing);
      trailing++;
    }
  }
  return depths;
}

/** Synthesizes the assertion-node id for the `index`-th expect of `opId`. */
export function expectNodeId(opId: string, index: number): string {
  return `${opId}/expect/${index}`;
}

/**
 * Converts the editor graph back into a validated scenario.
 *
 * @throws {FlowGraphError} on dangling edges, unknown start node, duplicate op
 * node ids, or schema/semantic validation failures.
 */
export function flowToScenario(graph: FlowGraphModel): Scenario {
  const opNodes = new Map<string, OpNodeModel>();
  for (const node of graph.nodes) {
    if (node.kind !== "op") continue;
    if (opNodes.has(node.id)) {
      throw new FlowGraphError(`Duplicate operation node id '${node.id}'.`);
    }
    opNodes.set(node.id, node);
  }
  if (opNodes.size === 0) {
    throw new FlowGraphError("The graph has no operation nodes.");
  }
  if (!opNodes.has(graph.startNodeId)) {
    throw new FlowGraphError(`Start node '${graph.startNodeId}' is not an operation node in the graph.`);
  }

  const expectsByOwner = new Map<string, ExpectNodeModel[]>();
  for (const node of graph.nodes) {
    if (node.kind !== "expect") continue;
    if (!opNodes.has(node.owner)) {
      throw new FlowGraphError(`Expect node '${node.id}' belongs to unknown operation '${node.owner}'.`);
    }
    const list = expectsByOwner.get(node.owner) ?? [];
    list.push(node);
    expectsByOwner.set(node.owner, list);
  }

  const nextBySource = new Map<string, NextEdgeModel[]>();
  for (const edge of graph.edges) {
    if (edge.kind === "expect") {
      const target = graph.nodes.find((node) => node.id === edge.to);
      if (target?.kind !== "expect" || target.owner !== edge.from) {
        throw new FlowGraphError(`Expect edge '${edge.id}' does not link an operation to its own assertion node.`);
      }
      continue;
    }
    if (!opNodes.has(edge.from)) {
      throw new FlowGraphError(`Next edge '${edge.id}' starts at unknown operation '${edge.from}'.`);
    }
    if (!opNodes.has(edge.to)) {
      throw new FlowGraphError(`Next edge '${edge.id}' references unknown node '${edge.to}'.`);
    }
    const list = nextBySource.get(edge.from) ?? [];
    list.push(edge);
    nextBySource.set(edge.from, list);
  }

  const nodes = new Map<string, { op: Op; expect?: Expect[]; next?: { to: string; weight: number }[] }>();
  for (const [nodeId, opNode] of opNodes) {
    const entry: { op: Op; expect?: Expect[]; next?: { to: string; weight: number }[] } = { op: opNode.op };

    const expects = (expectsByOwner.get(nodeId) ?? [])
      .slice()
      .sort(compareByLoadOrder((node) => node.expectIndex));
    if (expects.length > 0 || opNode.explicitExpect === true) {
      // Apply the scenario default timeout exactly as the loader does so the
      // graph -> YAML -> parse chain is stable for default-relying expects.
      entry.expect = expects.map((node) =>
        node.expect.timeoutMs === undefined && graph.defaults.expectTimeoutMs !== undefined
          ? { ...node.expect, timeoutMs: graph.defaults.expectTimeoutMs }
          : node.expect,
      );
    }

    const nextEdges = (nextBySource.get(nodeId) ?? [])
      .slice()
      .sort(compareByLoadOrder((edge) => edge.nextIndex));
    if (nextEdges.length > 0 || opNode.explicitNext === true) {
      entry.next = nextEdges.map((edge) => ({ to: edge.to, weight: edge.weight }));
    }

    nodes.set(nodeId, entry);
  }

  return {
    name: graph.name,
    start: graph.startNodeId,
    defaults: graph.defaults,
    nodes,
  };
}

/**
 * Sorts by the explicit load-time declaration index first (undefined last),
 * then by id for deterministic placement of editor-created items.
 */
function compareByLoadOrder<T extends { readonly id: string }>(
  pick: (item: T) => number | undefined,
): (a: T, b: T) => number {
  return (a, b) => {
    const ai = pick(a);
    const bi = pick(b);
    if (ai !== undefined && bi !== undefined && ai !== bi) return ai - bi;
    if (ai !== undefined && bi === undefined) return -1;
    if (ai === undefined && bi !== undefined) return 1;
    return a.id.localeCompare(b.id);
  };
}

/**
 * Converts a scenario to the raw YAML object shape: inline matcher keys,
 * canonical key order, `timeoutMs` omitted when it equals the scenario
 * default so default-relying files keep their hand-authored form.
 */
export function scenarioToRaw(scenario: Scenario): Record<string, unknown> {
  const rawNodes: Record<string, unknown> = {};
  for (const [nodeId, node] of scenario.nodes) {
    const rawNode: Record<string, unknown> = { op: opToRaw(node.op) };
    if (node.expect !== undefined) {
      rawNode.expect = node.expect.map((expect) => expectToRaw(expect, scenario.defaults));
    }
    if (node.next !== undefined) {
      rawNode.next = node.next.map((edge) => ({ to: edge.to, weight: edge.weight }));
    }
    rawNodes[nodeId] = rawNode;
  }

  const raw: Record<string, unknown> = { name: scenario.name, start: scenario.start };
  if (scenario.defaults.expectTimeoutMs !== undefined) {
    raw.defaults = { expectTimeoutMs: scenario.defaults.expectTimeoutMs };
  }
  raw.nodes = rawNodes;
  return raw;
}

function opToRaw(op: Op): Record<string, unknown> {
  const raw: Record<string, unknown> = { action: op.action };
  if (op.target !== undefined) raw.target = op.target;
  if (op.property !== undefined) raw.property = op.property;
  if (op.matcher !== undefined) raw[op.matcher] = op.matcher === "truthy" ? true : op.expected;
  if (op.timeoutMs !== undefined) raw.timeoutMs = op.timeoutMs;
  if (op.text !== undefined) raw.text = op.text;
  if (op.key !== undefined) raw.key = op.key;
  if (op.ctrl !== undefined) raw.ctrl = op.ctrl;
  if (op.shift !== undefined) raw.shift = op.shift;
  if (op.alt !== undefined) raw.alt = op.alt;
  if (op.fromNx !== undefined) raw.fromNx = op.fromNx;
  if (op.toNx !== undefined) raw.toNx = op.toNx;
  if (op.ny !== undefined) raw.ny = op.ny;
  if (op.steps !== undefined) raw.steps = op.steps;
  if (op.ms !== undefined) raw.ms = op.ms;
  return raw;
}

function expectToRaw(expect: Expect, defaults: ScenarioDefaults): Record<string, unknown> {
  const raw: Record<string, unknown> = { target: expect.target, property: expect.property };
  raw[expect.matcher] = expect.matcher === "truthy" ? true : expect.expected;
  if (expect.timeoutMs !== undefined && expect.timeoutMs !== defaults.expectTimeoutMs) {
    raw.timeoutMs = expect.timeoutMs;
  }
  return raw;
}

/**
 * Serializes a scenario to schema-valid YAML text.
 *
 * @throws {ScenarioError} when the serialized form fails validation (the final
 * guard shared with the loader, so editor output can never drift from what the
 * runner accepts).
 */
export function scenarioToYamlText(scenario: Scenario): string {
  const raw = scenarioToRaw(scenario);
  const validation = validateScenario(raw);
  if (!validation.valid) {
    throw new ScenarioError(`Serialized scenario failed validation:\n  - ${validation.errors.join("\n  - ")}`);
  }
  return yaml.dump(raw, { lineWidth: 120, noRefs: true });
}

/** One-call editor save path: graph -> scenario -> validated YAML text. */
export function flowToYamlText(graph: FlowGraphModel): string {
  return scenarioToYamlText(flowToScenario(graph));
}
