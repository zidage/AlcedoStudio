//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

/**
 * Server-side filesystem listing for the dashboard path picker. The browser
 * cannot expose absolute local paths via `<input type="file">`, so the
 * dashboard lists directories through this module on the machine running the
 * Next.js / API server (bound to 127.0.0.1).
 */

import { access, readdir, realpath, stat } from "node:fs/promises";
import { constants as fsConstants } from "node:fs";
import { dirname, join, normalize, resolve, sep } from "node:path";

import { platformRootDir } from "./paths.js";

export type FsEntryKind = "file" | "directory" | "drive";

export interface FsBrowseEntry {
  readonly name: string;
  readonly path: string;
  readonly kind: FsEntryKind;
  readonly size: number | null;
  /** True when the entry matches the requested extension filter (files only). */
  readonly selectable: boolean;
}

export interface FsBrowseResult {
  readonly path: string;
  readonly parent: string | null;
  readonly platform: NodeJS.Platform;
  readonly entries: FsBrowseEntry[];
}

export interface BrowseDirectoryOptions {
  /** Absolute path to list. Empty / omitted starts at {@link defaultBrowseRoot}. */
  readonly path?: string;
  /**
   * When non-empty, only files whose lower-cased extension is in this list are
   * selectable (directories remain navigable). Empty = every file is selectable.
   * Ignored when {@link executableOnly} is true (server picks platform defaults).
   */
  readonly extensions?: readonly string[];
  /**
   * When true, only host-executable candidates are selectable: `.exe`/`.bat`/
   * `.cmd`/`.com` on Windows; any regular file on other platforms.
   */
  readonly executableOnly?: boolean;
}

/** Repository root two levels above `tools/ui_fuzz_platform`. */
export function repoRootDir(): string {
  return resolve(platformRootDir(), "..", "..");
}

/**
 * Preferred starting folder for the host executable picker: `build/debug` when
 * present, otherwise the repo root.
 */
export async function defaultBrowseRoot(): Promise<string> {
  const debugBuild = join(repoRootDir(), "build", "debug");
  try {
    await access(debugBuild, fsConstants.R_OK);
    const info = await stat(debugBuild);
    if (info.isDirectory()) return debugBuild;
  } catch {
    // fall through
  }
  return repoRootDir();
}

/** Platform default extensions for executable-only browsing. */
export function defaultExecutableExtensions(platform: NodeJS.Platform = process.platform): string[] {
  if (platform === "win32") return [".exe", ".bat", ".cmd", ".com"];
  return [];
}

/**
 * Lists one directory (or Windows drive roots when `path` is empty on win32).
 * Symlinks are resolved; non-directory targets return 400-style errors via throw.
 */
export async function browseDirectory(options: BrowseDirectoryOptions = {}): Promise<FsBrowseResult> {
  const extensions = options.executableOnly
    ? defaultExecutableExtensions()
    : normalizeExtensions(options.extensions ?? []);
  const raw = options.path?.trim() ?? "";

  if (raw.length === 0) {
    if (process.platform === "win32") {
      return {
        path: "",
        parent: null,
        platform: process.platform,
        entries: await listWindowsDrives(),
      };
    }
    return browseDirectory({
      path: sep,
      extensions: options.executableOnly ? undefined : extensions,
      executableOnly: options.executableOnly,
    });
  }

  const absolute = resolve(normalize(raw));
  let target: string;
  try {
    target = await realpath(absolute);
  } catch {
    target = absolute;
  }

  const info = await stat(target);
  if (!info.isDirectory()) {
    throw new Error(`Not a directory: ${target}`);
  }

  const names = await readdir(target);
  const entries: FsBrowseEntry[] = [];

  for (const name of names) {
    if (name === "." || name === "..") continue;
    const full = join(target, name);
    try {
      const child = await stat(full);
      if (child.isDirectory()) {
        entries.push({
          name,
          path: full,
          kind: "directory",
          size: null,
          selectable: false,
        });
      } else if (child.isFile()) {
        entries.push({
          name,
          path: full,
          kind: "file",
          size: child.size,
          selectable: isSelectableFile(name, extensions),
        });
      }
    } catch {
      // Skip entries we cannot stat (permissions, broken links).
    }
  }

  entries.sort((a, b) => {
    if (a.kind !== b.kind) return a.kind === "directory" ? -1 : 1;
    return a.name.localeCompare(b.name, undefined, { sensitivity: "base" });
  });

  return {
    path: target,
    parent: parentPath(target),
    platform: process.platform,
    entries,
  };
}

function normalizeExtensions(extensions: readonly string[]): string[] {
  return extensions
    .map((ext) => ext.trim().toLowerCase())
    .filter((ext) => ext.length > 0)
    .map((ext) => (ext.startsWith(".") ? ext : `.${ext}`));
}

function isSelectableFile(name: string, extensions: readonly string[]): boolean {
  if (extensions.length === 0) return true;
  const lower = name.toLowerCase();
  return extensions.some((ext) => lower.endsWith(ext));
}

function parentPath(absolute: string): string | null {
  if (process.platform === "win32") {
    // `C:\` has parent null so the UI can return to the drive list.
    const trimmed = absolute.replace(/[\\/]+$/, "");
    if (/^[A-Za-z]:$/i.test(trimmed)) return null;
    const parent = dirname(absolute);
    if (parent === absolute) return null;
    return parent;
  }
  if (absolute === sep) return null;
  const parent = dirname(absolute);
  return parent === absolute ? null : parent;
}

async function listWindowsDrives(): Promise<FsBrowseEntry[]> {
  const drives: FsBrowseEntry[] = [];
  for (let code = 65; code <= 90; code += 1) {
    const letter = String.fromCharCode(code);
    const root = `${letter}:\\`;
    try {
      await access(root, fsConstants.R_OK);
      drives.push({
        name: `${letter}:`,
        path: root,
        kind: "drive",
        size: null,
        selectable: false,
      });
    } catch {
      // Drive letter not present.
    }
  }
  return drives;
}
