//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

import { join } from "node:path";
import { fileURLToPath } from "node:url";
import { readFile } from "node:fs/promises";

import { afterEach, describe, expect, it } from "vitest";

import { startDashboardServer, type DashboardServer } from "../src/dashboard/http-server.js";
import { scenarioToFlow, flowToYamlText } from "../src/flow-graph.js";
import { loadScenario, parseScenario } from "../src/loader.js";
import { ProcessManager } from "../src/process-manager.js";
import { makeTempDir } from "./helpers/fixtures.js";

const fakeHostPath = fileURLToPath(new URL("./helpers/fake-host.mjs", import.meta.url));
const acceptanceScenarioPath = fileURLToPath(
  new URL("../scenarios/library_to_editor_exposure.yaml", import.meta.url),
);

async function waitFor(
  predicate: () => boolean,
  timeoutMs = 15_000,
  label = "condition",
): Promise<void> {
  const started = Date.now();
  while (Date.now() - started < timeoutMs) {
    if (predicate()) return;
    await new Promise((resolve) => setTimeout(resolve, 50));
  }
  throw new Error(`Timed out waiting for ${label}`);
}

interface ServerFixture {
  dashboard: DashboardServer;
  baseUrl: string;
  workflowsDir: string;
  qmlRootDir: string;
}

async function startServer(options: { withManager?: boolean } = {}): Promise<ServerFixture> {
  const workflowsDir = makeTempDir();
  const qmlRootDir = makeTempDir();
  const manager = options.withManager === true ? new ProcessManager({
    hostCommandOverride: [process.execPath, fakeHostPath],
  }) : undefined;
  const dashboard = await startDashboardServer({
    host: "127.0.0.1",
    port: 0,
    manager,
    workflowsDir,
    qmlRootDir,
  });
  return { dashboard, baseUrl: `http://127.0.0.1:${dashboard.port}`, workflowsDir, qmlRootDir };
}

