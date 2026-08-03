//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

import { join } from "node:path";
import { fileURLToPath } from "node:url";

import { afterEach, describe, expect, it } from "vitest";

import { startDashboardServer, type DashboardServer } from "../src/dashboard/http-server.js";
import { ProcessManager } from "../src/process-manager.js";
import { ResultStore } from "../src/result-store.js";
import { makeTempDir } from "./helpers/fixtures.js";

const fakeHostPath = fileURLToPath(new URL("./helpers/fake-host.mjs", import.meta.url));
const acceptanceScenarioPath = fileURLToPath(
  new URL("../scenarios/library_to_editor_exposure.yaml", import.meta.url),
);
const wrongExpectScenarioPath = fileURLToPath(
  new URL("../scenarios/wrong_expect_fails.yaml", import.meta.url),
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

describe("Dashboard results API", () => {
  let dashboard: DashboardServer | undefined;
  let store: ResultStore | undefined;

  afterEach(async () => {
    if (dashboard !== undefined) {
      await dashboard.close();
      dashboard = undefined;
    }
    store?.close();
    store = undefined;
  });

  it("DashboardApiPersistsFailedRunAndServesDetailWithLogTail", async () => {
    store = new ResultStore(join(makeTempDir(), "results.sqlite"));
    const manager = new ProcessManager({
      hostCommandOverride: [process.execPath, fakeHostPath],
      resultStore: store,
    });
    dashboard = await startDashboardServer({
      host: "127.0.0.1",
      port: 0,
      manager,
      resultStore: store,
    });

    const startResponse = await fetch(`http://127.0.0.1:${dashboard.port}/api/runs/start`, {
      method: "POST",
      headers: { "content-type": "application/json" },
      body: JSON.stringify({
        scenarioPath: wrongExpectScenarioPath,
        hostPath: "unused-when-override-set",
        projectPath: makeTempDir(),
        importDir: makeTempDir(),
        seed: 77,
        maxSteps: 20,
        maxDurationMs: 30_000,
        startupTimeoutMs: 10_000,
        outDir: makeTempDir(),
      }),
    });
    expect(startResponse.status).toBe(202);
    const { runId } = (await startResponse.json()) as { runId: string };

    await waitFor(() => manager.getSnapshot().status === "finished", 15_000, "finished");
    const snapshot = manager.getSnapshot();
    expect(snapshot.verdict).toBe("correctness");
    expect(snapshot.persistedRunId).toBe(runId);

    const listResponse = await fetch(`http://127.0.0.1:${dashboard.port}/api/runs`);
    expect(listResponse.status).toBe(200);
    const listBody = (await listResponse.json()) as { runs: Array<{ id: string; seed: number }> };
    expect(listBody.runs.some((row) => row.id === runId && row.seed === 77)).toBe(true);

    const detailResponse = await fetch(`http://127.0.0.1:${dashboard.port}/api/runs/${runId}`);
    expect(detailResponse.status).toBe(200);
    const detail = (await detailResponse.json()) as {
      run: { seed: number; verdict: string };
      steps: Array<{ nodeId: string }>;
      failure: { logTail: string; opHistory: unknown[] } | null;
    };
    expect(detail.run.seed).toBe(77);
    expect(detail.run.verdict).toBe("correctness");
    expect(detail.steps.length).toBeGreaterThan(0);
    expect(detail.failure).not.toBeNull();
    expect(detail.failure!.opHistory.length).toBe(detail.steps.length);
  });

  it("DashboardApiReplayStartsManagedRunWithSameSeed", async () => {
    store = new ResultStore(join(makeTempDir(), "results.sqlite"));
    const manager = new ProcessManager({
      hostCommandOverride: [process.execPath, fakeHostPath],
      resultStore: store,
    });
    dashboard = await startDashboardServer({
      host: "127.0.0.1",
      port: 0,
      manager,
      resultStore: store,
    });

    const startResponse = await fetch(`http://127.0.0.1:${dashboard.port}/api/runs/start`, {
      method: "POST",
      headers: { "content-type": "application/json" },
      body: JSON.stringify({
        scenarioPath: acceptanceScenarioPath,
        hostPath: "unused-when-override-set",
        projectPath: makeTempDir(),
        importDir: makeTempDir(),
        seed: 88,
        maxSteps: 50,
        maxDurationMs: 30_000,
        startupTimeoutMs: 10_000,
        outDir: makeTempDir(),
      }),
    });
    const { runId: originalId } = (await startResponse.json()) as { runId: string };
    await waitFor(() => manager.getSnapshot().status === "finished", 15_000, "original finished");
    expect(manager.getSnapshot().verdict).toBe("pass");

    const originalSteps = store.getStepFingerprints(originalId);

    const replayResponse = await fetch(
      `http://127.0.0.1:${dashboard.port}/api/runs/${originalId}/replay`,
      {
        method: "POST",
        headers: { "content-type": "application/json" },
        body: JSON.stringify({ hostPath: "unused-when-override-set" }),
      },
    );
    expect(replayResponse.status).toBe(202);
    const replayBody = (await replayResponse.json()) as {
      runId: string;
      parentRunId: string;
      seed: number;
    };
    expect(replayBody.parentRunId).toBe(originalId);
    expect(replayBody.seed).toBe(88);

    await waitFor(() => manager.getSnapshot().status === "finished", 15_000, "replay finished");
    expect(manager.getSnapshot().verdict).toBe("pass");
    expect(manager.getSnapshot().parentRunId).toBe(originalId);

    const replaySteps = store.getStepFingerprints(replayBody.runId);
    expect(replaySteps.map((step) => step.nodeId)).toEqual(originalSteps.map((step) => step.nodeId));
  });
});
