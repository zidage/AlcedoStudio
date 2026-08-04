"use client";

//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

/**
 * Active-run dashboard built with Ant Design Pro v6 components (antd 6 +
 * @ant-design/pro-components v3 + React Query). No custom visual chrome — layout,
 * forms, cards, and statistics come from ProComponents / antd.
 */

import React, { useCallback, useEffect, useMemo, useRef, useState } from "react";
import type { ProFormInstance } from "@ant-design/pro-components";
import {
  PageContainer,
  ProCard,
  ProForm,
  ProFormDigit,
  ProFormItem,
  ProFormSelect,
  ProFormSwitch,
} from "@ant-design/pro-components";
import { useMutation, useQuery, useQueryClient } from "@tanstack/react-query";
import {
  Badge,
  Button,
  Col,
  Empty,
  List,
  Row,
  Space,
  Statistic,
  Tag,
  Typography,
  App as AntdApp,
} from "antd";
import {
  CheckCircleOutlined,
  CloseCircleOutlined,
  HeartOutlined,
  PauseCircleOutlined,
  PlayCircleOutlined,
  UnorderedListOutlined,
} from "@ant-design/icons";
import Link from "next/link";

import { PathBrowseInput } from "../../components/PathBrowseInput";
import {
  fetchActiveRun,
  fetchWorkflows,
  runsWebSocketUrl,
  startRun,
  stopRun,
} from "../../lib/api";
import type { ActiveRunSnapshot, DashboardWsEvent, LogLine, StartRunFormValues } from "../../lib/types";

const MAX_LOG_LINES = 500;
const RUN_PATHS_STORAGE_KEY = "ui-fuzz-dashboard-run-paths";

const LOG_FONT =
  "ui-monospace, SFMono-Regular, Menlo, Monaco, Consolas, 'Liberation Mono', 'Courier New', monospace";

interface StoredRunPaths {
  hostPath?: string;
  projectPath?: string;
  importDir?: string;
  scenarioPath?: string;
}

function loadStoredRunPaths(): StoredRunPaths {
  if (typeof window === "undefined") {
    return {};
  }
  try {
    const raw = window.localStorage.getItem(RUN_PATHS_STORAGE_KEY);
    if (raw === null || raw.length === 0) {
      return {};
    }
    const parsed = JSON.parse(raw) as StoredRunPaths;
    return {
      hostPath: typeof parsed.hostPath === "string" ? parsed.hostPath : undefined,
      projectPath: typeof parsed.projectPath === "string" ? parsed.projectPath : undefined,
      importDir: typeof parsed.importDir === "string" ? parsed.importDir : undefined,
      scenarioPath: typeof parsed.scenarioPath === "string" ? parsed.scenarioPath : undefined,
    };
  } catch {
    return {};
  }
}

function saveStoredRunPaths(paths: StoredRunPaths): void {
  if (typeof window === "undefined") {
    return;
  }
  window.localStorage.setItem(RUN_PATHS_STORAGE_KEY, JSON.stringify(paths));
}

function statusColor(status: ActiveRunSnapshot["status"]): string {
  switch (status) {
    case "running":
      return "processing";
    case "starting":
    case "stopping":
      return "warning";
    case "finished":
      return "default";
    default:
      return "default";
  }
}

function verdictColor(verdict: string | null | undefined): string {
  switch (verdict) {
    case "pass":
      return "success";
    case "correctness":
      return "warning";
    case "deadlock":
    case "crash":
      return "error";
    default:
      return "default";
  }
}

function verdictValueColor(verdict: string | null | undefined): string | undefined {
  switch (verdict) {
    case "pass":
      return "#389e0d";
    case "correctness":
      return "#d48806";
    case "deadlock":
    case "crash":
      return "#cf1322";
    default:
      return undefined;
  }
}

function formatElapsed(ms: number): string {
  const totalSeconds = Math.floor(ms / 1000);
  const minutes = Math.floor(totalSeconds / 60);
  const seconds = totalSeconds % 60;
  return `${minutes}:${String(seconds).padStart(2, "0")}`;
}

