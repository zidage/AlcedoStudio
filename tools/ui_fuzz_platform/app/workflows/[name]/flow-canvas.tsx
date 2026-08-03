"use client";

//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

/**
 * React Flow canvas for the scenario editor. Operation nodes expose two
 * outlet kinds: a right-side `next` handle (weighted edges to successor
 * operations) and a bottom `expect` handle (edges to assertion nodes). The
 * canvas owns all graph state; the page retrieves serialized YAML through the
 * imperative `serialize` handle, which always runs `flowToYamlText`.
 */

import React from "react";
import {
  Background,
  BezierEdge,
  Controls,
  Handle,
  Position,
  ReactFlow,
  applyEdgeChanges,
  applyNodeChanges,
  useEdgesState,
  useNodesState,
  type Connection,
  type EdgeChange,
  type EdgeProps,
  type NodeChange,
  type NodeProps,
  type ReactFlowInstance,
} from "@xyflow/react";
import "@xyflow/react/dist/style.css";
import { App, Button, Dropdown, Input, InputNumber, Select, Space, Tag, Tooltip } from "antd";
import { AimOutlined, PlusOutlined } from "@ant-design/icons";

import { expectNodeId, flowToYamlText } from "../../../src/flow-graph";
import type { Action, Op } from "../../../src/scenario";
import { EditorInspector } from "./inspector";
import {
  freshOpNodeId,
  nextExpectIndex,
  rfToModel,
  type EditorEdge,
  type EditorNode,
  type ExpectNodeData,
  type NextEdgeData,
  type OpNodeData,
  type ScenarioMeta,
} from "./flow-mapper";

