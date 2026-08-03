"use client";

//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

/**
 * Results browser: archived runs from the SQLite result store. Ant Design Pro
 * table only — no custom chrome beyond Pro/antd tokens.
 */

import React from "react";
import Link from "next/link";
import { PageContainer, ProTable } from "@ant-design/pro-components";
import type { ProColumns } from "@ant-design/pro-components";
import { useQuery } from "@tanstack/react-query";
import { Button, Space, Tag, Typography } from "antd";
import { PlayCircleOutlined, ReloadOutlined } from "@ant-design/icons";

import { fetchRuns } from "../lib/api";
import type { StoredRunSummary } from "../lib/types";

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

function formatTime(ms: number): string {
  return new Date(ms).toLocaleString();
}

export default function RunsListPage(): React.ReactElement {
  const runsQuery = useQuery({
    queryKey: ["runs", "list"],
    queryFn: () => fetchRuns(200),
    refetchInterval: 5000,
  });

  const columns: ProColumns<StoredRunSummary>[] = [
    {
      title: "Started",
      dataIndex: "startedAt",
      width: 180,
      render: (_, row) => formatTime(row.startedAt),
    },
    {
      title: "Scenario",
      dataIndex: "scenario",
      ellipsis: true,
      render: (_, row) => (
        <Link href={`/runs/${encodeURIComponent(row.id)}`}>{row.scenario}</Link>
      ),
    },
    {
      title: "Seed",
      dataIndex: "seed",
      width: 80,
    },
    {
      title: "Verdict",
      dataIndex: "verdict",
      width: 120,
      render: (_, row) => <Tag color={verdictColor(row.verdict)}>{row.verdict}</Tag>,
    },
    {
      title: "Parent",
      dataIndex: "parentRunId",
      width: 100,
      render: (_, row) =>
        row.parentRunId ? (
          <Link href={`/runs/${encodeURIComponent(row.parentRunId)}`}>replay of…</Link>
        ) : (
          "—"
        ),
    },
    {
      title: "Failure",
      dataIndex: "failureReason",
      ellipsis: true,
      render: (_, row) => row.failureReason ?? "—",
    },
    {
      title: "Id",
      dataIndex: "id",
      width: 120,
      ellipsis: true,
      copyable: true,
    },
  ];

  return (
    <PageContainer
      title="Results"
      subTitle="Archived runs (seed, steps, log tail on failure)"
      extra={
        <Space>
          <Button icon={<ReloadOutlined />} onClick={() => void runsQuery.refetch()}>
            Refresh
          </Button>
          <Link href="/runs/active">
            <Button type="primary" icon={<PlayCircleOutlined />}>
              Active run
            </Button>
          </Link>
        </Space>
      }
    >
      {runsQuery.error ? (
        <Typography.Paragraph type="danger">
          {(runsQuery.error as Error).message}
        </Typography.Paragraph>
      ) : null}
      <ProTable<StoredRunSummary>
        rowKey="id"
        columns={columns}
        dataSource={runsQuery.data ?? []}
        loading={runsQuery.isLoading}
        search={false}
        options={false}
        pagination={{ pageSize: 20 }}
        cardBordered
      />
    </PageContainer>
  );
}
