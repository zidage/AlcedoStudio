//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

import { fileURLToPath } from "node:url";

import { afterEach, describe, expect, it } from "vitest";
import WebSocket from "ws";

import {
  startDashboardServer,
  type DashboardServer,
} from "../src/dashboard/http-server.js";
import { ProcessManager } from "../src/process-manager.js";
import type { RunManagerEvent } from "../src/run-events.js";
import { isProcessAlive } from "../src/process-tree.js";
import { makeTempDir } from "./helpers/fixtures.js";

const fakeHostPath = fileURLToPath(
  new URL("./helpers/fake-host.mjs", import.meta.url),
);
const slowScenarioPath = fileURLToPath(
  new URL("./helpers/slow_walk.yaml", import.meta.url),
);
const acceptanceScenarioPath = fileURLToPath(
  new URL("../scenarios/library_to_editor_exposure.yaml", import.meta.url),
);

async function waitFor(
  predicate: () => boolean,
  timeoutMs = 10_000,
  label = "condition",
): Promise<void> {
  const started = Date.now();
  while (Date.now() - started < timeoutMs) {
    if (predicate()) return;
    await new Promise((resolve) => setTimeout(resolve, 50));
  }
  throw new Error(`Timed out waiting for ${label}`);
}

describe("ProcessManager", () => {
  const managers: ProcessManager[] = [];

  afterEach(async () => {
    for (const manager of managers) {
      if (manager.isBusy()) await manager.stop();
    }
    managers.length = 0;
  });

  function createManager(): ProcessManager {
    const manager = new ProcessManager({
      hostCommandOverride: [process.execPath, fakeHostPath],
    });
    managers.push(manager);
    return manager;
  }

  it("ProcessManagerStartsRunStreamsLogsAndHeartbeatsThenFinishes", async () => {
    const manager = createManager();
    const events: RunManagerEvent[] = [];
    manager.on("event", (event: RunManagerEvent) => events.push(event));

    const runId = await manager.start({
      scenarioPath: acceptanceScenarioPath,
      hostPath: "unused-when-override-set",
      projectPath: makeTempDir(),
      importDir: makeTempDir(),
      seed: 3,
      maxSteps: 50,
      maxDurationMs: 30_000,
      startupTimeoutMs: 10_000,
      outDir: makeTempDir(),
    });

    expect(runId.length).toBeGreaterThan(0);
    await waitFor(
      () => manager.getSnapshot().status === "finished",
      15_000,
      "finished status",
    );

    const snapshot = manager.getSnapshot();
    expect(snapshot.verdict).toBe("pass");
    expect(snapshot.stepCounter).toBe(4);
    expect(events.some((event) => event.type === "log")).toBe(true);
    expect(events.some((event) => event.type === "heartbeat")).toBe(true);
    expect(events.some((event) => event.type === "stepStart")).toBe(true);
    expect(events.some((event) => event.type === "finished")).toBe(true);
    expect(snapshot.hostPid).toBeNull();
  });

  it("ProcessManagerStopKillsHostProcessTreeWithNoOrphan", async () => {
    const manager = createManager();

    await manager.start({
      scenarioPath: slowScenarioPath,
      hostPath: "unused-when-override-set",
      projectPath: makeTempDir(),
      importDir: makeTempDir(),
      seed: 1,
      maxSteps: 10,
      maxDurationMs: 60_000,
      startupTimeoutMs: 10_000,
      outDir: makeTempDir(),
    });

    await waitFor(
      () => manager.getSnapshot().hostPid !== null,
      10_000,
      "host pid",
    );
    const hostPid = manager.getSnapshot().hostPid!;
    expect(isProcessAlive(hostPid)).toBe(true);

    const stopped = await manager.stop();
    expect(stopped.status).toBe("finished");

    await waitFor(() => !isProcessAlive(hostPid), 5000, "host exit");
    expect(isProcessAlive(hostPid)).toBe(false);
    expect(manager.hasOrphanHost()).toBe(false);
  });
});

describe("Dashboard HTTP/WS API", () => {
  let dashboard: DashboardServer | undefined;

  afterEach(async () => {
    if (dashboard !== undefined) {
      await dashboard.close();
      dashboard = undefined;
    }
  });

  it("DashboardApiStartsStopsAndStreamsLiveLogsOverWebSocket", async () => {
    const manager = new ProcessManager({
      hostCommandOverride: [process.execPath, fakeHostPath],
    });
    dashboard = await startDashboardServer({
      host: "127.0.0.1",
      port: 0,
      manager,
    });

    const wsEvents: RunManagerEvent[] = [];
    const socket = new WebSocket(`ws://127.0.0.1:${dashboard.port}/ws/runs`);
    await new Promise<void>((resolve, reject) => {
      socket.once("open", () => resolve());
      socket.once("error", reject);
    });
    socket.on("message", (data) => {
      wsEvents.push(JSON.parse(String(data)) as RunManagerEvent);
    });

    const health = await fetch(`http://127.0.0.1:${dashboard.port}/api/health`);
    expect(health.status).toBe(200);

    const startResponse = await fetch(
      `http://127.0.0.1:${dashboard.port}/api/runs/start`,
      {
        method: "POST",
        headers: { "content-type": "application/json" },
        body: JSON.stringify({
          scenarioPath: slowScenarioPath,
          hostPath: "unused-when-override-set",
          projectPath: makeTempDir(),
          importDir: makeTempDir(),
          seed: 9,
          maxSteps: 10,
          maxDurationMs: 60_000,
          startupTimeoutMs: 10_000,
          outDir: makeTempDir(),
        }),
      },
    );
    expect(startResponse.status).toBe(202);
    const started = (await startResponse.json()) as { runId: string };
    expect(started.runId.length).toBeGreaterThan(0);

    await waitFor(
      () => wsEvents.some((event) => event.type === "log"),
      10_000,
      "websocket log event",
    );
    await waitFor(
      () =>
        manager.getSnapshot().status === "running" ||
        manager.getSnapshot().hostPid !== null,
      10_000,
      "running",
    );

    const hostPid = manager.getSnapshot().hostPid;
    expect(hostPid).not.toBeNull();

    const stopResponse = await fetch(
      `http://127.0.0.1:${dashboard.port}/api/runs/stop`,
      {
        method: "POST",
      },
    );
    expect(stopResponse.status).toBe(200);
    const stopped = (await stopResponse.json()) as { status: string };
    expect(stopped.status).toBe("finished");

    if (hostPid !== null) {
      await waitFor(() => !isProcessAlive(hostPid), 5000, "api stop host exit");
      expect(isProcessAlive(hostPid)).toBe(false);
    }

    const active = await fetch(
      `http://127.0.0.1:${dashboard.port}/api/runs/active`,
    );
    expect(active.status).toBe(200);
    const snapshot = (await active.json()) as { status: string };
    expect(snapshot.status).toBe("finished");

    socket.close();
  });
});
