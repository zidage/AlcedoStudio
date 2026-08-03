"use client";

//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

/**
 * Flow editor page (`/workflows/[name]`). Loads any valid scenario YAML onto
 * the React Flow canvas; saving serializes the canvas through the shared
 * flow-graph module and writes schema-valid YAML back to the workflow store,
 * so the runner executes the file with no manual edits. With `?create=1` the
 * page opens a fresh scaffold instead of loading an existing file.
 */

import React from "react";
import Link from "next/link";
import dynamic from "next/dynamic";
import { useRouter } from "next/navigation";
import { PageContainer } from "@ant-design/pro-components";
import { useQuery } from "@tanstack/react-query";
import { Alert, App, Button, Space, Spin, Typography } from "antd";
import { ArrowLeftOutlined, SaveOutlined } from "@ant-design/icons";

import { scenarioToFlow } from "../../../src/flow-graph";
import { parseScenario } from "../../../src/scenario-parse";
import { fetchCatalog, fetchWorkflow, saveWorkflow } from "../../lib/api";
import { modelToRf, type ScenarioMeta } from "./flow-mapper";
import type { FlowCanvasHandle } from "./flow-canvas";

const FlowCanvas = dynamic(
  () => import("./flow-canvas").then((module) => module.FlowCanvas),
  { ssr: false },
);

function scaffoldMeta(name: string): { meta: ScenarioMeta; yaml: string } {
  const yaml = [
    `name: ${name}`,
    "start: first_step",
    "defaults:",
    "  expectTimeoutMs: 8000",
    "nodes:",
    "  first_step:",
    "    op: { action: waitMs, ms: 500 }",
    "    next: []",
  ].join("\n");
  return { meta: { name, startNodeId: "first_step", expectTimeoutMs: 8000 }, yaml };
}

export default function WorkflowEditorPage({
  params,
  searchParams,
}: {
  params: Promise<{ name: string }>;
  searchParams: Promise<{ create?: string }>;
}): React.ReactElement {
  const { name } = React.use(params);
  const { create } = React.use(searchParams);
  const router = useRouter();
  const { message } = App.useApp();
  const handleRef = React.useRef<FlowCanvasHandle | null>(null);
  const [saving, setSaving] = React.useState(false);
  const [saveError, setSaveError] = React.useState<string | null>(null);

  const registerHandle = React.useCallback((handle: FlowCanvasHandle) => {
    handleRef.current = handle;
  }, []);

  const isCreate = create === "1";

  const workflowQuery = useQuery({
    queryKey: ["workflows", "document", name],
    queryFn: async () => {
      if (isCreate) {
        return scaffoldMeta(name).yaml;
      }
      const document = await fetchWorkflow(name);
      return document.yaml;
    },
    staleTime: Infinity,
    retry: false,
  });

  const catalogQuery = useQuery({
    queryKey: ["catalog"],
    queryFn: fetchCatalog,
    retry: false,
  });

  const catalogTargets = React.useMemo(() => {
    const names = new Set<string>();
    for (const entry of catalogQuery.data?.entries ?? []) {
      if (entry.objectName !== null) names.add(entry.objectName);
      for (const candidate of entry.candidates) names.add(candidate);
    }
    return [...names].sort();
  }, [catalogQuery.data]);

  const graph = React.useMemo(() => {
    if (workflowQuery.data === undefined) return undefined;
    try {
      const scenario = parseScenario(workflowQuery.data);
      const model = scenarioToFlow(scenario);
      const { nodes, edges } = modelToRf(model);
      const meta: ScenarioMeta = {
        name: scenario.name,
        startNodeId: scenario.start,
        expectTimeoutMs: scenario.defaults.expectTimeoutMs,
      };
      return { nodes, edges, meta };
    } catch (error) {
      return { error: (error as Error).message };
    }
  }, [workflowQuery.data]);

  const save = async (): Promise<void> => {
    if (handleRef.current === null) return;
    setSaving(true);
    setSaveError(null);
    try {
      const yaml = handleRef.current.serialize();
      const { path } = await saveWorkflow(name, yaml);
      void message.success(`Saved to ${path}`);
      if (isCreate) {
        router.replace(`/workflows/${encodeURIComponent(name)}`);
      }
    } catch (error) {
      setSaveError((error as Error).message);
    } finally {
      setSaving(false);
    }
  };

  const parseError =
    workflowQuery.error !== null
      ? (workflowQuery.error as Error).message
      : graph !== undefined && "error" in graph
        ? graph.error
        : null;

  return (
    <PageContainer
      title={`Workflow: ${name}`}
      subTitle="Two-outlet operation nodes: right = weighted next, bottom = expect assertions"
      extra={
        <Space>
          <Link href="/workflows">
            <Button icon={<ArrowLeftOutlined />}>All workflows</Button>
          </Link>
          <Button
            type="primary"
            icon={<SaveOutlined />}
            loading={saving}
            disabled={graph === undefined || "error" in (graph ?? {})}
            onClick={() => void save()}
          >
            Save
          </Button>
        </Space>
      }
    >
      {parseError !== null ? (
        <Alert
          type="error"
          showIcon
          message={isCreate ? "Failed to build the scaffold" : "This workflow file does not parse"}
          description={<Typography.Text style={{ whiteSpace: "pre-wrap" }}>{parseError}</Typography.Text>}
          style={{ marginBottom: 16 }}
        />
      ) : null}
      {saveError !== null ? (
        <Alert
          type="error"
          showIcon
          message="Save failed"
          description={<Typography.Text style={{ whiteSpace: "pre-wrap" }}>{saveError}</Typography.Text>}
          closable
          onClose={() => setSaveError(null)}
          style={{ marginBottom: 16 }}
        />
      ) : null}
      {graph === undefined ? (
        <Spin style={{ display: "block", margin: "120px auto" }} />
      ) : "error" in graph ? null : (
        <FlowCanvas
          key={name + String(isCreate)}
          initialNodes={graph.nodes}
          initialEdges={graph.edges}
          initialMeta={graph.meta}
          catalogTargets={catalogTargets}
          registerHandle={registerHandle}
        />
      )}
    </PageContainer>
  );
}
