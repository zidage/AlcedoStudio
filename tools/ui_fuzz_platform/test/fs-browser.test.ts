//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

import { mkdirSync, writeFileSync } from "node:fs";
import { join } from "node:path";

import { afterEach, describe, expect, it } from "vitest";

import { startDashboardServer, type DashboardServer } from "../src/dashboard/http-server.js";
import {
  browseDirectory,
  defaultExecutableExtensions,
  repoRootDir,
} from "../src/fs-browser.js";
import { makeTempDir } from "./helpers/fixtures.js";

describe("Filesystem browse helpers", () => {
  it("FsBrowserListsDirectoryEntriesWithExecutableFilter", async () => {
    const dir = makeTempDir("fuzz-fs-");
    mkdirSync(join(dir, "subdir"));
    writeFileSync(join(dir, "alcedo_studio_test_host.exe"), "mz");
    writeFileSync(join(dir, "readme.txt"), "notes");

    const listing = await browseDirectory({
      path: dir,
      executableOnly: true,
    });

    expect(listing.path).toBe(dir);
    expect(listing.parent).not.toBeNull();
    expect(listing.entries.some((entry) => entry.name === "subdir" && entry.kind === "directory")).toBe(
      true,
    );

    const exe = listing.entries.find((entry) => entry.name === "alcedo_studio_test_host.exe");
    expect(exe).toMatchObject({ kind: "file", selectable: true });

    const txt = listing.entries.find((entry) => entry.name === "readme.txt");
    if (process.platform === "win32") {
      expect(txt).toMatchObject({ kind: "file", selectable: false });
      expect(defaultExecutableExtensions()).toContain(".exe");
    } else {
      expect(txt).toMatchObject({ kind: "file", selectable: true });
      expect(defaultExecutableExtensions()).toEqual([]);
    }
  });

  it("FsBrowserRejectsFilePathAsBrowseTarget", async () => {
    const dir = makeTempDir("fuzz-fs-file-");
    const filePath = join(dir, "not-a-dir.exe");
    writeFileSync(filePath, "mz");

    await expect(browseDirectory({ path: filePath })).rejects.toThrow(/Not a directory/);
  });

  it("FsBrowserDefaultRootResolvesUnderRepo", async () => {
    const root = repoRootDir();
    expect(root.replace(/\\/g, "/")).toMatch(/pu-erh_lab$/);
  });
});

describe("Dashboard filesystem browse API", () => {
  let dashboard: DashboardServer | undefined;

  afterEach(async () => {
    if (dashboard !== undefined) {
      await dashboard.close();
      dashboard = undefined;
    }
  });

  it("DashboardApiServesFsBrowseListingForExecutablePicker", async () => {
    const dir = makeTempDir("fuzz-fs-api-");
    writeFileSync(join(dir, "host.exe"), "mz");
    writeFileSync(join(dir, "notes.md"), "x");

    dashboard = await startDashboardServer({ host: "127.0.0.1", port: 0 });

    const response = await fetch(
      `http://127.0.0.1:${dashboard.port}/api/fs/browse?path=${encodeURIComponent(dir)}&executable=1`,
    );
    expect(response.status).toBe(200);
    const body = (await response.json()) as {
      path: string;
      entries: Array<{ name: string; selectable: boolean; kind: string }>;
    };
    expect(body.path).toBe(dir);
    expect(body.entries.find((entry) => entry.name === "host.exe")?.selectable).toBe(true);
    if (process.platform === "win32") {
      expect(body.entries.find((entry) => entry.name === "notes.md")?.selectable).toBe(false);
    }
  });

  it("DashboardApiBrowseWithoutPathStartsAtPreferredRoot", async () => {
    dashboard = await startDashboardServer({ host: "127.0.0.1", port: 0 });
    const response = await fetch(`http://127.0.0.1:${dashboard.port}/api/fs/browse`);
    expect(response.status).toBe(200);
    const body = (await response.json()) as { path: string; entries: unknown[] };
    expect(body.path.length).toBeGreaterThan(0);
    expect(Array.isArray(body.entries)).toBe(true);
  });
});
