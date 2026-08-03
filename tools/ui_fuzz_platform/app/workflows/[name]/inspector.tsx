"use client";

//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

/**
 * Inspector panel for the flow editor: edits the selected operation node,
 * assertion node, or `next` edge weight. Operation fields adapt to the
 * selected action so the form only shows parameters the DSL supports; targets
 * autocomplete from the QML element catalog.
 */

import React from "react";
import { AutoComplete, Button, Card, Checkbox, Input, InputNumber, Select, Typography } from "antd";

import { MATCHERS, type Matcher } from "../../../src/protocol";
import type { Action, Expect, Op } from "../../../src/scenario";
import type {
  EditorEdge,
  EditorNode,
  ExpectNodeData,
  OpNodeData,
} from "./flow-mapper";
import { ACTIONS } from "./flow-canvas";

type OpField =
  | "target"
  | "property"
  | "matcher"
  | "timeoutMs"
  | "text"
  | "key"
  | "modifiers"
  | "drag"
  | "ms";

const ACTION_FIELDS: Record<Action, readonly OpField[]> = {
  click: ["target"],
  rightClick: ["target"],
  doubleClick: ["target"],
  key: ["key", "text", "modifiers"],
  typeText: ["text"],
  drag: ["target", "drag"],
  wait: ["target", "property", "matcher", "timeoutMs"],
  waitMs: ["ms"],
  screenshot: [],
};

export interface EditorInspectorProps {
  readonly selectedNode: EditorNode | undefined;
  readonly selectedEdge: EditorEdge | undefined;
  readonly catalogTargets: readonly string[];
  readonly onUpdateOp: (id: string, updater: (data: OpNodeData) => OpNodeData) => void;
  readonly onUpdateExpect: (id: string, updater: (data: ExpectNodeData) => ExpectNodeData) => void;
  readonly onRenameOp: (oldId: string, newId: string) => void;
  readonly onUpdateNextWeight: (edgeId: string, weight: number) => void;
  readonly onSetStart: (id: string) => void;
}

/** Parses inspector text into a YAML scalar: boolean, number, or string. */
function parseScalar(text: string): unknown {
  const trimmed = text.trim();
  if (trimmed === "true") return true;
  if (trimmed === "false") return false;
  const asNumber = Number(trimmed);
  if (trimmed.length > 0 && !Number.isNaN(asNumber)) return asNumber;
  return trimmed;
}

function FieldRow({ label, children }: { label: string; children: React.ReactNode }): React.ReactElement {
  return (
    <div style={{ display: "flex", alignItems: "center", gap: 8, marginBottom: 8 }}>
      <Typography.Text type="secondary" style={{ width: 88, flexShrink: 0 }}>
        {label}
      </Typography.Text>
      <div style={{ flex: 1 }}>{children}</div>
    </div>
  );
}

export function EditorInspector(props: EditorInspectorProps): React.ReactElement {
  const { selectedNode, selectedEdge } = props;

  return (
    <Card size="small" title="Inspector" style={{ width: 320, flexShrink: 0, overflowY: "auto" }}>
      {selectedNode?.type === "opNode" ? (
        <OpInspector node={selectedNode} {...props} />
      ) : selectedNode?.type === "expectNode" ? (
        <ExpectInspector node={selectedNode} {...props} />
      ) : selectedEdge !== undefined && selectedEdge.type !== "expectEdge" ? (
        <NextEdgeInspector edge={selectedEdge} onUpdateNextWeight={props.onUpdateNextWeight} />
      ) : (
        <Typography.Text type="secondary">
          Select a node or an edge to edit it. Drag from a node's right handle to add a weighted
          next edge; drag from the bottom handle to link an assertion.
        </Typography.Text>
      )}
    </Card>
  );
}