describe("Workflow and catalog API", () => {
  let fixture: ServerFixture | undefined;

  afterEach(async () => {
    await fixture?.dashboard.close();
    fixture = undefined;
  });

  it("WorkflowApiSavesEditorYamlAndReloadsIdenticalSemantics", async () => {
    fixture = await startServer();
    const handAuthored = await loadScenario(acceptanceScenarioPath);
    const editorYaml = flowToYamlText(scenarioToFlow(handAuthored));

    const putResponse = await fetch(`${fixture.baseUrl}/api/workflows/editor_built`, {
      method: "PUT",
      headers: { "content-type": "application/json" },
      body: JSON.stringify({ yaml: editorYaml }),
    });
    expect(putResponse.status).toBe(200);
    const putBody = (await putResponse.json()) as { path: string; validation: { valid: boolean } };
    expect(putBody.validation.valid).toBe(true);

    const savedOnDisk = await readFile(putBody.path, "utf8");
    expect(parseScenario(savedOnDisk)).toEqual(handAuthored);

    const getResponse = await fetch(`${fixture.baseUrl}/api/workflows/editor_built`);
    expect(getResponse.status).toBe(200);
    const getBody = (await getResponse.json()) as { yaml: string };
    expect(parseScenario(getBody.yaml)).toEqual(handAuthored);

    const listResponse = await fetch(`${fixture.baseUrl}/api/workflows`);
    const listBody = (await listResponse.json()) as {
      workflows: Array<{ name: string; scenarioName: string | null; start: string | null; errors: string[] | null }>;
    };
    const listed = listBody.workflows.find((workflow) => workflow.name === "editor_built");
    expect(listed).toBeDefined();
    expect(listed!.scenarioName).toBe("library_to_editor_exposure");
    expect(listed!.start).toBe("workspace_ready");
    expect(listed!.errors).toBeNull();
  });

  it("WorkflowApiRejectsSchemaViolationWithErrorList", async () => {
    fixture = await startServer();
    const invalid = [
      "name: broken",
      "start: missing_node",
      "nodes:",
      "  other_node:",
      "    op: { action: click, target: x }",
    ].join("\n");

    const putResponse = await fetch(`${fixture.baseUrl}/api/workflows/broken`, {
      method: "PUT",
      headers: { "content-type": "application/json" },
      body: JSON.stringify({ yaml: invalid }),
    });
    expect(putResponse.status).toBe(400);
    const body = (await putResponse.json()) as { error: string };
    expect(body.error).toContain("missing_node");

    const listResponse = await fetch(`${fixture.baseUrl}/api/workflows`);
    const listBody = (await listResponse.json()) as { workflows: unknown[] };
    expect(listBody.workflows.length).toBe(0);
  });

  it("WorkflowApiRejectsPathEscapingNames", async () => {
    fixture = await startServer();
    const putResponse = await fetch(
      `${fixture.baseUrl}/api/workflows/${encodeURIComponent("..%2F..%2Fevil")}`,
      {
        method: "PUT",
        headers: { "content-type": "application/json" },
        body: JSON.stringify({ yaml: "name: x\nstart: a\nnodes:\n  a:\n    op: { action: waitMs, ms: 1 }" }),
      },
    );
    expect(putResponse.status).toBe(400);
    const body = (await putResponse.json()) as { error: string };
    expect(body.error).toContain("Invalid workflow name");
  });

  it("CatalogApiScansQmlRootAndReportsEntries", async () => {
    fixture = await startServer();
    const { writeFile } = await import("node:fs/promises");
    await writeFile(
      join(fixture.qmlRootDir, "Panel.qml"),
      [
        "Item {",
        '    objectName: "panelRoot"',
        "    AdjustmentSlider {",
        '        objectName: "toneExposureSlider"',
        "    }",
        "}",
      ].join("\n"),
    );

    const response = await fetch(`${fixture.baseUrl}/api/catalog`);
    expect(response.status).toBe(200);
    const catalog = (await response.json()) as {
      filesScanned: number;
      entries: Array<{ objectName: string | null; opKinds: string[] }>;
    };
    expect(catalog.filesScanned).toBe(1);
    expect(catalog.entries.map((entry) => entry.objectName)).toEqual([
      "panelRoot",
      "toneExposureSlider",
    ]);
    expect(catalog.entries[1]!.opKinds).toEqual(["click", "drag", "wait"]);
  });

  it("CatalogApiDiffsPostedSnapshotAndMarksStaleEntries", async () => {
    fixture = await startServer();
    const { writeFile } = await import("node:fs/promises");
    await writeFile(
      join(fixture.qmlRootDir, "Panel.qml"),
      [
        "Item {",
        '    objectName: "panelRoot"',
        "    IconActionButton {",
        '        objectName: "retiredButton"',
        "    }",
        "}",
      ].join("\n"),
    );

    const response = await fetch(`${fixture.baseUrl}/api/catalog/staleness`, {
      method: "POST",
      headers: { "content-type": "application/json" },
      body: JSON.stringify({
        snapshot: { elements: [{ objectName: "panelRoot" }] },
      }),
    });
    expect(response.status).toBe(200);
    const body = (await response.json()) as {
      report: {
        present: number;
        stale: number;
        entries: Array<{ entry: { objectName: string | null }; status: string }>;
      };
    };
    expect(body.report.present).toBe(1);
    expect(body.report.stale).toBe(1);
    const retired = body.report.entries.find((item) => item.entry.objectName === "retiredButton");
    expect(retired!.status).toBe("stale");
  });

  it("DashboardApiServesLiveProbeSnapshotOnlyWhileRunIsActive", async () => {
    fixture = await startServer({ withManager: true });
    const manager = fixture.dashboard.manager;

    const idleResponse = await fetch(`${fixture.baseUrl}/api/runs/active/snapshot`);
    expect(idleResponse.status).toBe(409);

    const startResponse = await fetch(`${fixture.baseUrl}/api/runs/start`, {
      method: "POST",
      headers: { "content-type": "application/json" },
      body: JSON.stringify({
        scenarioPath: fileURLToPath(new URL("./helpers/slow_walk.yaml", import.meta.url)),
        hostPath: "unused-when-override-set",
        projectPath: makeTempDir(),
        importDir: makeTempDir(),
        maxSteps: 50,
        maxDurationMs: 60_000,
        startupTimeoutMs: 10_000,
        outDir: makeTempDir(),
      }),
    });
    expect(startResponse.status).toBe(202);
    await waitFor(() => manager.getSnapshot().status === "running", 15_000, "running");

    const liveResponse = await fetch(`${fixture.baseUrl}/api/runs/active/snapshot`);
    expect(liveResponse.status).toBe(200);
    const liveBody = (await liveResponse.json()) as {
      snapshot: { elements: Array<{ objectName: string }> };
    };
    expect(liveBody.snapshot.elements.some((element) => element.objectName === "workspaceHost")).toBe(true);

    await manager.stop();
    const stoppedResponse = await fetch(`${fixture.baseUrl}/api/runs/active/snapshot`);
    expect(stoppedResponse.status).toBe(409);
  });
});
