"use client";

//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

/**
 * Workflow list: scenario YAML files known to the platform. Each entry opens
 * in the React Flow editor; saving there rewrites the YAML file so the runner
 * executes it with no further edits.
 */

import React from "react";
import Link from "next/link";
import { useRouter } from "next/navigation";
import { PageContainer, ProTable } from "@ant-design/pro-components";
import type { ProColumns } from "@ant-design/pro-components";
import { useQuery } from "@tanstack/react-query";
import { App, Button, Form, Input, Modal, Space, Tag, Typography } from "antd";
import {
  ApartmentOutlined,
  EditOutlined,
  PlayCircleOutlined,
  PlusOutlined,
  ReloadOutlined,
} from "@ant-design/icons";

import { fetchWorkflows } from "../lib/api";
import type { WorkflowSummary } from "../lib/types";

const NAME_PATTERN = /^[A-Za-z0-9][A-Za-z0-9_-]*$/;

export default function WorkflowsPage(): React.ReactElement {
  const router = useRouter();
  const { message } = App.useApp();
  const [createOpen, setCreateOpen] = React.useState(false);
  const [createForm] = Form.useForm<{ name: string }>();

  const workflowsQuery = useQuery({
    queryKey: ["workflows", "list"],
    queryFn: fetchWorkflows,
  });

  const openEditor = (name: string, create: boolean): void => {
    router.push(`/workflows/${encodeURIComponent(name)}${create ? "?create=1" : ""}`);
  };

  const columns: ProColumns<WorkflowSummary>[] = [
    {
      title: "Workflow",
      dataIndex: "name",
      render: (_, row) => (
        <Link href={`/workflows/${encodeURIComponent(row.name)}`}>{row.name}</Link>
      ),
    },
    {
      title: "Scenario",
      dataIndex: "scenarioName",
      render: (_, row) => row.scenarioName ?? "—",
    },
    {
      title: "Start node",
      dataIndex: "start",
      width: 200,
      render: (_, row) => (row.start !== null ? <Typography.Text code>{row.start}</Typography.Text> : "—"),
    },
    {
      title: "State",
      key: "state",
      width: 110,
      render: (_, row) =>
        row.errors === null ? <Tag color="success">valid</Tag> : <Tag color="error">invalid</Tag>,
    },
    {
      title: "Path",
      dataIndex: "path",
      ellipsis: true,
      render: (_, row) => <Typography.Text type="secondary">{row.path}</Typography.Text>,
    },
    {
      title: "",
      key: "actions",
      width: 110,
      render: (_, row) => (
        <Button size="small" icon={<EditOutlined />} onClick={() => openEditor(row.name, false)}>
          Edit
        </Button>
      ),
    },
  ];

  return (
    <PageContainer
      title="Workflows"
      subTitle="Scenario YAML files; the flow editor round-trips them losslessly"
      extra={
        <Space>
          <Button icon={<ReloadOutlined />} onClick={() => void workflowsQuery.refetch()}>
            Refresh
          </Button>
          <Button type="primary" icon={<PlusOutlined />} onClick={() => setCreateOpen(true)}>
            New workflow
          </Button>
          <Link href="/runs/active">
            <Button icon={<PlayCircleOutlined />}>Active run</Button>
          </Link>
        </Space>
      }
    >
      {workflowsQuery.error ? (
        <Typography.Paragraph type="danger">
          {(workflowsQuery.error as Error).message}
        </Typography.Paragraph>
      ) : null}
      <ProTable<WorkflowSummary>
        rowKey="name"
        columns={columns}
        dataSource={workflowsQuery.data ?? []}
        loading={workflowsQuery.isLoading}
        search={false}
        options={false}
        pagination={{ pageSize: 20 }}
        cardBordered
      />
      <Modal
        title="New workflow"
        open={createOpen}
        onCancel={() => setCreateOpen(false)}
        onOk={() => {
          void createForm.validateFields().then((values) => {
            if (workflowsQuery.data?.some((workflow) => workflow.name === values.name)) {
              void message.error(`Workflow '${values.name}' already exists.`);
              return;
            }
            setCreateOpen(false);
            createForm.resetFields();
            openEditor(values.name, true);
          });
        }}
        okText="Create"
        destroyOnHidden
      >
        <Form form={createForm} layout="vertical" preserve={false}>
          <Form.Item
            name="name"
            label="File name (saved as scenarios/<name>.yaml)"
            rules={[
              { required: true, message: "Name is required." },
              {
                pattern: NAME_PATTERN,
                message: "Letters, digits, '-' and '_' only; first character alphanumeric.",
              },
            ]}
          >
            <Input placeholder="library_to_editor_exposure" autoFocus />
          </Form.Item>
        </Form>
      </Modal>
    </PageContainer>
  );
}