export const ACTIONS: readonly Action[] = [
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

export interface FlowCanvasHandle {
  /** Serializes the current canvas to schema-valid YAML; throws FlowGraphError. */
  serialize: () => string;
}

interface FlowCanvasProps {
  readonly initialNodes: EditorNode[];
  readonly initialEdges: EditorEdge[];
  readonly initialMeta: ScenarioMeta;
  readonly catalogTargets: readonly string[];
  /** Called on mount and after every graph change with the live handle. */
  readonly registerHandle: (handle: FlowCanvasHandle) => void;
}

const opNodeStyle: React.CSSProperties = {
  minWidth: 180,
  maxWidth: 260,
  background: "#ffffff",
  border: "1px solid #d9d9d9",
  borderRadius: 6,
  padding: "8px 10px",
  fontSize: 12,
  boxShadow: "0 1px 2px rgba(0,0,0,0.06)",
};

const expectNodeStyle: React.CSSProperties = {
  ...opNodeStyle,
  minWidth: 160,
  background: "#fafafa",
  borderStyle: "dashed",
};

function OpNode({ id, data, selected }: NodeProps): React.ReactElement {
  const nodeData = data as OpNodeData;
  return (
    <div style={{ ...opNodeStyle, borderColor: selected ? "#1677ff" : "#d9d9d9" }}>
      <Handle type="target" position={Position.Left} id="in" />
      <div style={{ display: "flex", alignItems: "center", gap: 6, marginBottom: 4 }}>
        {nodeData.isStart ? <Tag color="success">start</Tag> : null}
        <strong style={{ overflow: "hidden", textOverflow: "ellipsis" }}>{id}</strong>
      </div>
      <div>
        <Tag color="blue">{nodeData.op.action}</Tag>
        {nodeData.op.target !== undefined ? <code>{nodeData.op.target}</code> : null}
      </div>
      {nodeData.op.property !== undefined ? (
        <div style={{ color: "#8c8c8c", marginTop: 2 }}>
          {nodeData.op.property} {nodeData.op.matcher ?? ""} {String(nodeData.op.expected ?? "")}
        </div>
      ) : null}
      <Tooltip title="next (weighted successor)">
        <Handle type="source" position={Position.Right} id="next" />
      </Tooltip>
      <Tooltip title="expect (assertion outlet)">
        <Handle type="source" position={Position.Bottom} id="expect" style={{ background: "#fa8c16" }} />
      </Tooltip>
    </div>
  );
}

function ExpectNode({ data, selected }: NodeProps): React.ReactElement {
  const nodeData = data as ExpectNodeData;
  return (
    <div style={{ ...expectNodeStyle, borderColor: selected ? "#1677ff" : "#d9d9d9" }}>
      <Handle type="target" position={Position.Left} id="in" />
      <div style={{ marginBottom: 4 }}>
        <Tag color="orange">expect</Tag>
      </div>
      <div>
        <code>{nodeData.expect.target}</code>.{nodeData.expect.property}
      </div>
      <div style={{ color: "#8c8c8c" }}>
        {nodeData.expect.matcher} {String(nodeData.expect.expected)}
        {nodeData.expect.timeoutMs !== undefined ? ` · ${nodeData.expect.timeoutMs}ms` : ""}
      </div>
    </div>
  );
}

function ExpectEdge(props: EdgeProps): React.ReactElement {
  return <BezierEdge {...props} style={{ stroke: "#fa8c16", strokeDasharray: "6 3" }} />;
}

const nodeTypes = { opNode: OpNode, expectNode: ExpectNode };
const edgeTypes = { nextEdge: BezierEdge, expectEdge: ExpectEdge };

export function FlowCanvas({
  initialNodes,
  initialEdges,
  initialMeta,
  catalogTargets,
  registerHandle,
}: FlowCanvasProps): React.ReactElement {
  const { message } = App.useApp();
  const [nodes, setNodes] = useNodesState<EditorNode>(initialNodes);
  const [edges, setEdges] = useEdgesState<EditorEdge>(initialEdges);
  const [meta, setMeta] = React.useState<ScenarioMeta>(initialMeta);
  const [selectedId, setSelectedId] = React.useState<string | null>(null);
  const flowInstance = React.useRef<ReactFlowInstance<EditorNode, EditorEdge> | null>(null);

  React.useEffect(() => {
    registerHandle({ serialize: () => flowToYamlText(rfToModel(nodes, edges, meta)) });
  }, [nodes, edges, meta, registerHandle]);

  const selectedNode = nodes.find((node) => node.id === selectedId && node.selected);
  const selectedEdge = edges.find((edge) => edge.id === selectedId && edge.selected);

  const onNodesChange = React.useCallback(
    (changes: NodeChange<EditorNode>[]) => {
      const removedOpIds = changes
        .filter((change) => change.type === "remove")
        .map((change) => change.id)
        .filter((id) => nodes.some((node) => node.id === id && node.type === "opNode"));

      let effective = changes;
      if (removedOpIds.length > 0) {
        // Cascade: assertion nodes owned by a removed operation go with it,
        // plus every edge touching any removed node (React Flow does not
        // cascade in controlled mode).
        const removedExpectIds = nodes
          .filter(
            (node) =>
              node.type === "expectNode" &&
              removedOpIds.includes((node.data as ExpectNodeData).owner),
          )
          .map((node) => node.id);
        const allRemoved = new Set([...removedOpIds, ...removedExpectIds]);
        setEdges((current) =>
          current.filter((edge) => !allRemoved.has(edge.source) && !allRemoved.has(edge.target)),
        );
        effective = [
          ...changes,
          ...removedExpectIds.map((id) => ({ type: "remove", id }) as NodeChange<EditorNode>),
        ];
        if (removedOpIds.includes(meta.startNodeId)) {
          const fallback = nodes.find(
            (node) => node.type === "opNode" && !allRemoved.has(node.id),
          );
          setMeta((current) => ({ ...current, startNodeId: fallback?.id ?? "" }));
        }
      }
      setNodes((current) => applyNodeChanges(effective, current));
    },
    [nodes, edges, meta.startNodeId, setNodes, setEdges],
  );

  const onEdgesChange = React.useCallback(
    (changes: EdgeChange<EditorEdge>[]) => {
      setEdges((current) => applyEdgeChanges(changes, current));
    },
    [setEdges],
  );

  const onConnect = React.useCallback(
    (connection: Connection) => {
      const source = nodes.find((node) => node.id === connection.source);
      const target = nodes.find((node) => node.id === connection.target);
      if (source === undefined || target === undefined) return;

      if (connection.sourceHandle === "next" && source.type === "opNode" && target.type === "opNode") {
        const siblingCount = edges.filter(
          (edge) => edge.type !== "expectEdge" && edge.source === source.id,
        ).length;
        setEdges((current) => [
          ...current,
          {
            id: `${source.id}->${target.id}#${siblingCount}`,
            type: "nextEdge",
            source: source.id,
            target: target.id,
            sourceHandle: "next",
            targetHandle: "in",
            data: { weight: 1 } satisfies NextEdgeData,
            label: "w=1",
          },
        ]);
        return;
      }

      if (
        connection.sourceHandle === "expect" &&
        source.type === "opNode" &&
        target.type === "expectNode" &&
        (target.data as ExpectNodeData).owner === source.id
      ) {
        setEdges((current) => [
          ...current,
          {
            id: `${source.id}=>${target.id}`,
            type: "expectEdge",
            source: source.id,
            target: target.id,
            sourceHandle: "expect",
            targetHandle: "in",
          },
        ]);
        return;
      }

      void message.warning(
        "Connect next outlets to operation nodes, and expect outlets to that operation's own assertion nodes.",
      );
    },
    [nodes, edges, setEdges, message],
  );

  const onSelectionChange = React.useCallback(
    ({ nodes: selectedNodes, edges: selectedEdges }: { nodes: EditorNode[]; edges: EditorEdge[] }) => {
      setSelectedId(selectedNodes[0]?.id ?? selectedEdges[0]?.id ?? null);
    },
    [],
  );

  const updateOpNode = React.useCallback(
    (id: string, updater: (data: OpNodeData) => OpNodeData) => {
      setNodes((current) =>
        current.map((node) =>
          node.id === id && node.type === "opNode"
            ? { ...node, data: updater(node.data as OpNodeData) }
            : node,
        ),
      );
    },
    [setNodes],
  );

  const updateExpectNode = React.useCallback(
    (id: string, updater: (data: ExpectNodeData) => ExpectNodeData) => {
      setNodes((current) =>
        current.map((node) =>
          node.id === id && node.type === "expectNode"
            ? { ...node, data: updater(node.data as ExpectNodeData) }
            : node,
        ),
      );
    },
    [setNodes],
  );

  const updateNextEdge = React.useCallback(
    (id: string, weight: number) => {
      setEdges((current) =>
        current.map((edge) =>
          edge.id === id
            ? { ...edge, data: { ...edge.data, weight } as NextEdgeData, label: `w=${weight}` }
            : edge,
        ),
      );
    },
    [setEdges],
  );

  const renameOpNode = React.useCallback(
    (oldId: string, newId: string) => {
      if (newId === oldId) return;
      if (nodes.some((node) => node.id === newId)) {
        void message.error(`Node id '${newId}' already exists.`);
        return;
      }
      setNodes((current) =>
        current.map((node) => {
          if (node.id === oldId) return { ...node, id: newId };
          if (node.type === "expectNode" && (node.data as ExpectNodeData).owner === oldId) {
            const data = node.data as ExpectNodeData;
            return {
              ...node,
              id: expectNodeId(newId, data.expectIndex ?? 0),
              data: { ...data, owner: newId },
            };
          }
          return node;
        }),
      );
      setEdges((current) =>
        current.map((edge) => ({
          ...edge,
          source: edge.source === oldId ? newId : edge.source,
          target: edge.target === oldId ? newId : edge.target,
        })),
      );
      setMeta((current) => ({
        ...current,
        startNodeId: current.startNodeId === oldId ? newId : current.startNodeId,
      }));
      setSelectedId((current) => (current === oldId ? newId : current));
    },
    [nodes, setNodes, setEdges, message],
  );

  const addOperation = React.useCallback(
    (action: Action) => {
      const id = freshOpNodeId(nodes);
      const op: Op = { action };
      setNodes((current) => [
        ...current,
        {
          id,
          type: "opNode" as const,
          position: { x: 80 + current.length * 40, y: 80 + current.length * 40 },
          data: { op, isStart: !current.some((node) => node.type === "opNode") } satisfies OpNodeData,
        },
      ]);
      setMeta((current) =>
        current.startNodeId.length === 0 ? { ...current, startNodeId: id } : current,
      );
    },
    [nodes, setNodes],
  );

  const addExpect = React.useCallback(() => {
    const owner = nodes.find((node) => node.id === selectedId && node.type === "opNode");
    if (owner === undefined) {
      void message.warning("Select an operation node first, then add an assertion.");
      return;
    }
    const index = nextExpectIndex(nodes, owner.id);
    const id = expectNodeId(owner.id, index);
    setNodes((current) => [
      ...current,
      {
        id,
        type: "expectNode" as const,
        position: { x: owner.position.x + 300, y: owner.position.y + (index + 1) * 110 },
        data: {
          owner: owner.id,
          expect: { target: "", property: "visible", matcher: "eq", expected: true },
        } satisfies ExpectNodeData,
      },
    ]);
    setEdges((current) => [
      ...current,
      {
        id: `${owner.id}=>${id}`,
        type: "expectEdge" as const,
        source: owner.id,
        target: id,
        sourceHandle: "expect",
        targetHandle: "in",
      },
    ]);
  }, [nodes, selectedId, setNodes, setEdges, message]);

  const opNodeIds = nodes.filter((node) => node.type === "opNode").map((node) => node.id);

  return (
    <div style={{ display: "flex", gap: 12, height: "calc(100vh - 220px)", minHeight: 480 }}>
      <div style={{ flex: 1, display: "flex", flexDirection: "column", gap: 8 }}>
        <Space wrap size={8}>
          <Input
            addonBefore="name"
            style={{ width: 300 }}
            value={meta.name}
            onChange={(event) => setMeta((current) => ({ ...current, name: event.target.value }))}
          />
          <Select
            style={{ width: 220 }}
            value={meta.startNodeId || undefined}
            placeholder="start node"
            onChange={(value) => setMeta((current) => ({ ...current, startNodeId: value }))}
            options={opNodeIds.map((id) => ({ value: id, label: id }))}
          />
          <InputNumber
            addonBefore="expect timeout"
            min={0}
            style={{ width: 220 }}
            value={meta.expectTimeoutMs}
            onChange={(value) =>
              setMeta((current) => ({ ...current, expectTimeoutMs: value ?? undefined }))
            }
          />
          <Dropdown
            menu={{
              items: ACTIONS.map((action) => ({ key: action, label: action })),
              onClick: ({ key }) => addOperation(key as Action),
            }}
          >
            <Button type="primary" icon={<PlusOutlined />}>
              Add operation
            </Button>
          </Dropdown>
          <Button icon={<AimOutlined />} onClick={addExpect}>
            Add expect
          </Button>
        </Space>
        <div style={{ flex: 1, border: "1px solid #f0f0f0", borderRadius: 8 }}>
          <ReactFlow
            nodes={nodes}
            edges={edges}
            nodeTypes={nodeTypes}
            edgeTypes={edgeTypes}
            onNodesChange={onNodesChange}
            onEdgesChange={onEdgesChange}
            onConnect={onConnect}
            onSelectionChange={onSelectionChange}
            onInit={(instance) => {
              flowInstance.current = instance;
              instance.fitView({ padding: 0.2 });
            }}
            deleteKeyCode={["Backspace", "Delete"]}
            fitView
            proOptions={{ hideAttribution: true }}
          >
            <Background gap={16} />
            <Controls />
          </ReactFlow>
        </div>
      </div>
      <EditorInspector
        selectedNode={selectedNode}
        selectedEdge={selectedEdge}
        catalogTargets={catalogTargets}
        onUpdateOp={updateOpNode}
        onUpdateExpect={updateExpectNode}
        onRenameOp={renameOpNode}
        onUpdateNextWeight={updateNextEdge}
        onSetStart={(id) =>
          setMeta((current) => ({ ...current, startNodeId: id }))
        }
      />
    </div>
  );
}