function OpInspector({
  node,
  catalogTargets,
  onUpdateOp,
  onRenameOp,
  onSetStart,
}: EditorInspectorProps & { node: EditorNode }): React.ReactElement {
  const data = node.data as OpNodeData;
  const op = data.op;
  const fields = ACTION_FIELDS[op.action];

  const patchOp = (patch: Partial<Op>): void => {
    onUpdateOp(node.id, (current) => ({ ...current, op: { ...current.op, ...patch } }));
  };

  const targetOptions = catalogTargets.map((value) => ({ value }));

  return (
    <div>
      <FieldRow label="node id">
        <Input
          key={node.id}
          defaultValue={node.id}
          onBlur={(event) => onRenameOp(node.id, event.target.value.trim())}
          onPressEnter={(event) => onRenameOp(node.id, (event.target as HTMLInputElement).value.trim())}
        />
      </FieldRow>
      <FieldRow label="action">
        <Select
          style={{ width: "100%" }}
          value={op.action}
          options={ACTIONS.map((action) => ({ value: action, label: action }))}
          onChange={(action) => onUpdateOp(node.id, (current) => ({ ...current, op: { action } }))}
        />
      </FieldRow>
      {fields.includes("target") ? (
        <FieldRow label="target">
          <AutoComplete
            style={{ width: "100%" }}
            value={op.target ?? ""}
            options={targetOptions}
            filterOption={(input, option) =>
              (option?.value ?? "").toLowerCase().includes(input.toLowerCase())
            }
            onChange={(value) => patchOp({ target: value })}
            placeholder="objectName / testId"
          />
        </FieldRow>
      ) : null}
      {fields.includes("property") ? (
        <FieldRow label="property">
          <Input
            value={op.property ?? ""}
            placeholder="visible"
            onChange={(event) => patchOp({ property: event.target.value })}
          />
        </FieldRow>
      ) : null}
      {fields.includes("matcher") ? (
        <FieldRow label="matcher">
          <Select
            style={{ width: "100%" }}
            value={op.matcher ?? "eq"}
            options={MATCHERS.map((matcher) => ({ value: matcher, label: matcher }))}
            onChange={(matcher: Matcher) =>
              patchOp({ matcher, expected: matcher === "truthy" ? true : op.expected })
            }
          />
        </FieldRow>
      ) : null}
      {fields.includes("matcher") && op.matcher !== "truthy" ? (
        <FieldRow label="expected">
          <Input
            value={op.expected === undefined ? "" : String(op.expected)}
            placeholder="true / 42 / text"
            onChange={(event) => patchOp({ expected: parseScalar(event.target.value) })}
          />
        </FieldRow>
      ) : null}
      {fields.includes("timeoutMs") ? (
        <FieldRow label="timeoutMs">
          <InputNumber
            style={{ width: "100%" }}
            min={0}
            value={op.timeoutMs}
            onChange={(value) => patchOp({ timeoutMs: value ?? undefined })}
          />
        </FieldRow>
      ) : null}
      {fields.includes("key") ? (
        <FieldRow label="key code">
          <InputNumber
            style={{ width: "100%" }}
            min={0}
            value={op.key}
            placeholder="Qt::Key integer"
            onChange={(value) => patchOp({ key: value ?? undefined })}
          />
        </FieldRow>
      ) : null}
      {fields.includes("text") ? (
        <FieldRow label="text">
          <Input value={op.text ?? ""} onChange={(event) => patchOp({ text: event.target.value })} />
        </FieldRow>
      ) : null}
      {fields.includes("modifiers") ? (
        <FieldRow label="modifiers">
          <Checkbox checked={op.ctrl ?? false} onChange={(event) => patchOp({ ctrl: event.target.checked })}>
            Ctrl
          </Checkbox>
          <Checkbox checked={op.shift ?? false} onChange={(event) => patchOp({ shift: event.target.checked })}>
            Shift
          </Checkbox>
          <Checkbox checked={op.alt ?? false} onChange={(event) => patchOp({ alt: event.target.checked })}>
            Alt
          </Checkbox>
        </FieldRow>
      ) : null}
      {fields.includes("drag") ? (
        <>
          <FieldRow label="fromNx">
            <InputNumber
              style={{ width: "100%" }}
              step={0.05}
              min={0}
              max={1}
              value={op.fromNx}
              onChange={(value) => patchOp({ fromNx: value ?? undefined })}
            />
          </FieldRow>
          <FieldRow label="toNx">
            <InputNumber
              style={{ width: "100%" }}
              step={0.05}
              min={0}
              max={1}
              value={op.toNx}
              onChange={(value) => patchOp({ toNx: value ?? undefined })}
            />
          </FieldRow>
          <FieldRow label="ny">
            <InputNumber
              style={{ width: "100%" }}
              step={0.05}
              min={0}
              max={1}
              value={op.ny}
              onChange={(value) => patchOp({ ny: value ?? undefined })}
            />
          </FieldRow>
          <FieldRow label="steps">
            <InputNumber
              style={{ width: "100%" }}
              min={1}
              value={op.steps}
              onChange={(value) => patchOp({ steps: value ?? undefined })}
            />
          </FieldRow>
        </>
      ) : null}
      {fields.includes("ms") ? (
        <FieldRow label="ms">
          <InputNumber
            style={{ width: "100%" }}
            min={0}
            value={op.ms}
            onChange={(value) => patchOp({ ms: value ?? undefined })}
          />
        </FieldRow>
      ) : null}
      <Button size="small" disabled={data.isStart} onClick={() => onSetStart(node.id)}>
        {data.isStart ? "Start node" : "Set as start"}
      </Button>
    </div>
  );
}

