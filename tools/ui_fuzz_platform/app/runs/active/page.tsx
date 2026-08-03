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
  ProFormSwitch,
  ProFormText,
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
  HeartOutlined,
  PauseCircleOutlined,
  PlayCircleOutlined,
} from "@ant-design/icons";

import { fetchActiveRun, runsWebSocketUrl, startRun, stopRun } from "../../lib/api";
import type { ActiveRunSnapshot, DashboardWsEvent, LogLine, StartRunFormValues } from "../../lib/types";

const MAX_LOG_LINES = 500;

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
  const logSeq = useRef(0);

  const activeQuery = useQuery({
    queryKey: ["runs", "active"],
    queryFn: fetchActiveRun,
    refetchInterval: (query) => {
      const status = query.state.data?.status;
      return status === "running" || status === "starting" || status === "stopping" ? 2000 : false;
    },
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
    onSuccess: () => {
      message.success("Run started");
      setLogs([]);
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

  const initialValues = useMemo<StartRunFormValues>(
    () => ({
      scenarioPath: current?.scenarioPath ?? "scenarios/library_to_editor_exposure.yaml",
      hostPath: "",
      projectPath: "",
      importDir: "",
      seed: current?.seed ?? 0,
      maxSteps: current?.maxSteps ?? 1000,
      maxDurationMs: current?.maxDurationMs ?? 300_000,
      livenessThresholdMs: current?.livenessThresholdMs ?? 5000,
      reuseProject: false,
    }),
    [current],
  );

  const currentOpLabel = current?.currentOp
    ? `${current.currentOp.action}${current.currentOp.target ? ` → ${current.currentOp.target}` : ""}`
    : "—";

  return (
    <PageContainer
      title="Active Run"
      subTitle="Alcedo UI Fuzz Automation"
      tags={
        current ? (
          <Tag color={statusColor(current.status)}>{current.status.toUpperCase()}</Tag>
        ) : undefined
      }
      extra={
        <Space>
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
              onFinish={async (values) => {
                await startMutation.mutateAsync({
                  ...values,
                  projectPath: values.projectPath || undefined,
                  importDir: values.importDir || undefined,
                });
              }}
            >
              <ProFormText
                name="scenarioPath"
                label="Scenario YAML"
                rules={[{ required: true, message: "Scenario path is required" }]}
                placeholder="scenarios/library_to_editor_exposure.yaml"
              />
              <ProFormText
                name="hostPath"
                label="Test host executable"
                rules={[{ required: true, message: "Host path is required" }]}
                placeholder="build/debug/alcedo_studio/src/alcedo_studio_test_host.exe"
              />
              <ProFormText name="projectPath" label="Project path" placeholder="Scratch project directory" />
              <ProFormText name="importDir" label="Import directory" placeholder="RAW sample tree" />
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
              <Col span={8}>
                <Statistic title="Current operation" value={currentOpLabel} />
              </Col>
              <Col span={8}>
                <Statistic title="Step" value={current?.stepCounter ?? 0} />
              </Col>
              <Col span={8}>
                <Statistic title="Elapsed" value={formatElapsed(current?.elapsedMs ?? 0)} />
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
              <Typography.Paragraph style={{ marginTop: 16 }}>
                Verdict: <Tag>{current.verdict}</Tag>
                {current.failureReason ? ` — ${current.failureReason}` : null}
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
              <List
                size="small"
                dataSource={[...logs].reverse()}
                style={{
                  maxHeight: 420,
                  overflow: "auto",
                  fontFamily: "ui-monospace, SFMono-Regular, Menlo, monospace",
                }}
                renderItem={(item) => (
                  <List.Item style={{ padding: "4px 0" }}>
                    <Typography.Text type={item.stream === "stderr" ? "danger" : undefined}>
                      [{item.stream}] {item.line}
                    </Typography.Text>
                  </List.Item>
                )}
              />
            )}
          </ProCard>
        </Col>
      </Row>
    </PageContainer>
  );
}
