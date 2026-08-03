//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

/**
 * HTTP + WebSocket surface for the dashboard. The Next.js UI talks to these
 * routes; the Qt host is never reached from the browser directly.
 *
 * Routes:
 * - GET  /api/health
 * - GET  /api/runs/active          -> ActiveRunSnapshot
 * - POST /api/runs/start           -> { runId }
 * - POST /api/runs/stop            -> ActiveRunSnapshot
 * - GET  /api/runs                 -> StoredRun[]
 * - GET  /api/runs/:id             -> StoredRunDetail
 * - POST /api/runs/:id/replay      -> { runId } (starts managed replay)
 * - WS   /ws/runs                  -> RunManagerEvent JSON frames
 */

import { createServer, type IncomingMessage, type Server, type ServerResponse } from "node:http";
import { join } from "node:path";
import { WebSocketServer, type WebSocket } from "ws";

import { ProcessManager } from "../process-manager.js";
import { defaultResultDbPath } from "../paths.js";
import { ResultStore } from "../result-store.js";
import type { RunManagerEvent, StartRunRequest } from "../run-events.js";

export interface DashboardServerOptions {
  readonly host?: string;
  readonly port?: number;
  readonly manager?: ProcessManager;
  /** When omitted and manager has no store, opens {@link defaultResultDbPath}. */
  readonly resultStore?: ResultStore;
  /** Optional Next.js request handler; when omitted, unknown paths return 404. */
  readonly nextHandler?: (req: IncomingMessage, res: ServerResponse) => void | Promise<void>;
}

export interface DashboardServer {
  readonly server: Server;
  readonly manager: ProcessManager;
  readonly resultStore: ResultStore | undefined;
  readonly port: number;
  readonly host: string;
  close(): Promise<void>;
}

/**
 * Starts the dashboard HTTP/WS server. When `nextHandler` is provided, non-API
 * traffic is delegated to Next.js.
 */
export async function startDashboardServer(options: DashboardServerOptions = {}): Promise<DashboardServer> {
  const host = options.host ?? "127.0.0.1";
  const port = options.port ?? 0;

  let ownedStore = false;
  let resultStore = options.resultStore ?? options.manager?.resultStore;
  if (resultStore === undefined && options.manager === undefined) {
    resultStore = new ResultStore(defaultResultDbPath());
    ownedStore = true;
  }

  const manager =
    options.manager ??
    new ProcessManager({
      resultStore,
    });

  // Prefer the manager's store when both exist so archive and query share one DB.
  const store = manager.resultStore ?? resultStore;
  const nextHandler = options.nextHandler;

  const server = createServer((req, res) => {
    void handleHttp(req, res, manager, store, nextHandler);
  });

  const wss = new WebSocketServer({ server, path: "/ws/runs" });
  const sockets = new Set<WebSocket>();

  wss.on("connection", (socket) => {
    sockets.add(socket);
    socket.send(JSON.stringify({ type: "status", snapshot: manager.getSnapshot() } satisfies RunManagerEvent));
    socket.on("close", () => sockets.delete(socket));
  });

  const onEvent = (event: RunManagerEvent) => {
    const payload = JSON.stringify(event);
    for (const socket of sockets) {
      if (socket.readyState === socket.OPEN) socket.send(payload);
    }
  };
  manager.on("event", onEvent);

  await new Promise<void>((resolve, reject) => {
    server.once("error", reject);
    server.listen(port, host, () => resolve());
  });

  const address = server.address();
  if (address === null || typeof address === "string") {
    throw new Error("Dashboard server failed to bind a TCP port.");
  }

  return {
    server,
    manager,
    resultStore: store,
    port: address.port,
    host,
    async close() {
      manager.off("event", onEvent);
      for (const socket of sockets) socket.close();
      wss.close();
      if (manager.isBusy()) {
        await manager.stop();
      }
      await new Promise<void>((resolve, reject) => {
        server.close((error) => (error ? reject(error) : resolve()));
      });
      if (ownedStore && store !== undefined) {
        store.close();
      }
    },
  };
}

