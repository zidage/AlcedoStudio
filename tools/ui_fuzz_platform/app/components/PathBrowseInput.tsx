"use client";

//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

/**
 * Controlled path input with a server-side filesystem browser. Used for local
 * absolute paths (test host executable, project/import directories, etc.) that
 * the browser cannot pick via `<input type="file">`.
 */

import React, { useCallback, useEffect, useState } from "react";
import {
  App as AntdApp,
  Button,
  Input,
  List,
  Modal,
  Space,
  Typography,
} from "antd";
import {
  FolderOpenOutlined,
  FileOutlined,
  FolderOutlined,
  HddOutlined,
  ArrowUpOutlined,
} from "@ant-design/icons";

import { browseFs } from "../lib/api";
import type { FsBrowseEntry, FsBrowseResult } from "../lib/types";

export type PathBrowseSelectionMode = "file" | "directory";

export interface PathBrowseInputProps {
  value?: string;
  onChange?: (value: string) => void;
  placeholder?: string;
  disabled?: boolean;
  /**
   * `file` selects a file entry; `directory` selects the folder currently
   * listed (or a navigable subdirectory via Use / Select).
   */
  selectionMode?: PathBrowseSelectionMode;
  /**
   * When true (file mode), only executable candidates are selectable
   * (platform-aware extension filter on the server).
   */
  executableOnly?: boolean;
  /** Modal title. */
  browseTitle?: string;
}

function entryIcon(entry: FsBrowseEntry): React.ReactNode {
  switch (entry.kind) {
    case "drive":
      return <HddOutlined />;
    case "directory":
      return <FolderOutlined />;
    default:
      return <FileOutlined />;
  }
}

export function PathBrowseInput(props: PathBrowseInputProps): React.ReactElement {
  const selectionMode = props.selectionMode ?? "file";
  const { message } = AntdApp.useApp();
  const [open, setOpen] = useState(false);
  const [loading, setLoading] = useState(false);
  const [listing, setListing] = useState<FsBrowseResult | null>(null);
  const [selected, setSelected] = useState<string | null>(null);

  const load = useCallback(
    async (path: string | undefined, preferSelect?: string) => {
      setLoading(true);
      try {
        const result = await browseFs({
          path,
          executableOnly: selectionMode === "file" ? props.executableOnly : false,
        });
        setListing(result);
        if (selectionMode === "directory") {
          // Current folder is the default selection unless browsing drive roots.
          setSelected(result.path.length > 0 ? result.path : null);
        } else if (
          preferSelect !== undefined &&
          result.entries.some((entry) => entry.path === preferSelect && entry.selectable)
        ) {
          setSelected(preferSelect);
        } else {
          setSelected(null);
        }
      } catch (error) {
        message.error(error instanceof Error ? error.message : String(error));
      } finally {
        setLoading(false);
      }
    },
    [message, props.executableOnly, selectionMode],
  );

  useEffect(() => {
    if (!open) return;
    const current = props.value?.trim() ?? "";
    if (current.length === 0) {
      void load(undefined);
      return;
    }
    if (selectionMode === "directory") {
      void load(current);
    } else {
      void load(parentOfFileHint(current), current);
    }
  }, [open, load, props.value, selectionMode]);

  const atOsRoots = listing !== null && listing.parent === null && listing.path === "";
  const atPosixRoot = listing !== null && listing.parent === null && listing.path === "/";

  const confirmSelection = (path: string) => {
    props.onChange?.(path);
    setOpen(false);
  };

  const selectLabel = selectionMode === "directory" ? "Use this folder" : "Select";

  return (
    <>
      <Space.Compact style={{ width: "100%" }}>
        <Input
          value={props.value}
          onChange={(event) => props.onChange?.(event.target.value)}
          placeholder={props.placeholder}
          disabled={props.disabled}
        />
        <Button
          icon={<FolderOpenOutlined />}
          disabled={props.disabled}
          onClick={() => setOpen(true)}
        >
          Browse
        </Button>
      </Space.Compact>

      <Modal
        title={props.browseTitle ?? "Select path"}
        open={open}
        onCancel={() => setOpen(false)}
        width={720}
        destroyOnHidden
        footer={[
          <Button key="cancel" onClick={() => setOpen(false)}>
            Cancel
          </Button>,
          <Button
            key="select"
            type="primary"
            disabled={selected === null}
            onClick={() => {
              if (selected !== null) confirmSelection(selected);
            }}
          >
            {selectLabel}
          </Button>,
        ]}
      >
        <Space direction="vertical" style={{ width: "100%" }} size="middle">
          <Space wrap>
            <Button
              icon={<ArrowUpOutlined />}
              disabled={loading || listing === null || atOsRoots || atPosixRoot}
              onClick={() => {
                if (listing === null) return;
                if (listing.parent === null) {
                  void load("");
                } else {
                  void load(listing.parent);
                }
              }}
            >
              Up
            </Button>
            <Typography.Text code style={{ wordBreak: "break-all" }}>
              {listing?.path.length ? listing.path : "(drives)"}
            </Typography.Text>
          </Space>

          <List
            size="small"
            loading={loading}
            bordered
            style={{ maxHeight: 420, overflow: "auto" }}
            dataSource={visibleEntries(listing?.entries ?? [], selectionMode)}
            locale={{ emptyText: "Empty directory" }}
            renderItem={(entry) => {
              const navigable = entry.kind === "directory" || entry.kind === "drive";
              const canSelectFile = selectionMode === "file" && entry.kind === "file" && entry.selectable;
              const isSelected =
                selectionMode === "file"
                  ? selected === entry.path
                  : selected === entry.path && entry.kind === "directory";
              return (
                <List.Item
                  style={{
                    cursor: navigable || canSelectFile ? "pointer" : "default",
                    background: isSelected ? "rgba(22, 119, 255, 0.08)" : undefined,
                    opacity:
                      selectionMode === "file" && entry.kind === "file" && !entry.selectable
                        ? 0.45
                        : 1,
                  }}
                  onClick={() => {
                    if (navigable) {
                      void load(entry.path);
                      return;
                    }
                    if (canSelectFile) setSelected(entry.path);
                  }}
                  onDoubleClick={() => {
                    if (selectionMode === "file" && canSelectFile) {
                      confirmSelection(entry.path);
                    }
                  }}
                >
                  <Space>
                    {entryIcon(entry)}
                    <Typography.Text>{entry.name}</Typography.Text>
                  </Space>
                </List.Item>
              );
            }}
          />
        </Space>
      </Modal>
    </>
  );
}

/** Directory mode hides files so the list stays focused on folders. */
function visibleEntries(
  entries: FsBrowseEntry[],
  selectionMode: PathBrowseSelectionMode,
): FsBrowseEntry[] {
  if (selectionMode !== "directory") return entries;
  return entries.filter((entry) => entry.kind === "directory" || entry.kind === "drive");
}

/** Uses the directory containing `value` when it looks like a file path. */
function parentOfFileHint(value: string): string {
  const normalized = value.replace(/[/\\]+$/, "");
  const slash = Math.max(normalized.lastIndexOf("/"), normalized.lastIndexOf("\\"));
  if (slash <= 0) return normalized;
  return normalized.slice(0, slash);
}
