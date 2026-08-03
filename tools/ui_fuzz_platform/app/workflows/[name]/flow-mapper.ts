//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

/**
 * Mapping between the renderer-agnostic {@link FlowGraphModel} and React Flow
 * nodes/edges. The editor keeps React Flow state as its source of truth and
 * converts back to the model only on save, so the serialized YAML always goes
 * through `flowToYamlText` — the same path the round-trip tests exercise.
 */

import type { Edge, Node } from "@xyflow/react";

import type {
  ExpectEdgeModel,
  FlowGraphModel,
  FlowNodeModel,
  NextEdgeModel,
} from "../../../src/flow-graph";
import type { Expect, Op, ScenarioDefaults } from "../../../src/scenario";

export interface OpNodeData extends Record<string, unknown> {
  op: Op;
  isStart: boolean;
}

export interface ExpectNodeData extends Record<string, unknown> {
  owner: string;
  expect: Expect;
  expectIndex?: number;
}

export interface NextEdgeData extends Record<string, unknown> {
  weight: number;
  nextIndex?: number;
}

export type EditorNode = Node<OpNodeData | ExpectNodeData, "opNode" | "expectNode">;
export type EditorEdge = Edge<NextEdgeData>;

export interface ScenarioMeta {
  name: string;
  startNodeId: string;
  expectTimeoutMs?: number;
}

export function modelToRf(model: FlowGraphModel): { nodes: EditorNode[]; edges: EditorEdge[] } {
  const nodes: EditorNode[] = [];
  const edges: EditorEdge[] = [];

  for (const node of model.nodes) {
    if (node.kind === "op") {
      nodes.push({
        id: node.id,
        type: "opNode",
        position: node.position,
        data: { op: node.op, isStart: node.id === model.startNodeId },
      });
    } else {
      nodes.push({
        id: node.id,
        type: "expectNode",
        position: node.position,
        data: { owner: node.owner, expect: node.expect, expectIndex: node.expectIndex },
      });
    }
  }

  for (const edge of model.edges) {
    if (edge.kind === "next") {
      edges.push({
        id: edge.id,
        type: "nextEdge",
        source: edge.from,
        target: edge.to,
        sourceHandle: "next",
        targetHandle: "in",
        data: { weight: edge.weight, nextIndex: edge.nextIndex },
        label: `w=${edge.weight}`,
      });
    } else {
      edges.push({
        id: edge.id,
        type: "expectEdge",
        source: edge.from,
        target: edge.to,
        sourceHandle: "expect",
        targetHandle: "in",
      });
    }
  }

  return { nodes, edges };
}

export function rfToModel(
  nodes: EditorNode[],
  edges: EditorEdge[],
  meta: ScenarioMeta,
): FlowGraphModel {
  const modelNodes: FlowNodeModel[] = [];
  for (const node of nodes) {
    if (node.type === "opNode") {
      const data = node.data as OpNodeData;
      modelNodes.push({ kind: "op", id: node.id, position: node.position, op: data.op });
    } else {
      const data = node.data as ExpectNodeData;
      modelNodes.push({
        kind: "expect",
        id: node.id,
        owner: data.owner,
        expectIndex: data.expectIndex,
        expect: data.expect,
        position: node.position,
      });
    }
  }

  const modelEdges: (NextEdgeModel | ExpectEdgeModel)[] = [];
  for (const edge of edges) {
    if (edge.type === "expectEdge") {
      modelEdges.push({ kind: "expect", id: edge.id, from: edge.source, to: edge.target });
    } else {
      modelEdges.push({
        kind: "next",
        id: edge.id,
        from: edge.source,
        to: edge.target,
        weight: edge.data?.weight ?? 1,
        nextIndex: edge.data?.nextIndex,
      });
    }
  }

  const defaults: ScenarioDefaults = {};
  if (meta.expectTimeoutMs !== undefined) {
    defaults.expectTimeoutMs = meta.expectTimeoutMs;
  }

  return {
    name: meta.name,
    startNodeId: meta.startNodeId,
    defaults,
    nodes: modelNodes,
    edges: modelEdges,
  };
}

/** Finds the next free assertion index for a new expect node under `ownerId`. */
export function nextExpectIndex(nodes: EditorNode[], ownerId: string): number {
  let index = 0;
  const used = new Set(
    nodes
      .filter((node) => node.type === "expectNode" && (node.data as ExpectNodeData).owner === ownerId)
      .map((node) => (node.data as ExpectNodeData).expectIndex ?? -1),
  );
  while (used.has(index)) index++;
  return index;
}

/** Generates a fresh operation node id that does not collide with existing ones. */
export function freshOpNodeId(nodes: EditorNode[]): string {
  const used = new Set(nodes.map((node) => node.id));
  let index = nodes.filter((node) => node.type === "opNode").length + 1;
  let candidate = `node_${index}`;
  while (used.has(candidate)) {
    index++;
    candidate = `node_${index}`;
  }
  return candidate;
}