export default function ActiveRunPage(): React.ReactElement {
  const { message } = AntdApp.useApp();
  const queryClient = useQueryClient();
  const formRef = useRef<ProFormInstance<StartRunFormValues> | undefined>(undefined);
  const [snapshot, setSnapshot] = useState<ActiveRunSnapshot | null>(null);
  const [logs, setLogs] = useState<LogLine[]>([]);
  const [storedPaths] = useState<StoredRunPaths>(() => loadStoredRunPaths());
  const logSeq = useRef(0);
  const logScrollRef = useRef<HTMLDivElement | null>(null);

  const activeQuery = useQuery({
    queryKey: ["runs", "active"],
    queryFn: fetchActiveRun,
    refetchInterval: (query) => {
      const status = query.state.data?.status;
      return status === "running" || status === "starting" || status === "stopping" ? 2000 : false;
    },
  });

  const workflowsQuery = useQuery({
    queryKey: ["workflows", "list"],
    queryFn: fetchWorkflows,
  });

  useEffect(() => {
    if (activeQuery.data !== undefined) {
      setSnapshot(activeQuery.data);
    }
  }, [activeQuery.data]);

  const appendLog = useCallback((line: string, stream: "stdout" | "stderr", at: number) => {
    logSeq.current += 1;
    const entry: LogLine = { key: `${logSeq.current}`, line, stream, at };
    setLogs((prev) => {
      const next = [...prev, entry];
      return next.length > MAX_LOG_LINES ? next.slice(next.length - MAX_LOG_LINES) : next;
    });
  }, []);

  useEffect(() => {
    const el = logScrollRef.current;
    if (el === null) {
      return;
    }
    el.scrollTop = el.scrollHeight;
  }, [logs]);

  useEffect(() => {
    const socket = new WebSocket(runsWebSocketUrl());
    socket.onmessage = (event) => {
      let payload: DashboardWsEvent;
      try {
        payload = JSON.parse(String(event.data)) as DashboardWsEvent;
      } catch {
        return;
      }
      if (payload.type === "status" || payload.type === "finished") {
        setSnapshot(payload.snapshot);
        void queryClient.setQueryData(["runs", "active"], payload.snapshot);
      } else if (payload.type === "log") {
        appendLog(payload.line, payload.stream, payload.at);
      }
    };
    return () => socket.close();
  }, [appendLog, queryClient]);

  const startMutation = useMutation({
    mutationFn: startRun,
    onSuccess: (_data, values) => {
      message.success("Run started");
      setLogs([]);
      saveStoredRunPaths({
        hostPath: values.hostPath,
        projectPath: values.projectPath ?? "",
        importDir: values.importDir ?? "",
        scenarioPath: values.scenarioPath,
      });
      void queryClient.invalidateQueries({ queryKey: ["runs", "active"] });
    },
    onError: (error: Error) => message.error(error.message),
  });

  const stopMutation = useMutation({
    mutationFn: stopRun,
    onSuccess: (next) => {
      message.success("Run stopped — host process tree torn down");
      setSnapshot(next);
      void queryClient.setQueryData(["runs", "active"], next);
    },
    onError: (error: Error) => message.error(error.message),
  });

  const current = snapshot ?? activeQuery.data;
  const busy =
    current?.status === "starting" ||
    current?.status === "running" ||
    current?.status === "stopping";

  const workflowOptions = useMemo(() => {
    const workflows = workflowsQuery.data ?? [];
    return workflows.map((workflow) => {
      const label =
        workflow.scenarioName !== null && workflow.scenarioName.length > 0
          ? `${workflow.name} — ${workflow.scenarioName}`
          : workflow.name;
      return {
        value: workflow.path,
        label,
        disabled: workflow.errors !== null,
      };
    });
  }, [workflowsQuery.data]);

  const defaultScenarioPath = useMemo(() => {
    if (current?.scenarioPath) {
      return current.scenarioPath;
    }
    if (storedPaths.scenarioPath) {
      return storedPaths.scenarioPath;
    }
    const firstValid = (workflowsQuery.data ?? []).find((workflow) => workflow.errors === null);
    return firstValid?.path ?? "";
  }, [current?.scenarioPath, storedPaths.scenarioPath, workflowsQuery.data]);

  const initialValues = useMemo<StartRunFormValues>(
    () => ({
      scenarioPath: defaultScenarioPath,
      hostPath: storedPaths.hostPath ?? "",
      projectPath: storedPaths.projectPath ?? "",
      importDir: storedPaths.importDir ?? "",
      seed: current?.seed ?? 0,
      maxSteps: current?.maxSteps ?? 1000,
      maxDurationMs: current?.maxDurationMs ?? 300_000,
      livenessThresholdMs: current?.livenessThresholdMs ?? 5000,
      reuseProject: false,
    }),
    [current, defaultScenarioPath, storedPaths],
  );

  // Keep the scenario field filled once the workflow list arrives (first paint
  // may have an empty options list / empty default).
  useEffect(() => {
    if (busy || defaultScenarioPath.length === 0) {
      return;
    }
    const currentValue = formRef.current?.getFieldValue("scenarioPath") as string | undefined;
    if (currentValue === undefined || currentValue.length === 0) {
      formRef.current?.setFieldValue("scenarioPath", defaultScenarioPath);
    }
  }, [busy, defaultScenarioPath]);

  const currentOpLabel = current?.currentOp
    ? `${current.currentOp.action}${current.currentOp.target ? ` → ${current.currentOp.target}` : ""}`
    : "—";

  const resultLabel = current?.verdict
    ? current.verdict.toUpperCase()
    : busy
      ? "IN PROGRESS"
      : "—";

  return (
    <PageContainer
      title="Active Run"
      subTitle="Alcedo UI Fuzz Automation"
      tags={
        <Space size={4}>
          {current ? (
            <Tag color={statusColor(current.status)}>{current.status.toUpperCase()}</Tag>
          ) : null}
          {current?.verdict ? (
            <Tag
              color={verdictColor(current.verdict)}
              icon={
                current.verdict === "pass" ? <CheckCircleOutlined /> : <CloseCircleOutlined />
              }
            >
              {current.verdict.toUpperCase()}
            </Tag>
          ) : null}
        </Space>
      }
      extra={
        <Space>
          <Link href="/runs">
            <Button icon={<UnorderedListOutlined />}>Results</Button>
          </Link>
          <Link href="/workflows">
            <Button>Workflows</Button>
          </Link>
          <Link href="/catalog">
            <Button>Catalog</Button>
          </Link>
          <Button
            type="primary"
            icon={<PlayCircleOutlined />}
            loading={startMutation.isPending}
            disabled={busy}
            onClick={() => void formRef.current?.submit()}
          >
            Start
          </Button>
          <Button
            danger
            icon={<PauseCircleOutlined />}
            loading={stopMutation.isPending}
            disabled={!busy}
            onClick={() => stopMutation.mutate()}
          >
            Stop
          </Button>
        </Space>
      }
    >
      <Row gutter={[16, 16]}>
        <Col xs={24} lg={10}>
          <ProCard title="Run controls" variant="outlined">
            <ProForm<StartRunFormValues>
              formRef={formRef}
              submitter={false}
              disabled={busy}
              initialValues={initialValues}
              onValuesChange={(_, values) => {
                saveStoredRunPaths({
                  hostPath: values.hostPath ?? "",
                  projectPath: values.projectPath ?? "",
                  importDir: values.importDir ?? "",
                  scenarioPath: values.scenarioPath ?? "",
                });
              }}
              onFinish={async (values) => {
                await startMutation.mutateAsync({
                  ...values,
                  projectPath: values.projectPath || undefined,
                  importDir: values.importDir || undefined,
                });
              }}
            >
              <ProFormSelect
                name="scenarioPath"
                label="Scenario YAML"
                options={workflowOptions}
                showSearch
                rules={[{ required: true, message: "Select a workflow" }]}
                fieldProps={{
                  optionFilterProp: "label",
                  placeholder: workflowsQuery.isLoading
                    ? "Loading workflows…"
                    : "Search workflows…",
                  loading: workflowsQuery.isLoading,
                  allowClear: false,
                }}
                extra={
                  workflowsQuery.error ? (
                    <Typography.Text type="danger">
                      {(workflowsQuery.error as Error).message}
                    </Typography.Text>
                  ) : (
                    <Typography.Text type="secondary">
                      Same files as the Workflows page; invalid YAML is disabled.
                    </Typography.Text>
                  )
                }
              />
              <ProFormItem
                name="hostPath"
                label="Test host executable"
                rules={[{ required: true, message: "Host path is required" }]}
              >
                <PathBrowseInput
                  selectionMode="file"
                  executableOnly
                  browseTitle="Select test host executable"
                  placeholder="Browse to alcedo_studio_test_host.exe"
                />
              </ProFormItem>
              <ProFormItem name="projectPath" label="Project path">
                <PathBrowseInput
                  selectionMode="directory"
                  browseTitle="Select project directory"
                  placeholder="Scratch project directory"
                />
              </ProFormItem>
              <ProFormItem name="importDir" label="Import directory">
                <PathBrowseInput
                  selectionMode="directory"
                  browseTitle="Select import directory"
                  placeholder="RAW sample tree"
                />
              </ProFormItem>
              <ProFormDigit name="seed" label="Seed" min={0} fieldProps={{ precision: 0 }} />
              <ProFormDigit name="maxSteps" label="Max steps" min={1} fieldProps={{ precision: 0 }} />
              <ProFormDigit
                name="maxDurationMs"
                label="Max duration (ms)"
                min={1000}
                fieldProps={{ precision: 0 }}
              />
              <ProFormDigit
                name="livenessThresholdMs"
                label="Liveness threshold (ms)"
                min={500}
                fieldProps={{ precision: 0 }}
              />
              <ProFormSwitch name="reuseProject" label="Reuse project" />
            </ProForm>
          </ProCard>
        </Col>

        <Col xs={24} lg={14}>
          <ProCard title="Live status" variant="outlined" style={{ marginBottom: 16 }}>
            <Row gutter={16}>
              <Col span={6}>
                <Statistic title="Current operation" value={currentOpLabel} />
              </Col>
              <Col span={6}>
                <Statistic title="Step" value={current?.stepCounter ?? 0} />
              </Col>
              <Col span={6}>
                <Statistic title="Elapsed" value={formatElapsed(current?.elapsedMs ?? 0)} />
              </Col>
              <Col span={6}>
                <Statistic
                  title="Workflow result"
                  value={resultLabel}
                  valueStyle={{ color: verdictValueColor(current?.verdict), fontSize: 20 }}
                  prefix={
                    current?.verdict === "pass" ? (
                      <CheckCircleOutlined />
                    ) : current?.verdict ? (
                      <CloseCircleOutlined />
                    ) : undefined
                  }
                />
              </Col>
            </Row>
            <Row gutter={16} style={{ marginTop: 24 }}>
              <Col span={8}>
                <Space>
                  <HeartOutlined />
                  <Badge
                    status={current?.heartbeatAlive ? "success" : "default"}
                    text={
                      current?.heartbeatAlive
                        ? `Heartbeat #${current.heartbeat?.counter ?? 0}`
                        : "Heartbeat idle"
                    }
                  />
                </Space>
              </Col>
              <Col span={8}>
                <Typography.Text type="secondary">
                  Host PID: {current?.hostPid ?? "—"}
                </Typography.Text>
              </Col>
              <Col span={8}>
                <Typography.Text type="secondary">
                  Probe: {current?.probeSocket ?? "—"}
                </Typography.Text>
              </Col>
            </Row>
            {current?.verdict ? (
              <Typography.Paragraph style={{ marginTop: 16, marginBottom: 0 }}>
                Verdict:{" "}
                <Tag
                  color={verdictColor(current.verdict)}
                  icon={
                    current.verdict === "pass" ? (
                      <CheckCircleOutlined />
                    ) : (
                      <CloseCircleOutlined />
                    )
                  }
                >
                  {current.verdict}
                </Tag>
                {current.failureReason ? ` — ${current.failureReason}` : null}
                {current.persistedRunId ? (
                  <>
                    {" "}
                    <Link href={`/runs/${encodeURIComponent(current.persistedRunId)}`}>
                      View archived run
                    </Link>
                  </>
                ) : null}
              </Typography.Paragraph>
            ) : null}
            {current?.error ? (
              <Typography.Paragraph type="danger" style={{ marginTop: 8 }}>
                {current.error}
              </Typography.Paragraph>
            ) : null}
          </ProCard>

          <ProCard
            title="Qt log stream"
            variant="outlined"
            extra={<Typography.Text type="secondary">{logs.length} lines</Typography.Text>}
          >
            {logs.length === 0 ? (
              <Empty image={Empty.PRESENTED_IMAGE_SIMPLE} description="Waiting for host stdout/stderr" />
            ) : (
              <div
                ref={logScrollRef}
                style={{
                  maxHeight: 420,
                  overflow: "auto",
                  fontFamily: LOG_FONT,
                }}
              >
                <List
                  size="small"
                  dataSource={logs}
                  renderItem={(item) => (
                    <List.Item style={{ padding: "4px 0", fontFamily: LOG_FONT }}>
                      <Typography.Text
                        type={item.stream === "stderr" ? "danger" : undefined}
                        style={{ fontFamily: LOG_FONT, fontSize: 12 }}
                      >
                        [{item.stream}] {item.line}
                      </Typography.Text>
                    </List.Item>
                  )}
                />
              </div>
            )}
          </ProCard>
        </Col>
      </Row>
    </PageContainer>
  );
}
