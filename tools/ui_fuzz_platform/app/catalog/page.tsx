"use client";

//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

/**
 * Element catalog: every interactive element the QML scanner found in
 * `alcedo_main/qml`. The list is a candidate catalog for authoring; the
 * runner always resolves targets against the live tree, and the "Check live"
 * action diffs the catalog against the active run's probe snapshot to mark
 * entries missing at runtime as stale.
 */

import React from "react";
import Link from "next/link";
import { PageContainer, ProTable } from "@ant-design/pro-components";
import type { ProColumns } from "@ant-design/pro-components";
import { useQuery, useQueryClient } from "@tanstack/react-query";
import { Alert, App, Button, Space, Tag, Typography } from "antd";
import { ApartmentOutlined, PlayCircleOutlined, ReloadOutlined, SyncOutlined } from "@ant-design/icons";

import { fetchCatalog, fetchLiveStaleness } from "../lib/api";
import type { CatalogEntry, StalenessReport } from "../lib/types";

function entryKey(entry: CatalogEntry): string {
  return `${entry.source}:${entry.line}`;
}

function statusTag(status: string | undefined): React.ReactElement {
  switch (status) {
    case "present":
      return <Tag color="success">present</Tag>;
    case "stale":
      return <Tag color="error">stale</Tag>;
    case "dynamic":
      return <Tag>dynamic</Tag>;
    default:
      return <Tag>unchecked</Tag>;
  }
}

export default function CatalogPage(): React.ReactElement {
  const { message } = App.useApp();
  const queryClient = useQueryClient();
  const [staleness, setStaleness] = React.useState<StalenessReport | null>(null);
  const [checking, setChecking] = React.useState(false);

  const catalogQuery = useQuery({
    queryKey: ["catalog"],
    queryFn: fetchCatalog,
  });

  const statusByKey = React.useMemo(() => {
    const map = new Map<string, { status: string; matchedBy: string | null }>();
    for (const item of staleness?.entries ?? []) {
      map.set(entryKey(item.entry), { status: item.status, matchedBy: item.matchedBy });
    }
    return map;
  }, [staleness]);

  const checkLive = async (): Promise<void> => {
    setChecking(true);
    try {
      const { report } = await fetchLiveStaleness();
      setStaleness(report);
      void message.success(
        `Live diff: ${report.present} present, ${report.stale} stale, ${report.dynamic} dynamic.`,
      );
    } catch (error) {
      setStaleness(null);
      void message.warning((error as Error).message);
    } finally {
      setChecking(false);
    }
  };

  const rescan = async (): Promise<void> => {
    setStaleness(null);
    await queryClient.invalidateQueries({ queryKey: ["catalog"] });
  };

  const columns: ProColumns<CatalogEntry>[] = [
    {
      title: "Target",
      dataIndex: "objectName",
      render: (_, row) =>
        row.objectName !== null ? (
          <Typography.Text code copyable>
            {row.objectName}
          </Typography.Text>
        ) : (
          <Typography.Text type="secondary" code>
            {row.expression}
          </Typography.Text>
        ),
    },
    {
      title: "Runtime",
      key: "runtime",
      width: 120,
      render: (_, row) => statusTag(statusByKey.get(entryKey(row))?.status),
    },
    {
      title: "Component",
      dataIndex: "component",
      width: 200,
      ellipsis: true,
    },
    {
      title: "Op kinds",
      dataIndex: "opKinds",
      width: 240,
      render: (_, row) => (
        <Space size={4} wrap>
          {row.opKinds.map((kind) => (
            <Tag key={kind}>{kind}</Tag>
          ))}
        </Space>
      ),
    },
    {
      title: "Source",
      key: "source",
      width: 320,
      ellipsis: true,
      render: (_, row) => (
        <Typography.Text type="secondary">
          {row.source}:{row.line}
        </Typography.Text>
      ),
    },
    {
      title: "Binding",
      key: "binding",
      width: 110,
      render: (_, row) => (row.dynamic ? <Tag color="warning">dynamic</Tag> : <Tag>static</Tag>),
    },
  ];

  return (
    <PageContainer
      title="Element catalog"
      subTitle="Candidate automation targets scanned from alcedo_main/qml (advisory; runtime truth is the live tree)"
      extra={
        <Space>
          <Button icon={<ReloadOutlined />} onClick={() => void rescan()} loading={catalogQuery.isFetching}>
            Rescan
          </Button>
          <Button
            type="primary"
            icon={<SyncOutlined />}
            onClick={() => void checkLive()}
            loading={checking}
          >
            Check live
          </Button>
          <Link href="/workflows">
            <Button icon={<ApartmentOutlined />}>Workflows</Button>
          </Link>
          <Link href="/runs/active">
            <Button icon={<PlayCircleOutlined />}>Active run</Button>
          </Link>
        </Space>
      }
    >
      {catalogQuery.error ? (
        <Alert
          type="error"
          showIcon
          message="Catalog scan failed"
          description={(catalogQuery.error as Error).message}
          style={{ marginBottom: 16 }}
        />
      ) : null}
      {staleness !== null && staleness.unmatchedRuntimeNames.length > 0 ? (
        <Alert
          type="info"
          showIcon
          message={`${staleness.unmatchedRuntimeNames.length} runtime elements have no catalog entry`}
          description={staleness.unmatchedRuntimeNames.join(", ")}
          style={{ marginBottom: 16 }}
        />
      ) : null}
      <ProTable<CatalogEntry>
        rowKey={(row) => entryKey(row)}
        columns={columns}
        dataSource={catalogQuery.data?.entries ?? []}
        loading={catalogQuery.isLoading}
        search={false}
        options={false}
        pagination={{ pageSize: 50 }}
        cardBordered
        headerTitle={
          catalogQuery.data !== undefined
            ? `${catalogQuery.data.entries.length} bindings across ${catalogQuery.data.filesScanned} QML files`
            : undefined
        }
      />
    </PageContainer>
  );
}
