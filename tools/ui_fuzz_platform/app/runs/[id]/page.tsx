"use client";

//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

/**
 * Run detail: ordered steps, failing expect with last observed value, failure
 * log tail, and Replay seed control. Ant Design Pro components only.
 */

import React, { useState } from "react";
import Link from "next/link";
import { useParams, useRouter } from "next/navigation";
import {
  PageContainer,
  ProCard,
  ProDescriptions,
  ProTable,
} from "@ant-design/pro-components";
import type { ProColumns } from "@ant-design/pro-components";
import { useMutation, useQuery } from "@tanstack/react-query";
import {
  App as AntdApp,
  Button,
  Space,
  Tag,
  Typography,
  Form,
} from "antd";
import { ArrowLeftOutlined, RedoOutlined } from "@ant-design/icons";

import { PathBrowseInput } from "../../components/PathBrowseInput";
import { fetchRunDetail, replayRun } from "../../lib/api";
import type { StoredStep } from "../../lib/types";

function verdictColor(verdict: string): string {
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

function formatExpectResults(value: unknown): string {
  if (!Array.isArray(value)) return JSON.stringify(value);
  return value
    .map((entry) => {
      const item = entry as {
        ok?: boolean;
        actual?: unknown;
        expect?: { target?: string; property?: string; matcher?: string; expected?: unknown };
        errorCode?: string;
      };
      const target = item.expect?.target ?? "?";
      const property = item.expect?.property ?? "?";
      const status = item.ok ? "ok" : `fail${item.errorCode ? ` (${item.errorCode})` : ""}`;
      return `${target}.${property}=${JSON.stringify(item.actual)} [${status}]`;
    })
    .join("; ");
}

export default function RunDetailPage(): React.ReactElement {
  const params = useParams<{ id: string }>();
  const runId = decodeURIComponent(params.id);
  const router = useRouter();
  const { message } = AntdApp.useApp();
  const [hostPath, setHostPath] = useState("");

  const detailQuery = useQuery({
    queryKey: ["runs", "detail", runId],
    queryFn: () => fetchRunDetail(runId),
  });

  const replayMutation = useMutation({
    mutationFn: () =>
      replayRun(runId, {
        hostPath: hostPath.trim() || undefined,
      }),
    onSuccess: (body) => {
      message.success(`Replay started (seed=${body.seed})`);
      router.push("/runs/active");
    },
    onError: (error: Error) => message.error(error.message),
  });

  const detail = detailQuery.data;
  const run = detail?.run;

  const stepColumns: ProColumns<StoredStep>[] = [
    { title: "Seq", dataIndex: "seq", width: 60 },
    { title: "Node", dataIndex: "nodeId", width: 180 },
    {
      title: "Op",
      dataIndex: "op",
      width: 220,
      render: (_, row) => {
        const action = row.op?.action ?? "?";
        const target = row.op?.target;
        return target ? `${action} → ${target}` : action;
      },
    },
    {
      title: "Op ok",
      dataIndex: "opOk",
      width: 80,
      render: (_, row) => (row.opOk ? <Tag color="success">yes</Tag> : <Tag color="error">no</Tag>),
    },
    {
      title: "Expects",
      dataIndex: "expectResults",
      ellipsis: true,
      render: (_, row) => formatExpectResults(row.expectResults),
    },
  ];

  return (
    <PageContainer
      title={run ? run.scenario : "Run detail"}
      subTitle={runId}
      tags={run ? <Tag color={verdictColor(run.verdict)}>{run.verdict}</Tag> : undefined}
      extra={
        <Space>
          <Link href="/runs">
            <Button icon={<ArrowLeftOutlined />}>All results</Button>
          </Link>
          <Link href="/runs/active">
            <Button type="default">Active run</Button>
          </Link>
        </Space>
      }
    >
      {detailQuery.error ? (
        <Typography.Paragraph type="danger">
          {(detailQuery.error as Error).message}
        </Typography.Paragraph>
      ) : null}

      {run ? (
        <>
          <ProCard title="Run header" variant="outlined" style={{ marginBottom: 16 }}>
            <ProDescriptions
              column={2}
              bordered
              size="small"
              dataSource={run}
              columns={[
                { title: "Seed", dataIndex: "seed" },
                {
                  title: "Verdict",
                  dataIndex: "verdict",
                  render: (_, row) => (
                    <Tag color={verdictColor(row.verdict)}>{row.verdict}</Tag>
                  ),
                },
                {
                  title: "Started",
                  dataIndex: "startedAt",
                  render: (_, row) => new Date(row.startedAt).toLocaleString(),
                },
                {
                  title: "Ended",
                  dataIndex: "endedAt",
                  render: (_, row) => new Date(row.endedAt).toLocaleString(),
                },
                {
                  title: "Scenario path",
                  dataIndex: "scenarioPath",
                  span: 2,
                  render: (_, row) => row.scenarioPath ?? "—",
                },
                {
                  title: "Parent run",
                  dataIndex: "parentRunId",
                  span: 2,
                  render: (_, row) =>
                    row.parentRunId ? (
                      <Link href={`/runs/${encodeURIComponent(row.parentRunId)}`}>
                        {row.parentRunId}
                      </Link>
                    ) : (
                      "—"
                    ),
                },
                {
                  title: "Failure reason",
                  dataIndex: "failureReason",
                  span: 2,
                  render: (_, row) => row.failureReason ?? "—",
                },
              ]}
            />
          </ProCard>

          <ProCard title="Replay seed" variant="outlined" style={{ marginBottom: 16 }}>
            <Typography.Paragraph type="secondary">
              Re-executes the scenario with the same seed ({run.seed}). The runner records a new
              run row linked via parent_run_id; compare step sequences on the new detail page.
            </Typography.Paragraph>
            <Form layout="vertical" onFinish={() => replayMutation.mutate()}>
              <Form.Item label="Host path override">
                <PathBrowseInput
                  executableOnly
                  browseTitle="Select test host executable"
                  placeholder="Optional if original config stored hostPath"
                  value={hostPath}
                  onChange={setHostPath}
                />
              </Form.Item>
              <Form.Item>
                <Button
                  type="primary"
                  icon={<RedoOutlined />}
                  htmlType="submit"
                  loading={replayMutation.isPending}
                >
                  Replay seed
                </Button>
              </Form.Item>
            </Form>
          </ProCard>

          <ProCard title="Steps" variant="outlined" style={{ marginBottom: 16 }}>
            <ProTable<StoredStep>
              rowKey="id"
              columns={stepColumns}
              dataSource={[...(detail?.steps ?? [])]}
              search={false}
              options={false}
              pagination={false}
              size="small"
            />
          </ProCard>

          {detail?.failure ? (
            <ProCard title="Failure artifacts" variant="outlined">
              <ProDescriptions
                column={1}
                bordered
                size="small"
                dataSource={detail.failure}
                columns={[
                  { title: "Kind", dataIndex: "kind" },
                  {
                    title: "Detail",
                    dataIndex: "detail",
                    render: (_, row) => (
                      <Typography.Text code style={{ whiteSpace: "pre-wrap" }}>
                        {JSON.stringify(row.detail, null, 2)}
                      </Typography.Text>
                    ),
                  },
                  {
                    title: "Screenshot",
                    dataIndex: "screenshotPath",
                    render: (_, row) => row.screenshotPath ?? "—",
                  },
                  {
                    title: "Log tail",
                    dataIndex: "logTail",
                    render: (_, row) => (
                      <Typography.Paragraph
                        style={{
                          maxHeight: 320,
                          overflow: "auto",
                          fontFamily: "ui-monospace, SFMono-Regular, Menlo, monospace",
                          whiteSpace: "pre-wrap",
                          marginBottom: 0,
                        }}
                      >
                        {row.logTail.length > 0 ? row.logTail : "(empty)"}
                      </Typography.Paragraph>
                    ),
                  },
                ]}
              />
            </ProCard>
          ) : null}
        </>
      ) : null}
    </PageContainer>
  );
}