function ExpectInspector({
  node,
  catalogTargets,
  onUpdateExpect,
}: EditorInspectorProps & { node: EditorNode }): React.ReactElement {
  const data = node.data as ExpectNodeData;
  const expect = data.expect;

  const patchExpect = (patch: Partial<Expect>): void => {
    onUpdateExpect(node.id, (current) => ({
      ...current,
      expect: { ...current.expect, ...patch },
    }));
  };

  return (
    <div>
      <FieldRow label="owner">
        <Typography.Text code>{data.owner}</Typography.Text>
      </FieldRow>
      <FieldRow label="target">
        <AutoComplete
          style={{ width: "100%" }}
          value={expect.target}
          options={catalogTargets.map((value) => ({ value }))}
          filterOption={(input, option) =>
            (option?.value ?? "").toLowerCase().includes(input.toLowerCase())
          }
          onChange={(value) => patchExpect({ target: value })}
          placeholder="objectName / testId"
        />
      </FieldRow>
      <FieldRow label="property">
        <Input
          value={expect.property}
          onChange={(event) => patchExpect({ property: event.target.value })}
        />
      </FieldRow>
      <FieldRow label="matcher">
        <Select
          style={{ width: "100%" }}
          value={expect.matcher}
          options={MATCHERS.map((matcher) => ({ value: matcher, label: matcher }))}
          onChange={(matcher: Matcher) =>
            patchExpect({ matcher, expected: matcher === "truthy" ? true : expect.expected })
          }
        />
      </FieldRow>
      {expect.matcher !== "truthy" ? (
        <FieldRow label="expected">
          <Input
            value={String(expect.expected)}
            onChange={(event) => patchExpect({ expected: parseScalar(event.target.value) })}
          />
        </FieldRow>
      ) : null}
      <FieldRow label="timeoutMs">
        <InputNumber
          style={{ width: "100%" }}
          min={0}
          value={expect.timeoutMs}
          placeholder="scenario default"
          onChange={(value) => patchExpect({ timeoutMs: value ?? undefined })}
        />
      </FieldRow>
    </div>
  );
}

function NextEdgeInspector({
  edge,
  onUpdateNextWeight,
}: {
  edge: EditorEdge;
  onUpdateNextWeight: (edgeId: string, weight: number) => void;
}): React.ReactElement {
  return (
    <div>
      <FieldRow label="edge">
        <Typography.Text code>
          {edge.source} → {edge.target}
        </Typography.Text>
      </FieldRow>
      <FieldRow label="weight">
        <InputNumber
          style={{ width: "100%" }}
          min={0}
          value={edge.data?.weight ?? 1}
          onChange={(value) => onUpdateNextWeight(edge.id, value ?? 1)}
        />
      </FieldRow>
    </div>
  );
}