async function handleHttp(
  req: IncomingMessage,
  res: ServerResponse,
  manager: ProcessManager,
  store: ResultStore | undefined,
  nextHandler: DashboardServerOptions["nextHandler"],
): Promise<void> {
  const url = new URL(req.url ?? "/", "http://localhost");
  try {
    if (req.method === "GET" && url.pathname === "/api/health") {
      sendJson(res, 200, { ok: true });
      return;
    }
    if (req.method === "GET" && url.pathname === "/api/runs/active") {
      sendJson(res, 200, manager.getSnapshot());
      return;
    }
    if (req.method === "POST" && url.pathname === "/api/runs/start") {
      const body = (await readJson(req)) as StartRunRequest;
      if (typeof body?.scenarioPath !== "string" || typeof body?.hostPath !== "string") {
        sendJson(res, 400, { error: "scenarioPath and hostPath are required." });
        return;
      }
      const runId = await manager.start(body);
      sendJson(res, 202, { runId, snapshot: manager.getSnapshot() });
      return;
    }
    if (req.method === "POST" && url.pathname === "/api/runs/stop") {
      const snapshot = await manager.stop();
      sendJson(res, 200, snapshot);
      return;
    }

    if (req.method === "GET" && url.pathname === "/api/runs") {
      if (store === undefined) {
        sendJson(res, 503, { error: "Result store is not configured." });
        return;
      }
      const limit = Number.parseInt(url.searchParams.get("limit") ?? "100", 10);
      sendJson(res, 200, { runs: store.listRuns(Number.isFinite(limit) ? limit : 100) });
      return;
    }

    const runDetailMatch = /^\/api\/runs\/([^/]+)$/.exec(url.pathname);
    if (req.method === "GET" && runDetailMatch !== null) {
      if (store === undefined) {
        sendJson(res, 503, { error: "Result store is not configured." });
        return;
      }
      const runId = decodeURIComponent(runDetailMatch[1]!);
      if (runId === "active") {
        sendJson(res, 200, manager.getSnapshot());
        return;
      }
      const detail = store.getRunDetail(runId);
      if (detail === undefined) {
        sendJson(res, 404, { error: `Run not found: ${runId}` });
        return;
      }
      sendJson(res, 200, detail);
      return;
    }

    const replayMatch = /^\/api\/runs\/([^/]+)\/replay$/.exec(url.pathname);
    if (req.method === "POST" && replayMatch !== null) {
      if (store === undefined) {
        sendJson(res, 503, { error: "Result store is not configured." });
        return;
      }
      const originalRunId = decodeURIComponent(replayMatch[1]!);
      const original = store.getRun(originalRunId);
      if (original === undefined) {
        sendJson(res, 404, { error: `Run not found: ${originalRunId}` });
        return;
      }
      if (original.scenarioPath === null || original.scenarioPath.length === 0) {
        sendJson(res, 400, { error: "Run has no scenario_path; cannot replay." });
        return;
      }

      const body = (await readJson(req)) as Partial<StartRunRequest>;
      const hostPath =
        typeof body.hostPath === "string" && body.hostPath.length > 0
          ? body.hostPath
          : original.config.hostPath;
      if (hostPath === undefined || hostPath.length === 0) {
        sendJson(res, 400, {
          error: "hostPath is required for replay when the original run did not store one.",
        });
        return;
      }

      const runId = await manager.start({
        scenarioPath: original.scenarioPath,
        hostPath,
        projectPath: body.projectPath ?? original.config.projectPath,
        importDir: body.importDir ?? original.config.importDir,
        seed: original.seed,
        maxSteps: original.config.maxSteps,
        maxDurationMs: original.config.maxDurationMs,
        livenessThresholdMs: original.config.livenessThresholdMs,
        startupTimeoutMs: original.config.startupTimeoutMs,
        reuseProject: body.reuseProject ?? original.config.reuseProject,
        outDir:
          body.outDir ??
          join("build", "tmp", "ui_fuzz_platform", `replay-${originalRunId}-${Date.now()}`),
        parentRunId: originalRunId,
      });
      sendJson(res, 202, {
        runId,
        parentRunId: originalRunId,
        seed: original.seed,
        snapshot: manager.getSnapshot(),
      });
      return;
    }

    if (nextHandler !== undefined) {
      await nextHandler(req, res);
      return;
    }

    sendJson(res, 404, { error: `No route for ${req.method} ${url.pathname}` });
  } catch (error) {
    const message = error instanceof Error ? error.message : String(error);
    sendJson(res, 500, { error: message });
  }
}

function sendJson(res: ServerResponse, status: number, body: unknown): void {
  const payload = JSON.stringify(body);
  res.writeHead(status, {
    "content-type": "application/json; charset=utf-8",
    "cache-control": "no-store",
  });
  res.end(payload);
}

async function readJson(req: IncomingMessage): Promise<unknown> {
  const chunks: Buffer[] = [];
  for await (const chunk of req) {
    chunks.push(typeof chunk === "string" ? Buffer.from(chunk) : chunk);
  }
  if (chunks.length === 0) return {};
  return JSON.parse(Buffer.concat(chunks).toString("utf8")) as unknown;
}
