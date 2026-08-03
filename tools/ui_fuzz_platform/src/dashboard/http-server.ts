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
 * - GET  /api/runs/active/snapshot -> live probe snapshot (409 when idle)
 * - POST /api/runs/start           -> { runId }
 * - POST /api/runs/stop            -> ActiveRunSnapshot
 * - GET  /api/runs                 -> StoredRun[]
 * - GET  /api/runs/:id             -> StoredRunDetail
 * - POST /api/runs/:id/replay      -> { runId } (starts managed replay)
 * - GET  /api/workflows            -> WorkflowSummary[]
 * - GET  /api/workflows/:name      -> WorkflowDocument (raw YAML)
 * - PUT  /api/workflows/:name      -> validates + saves editor YAML (400 on errors)
 * - GET  /api/catalog              -> QmlCatalog (rescans the QML root)
 * - GET  /api/catalog/staleness    -> StalenessReport vs the live run (409 when idle)
 * - POST /api/catalog/staleness    -> StalenessReport vs a posted probe snapshot
 * - WS   /ws/runs                  -> RunManagerEvent JSON frames
 */

import { createServer, type IncomingMessage, type Server, type ServerResponse } from "node:http";
import { mkdir, writeFile } from "node:fs/promises";
import { dirname, join } from "node:path";
import { WebSocketServer, type WebSocket } from "ws";

import { diffCatalogAgainstSnapshot, type StalenessReport } from "../catalog-staleness.js";
import { ProcessManager } from "../process-manager.js";
import {
  defaultCatalogPath,
  defaultQmlRootDir,
  defaultResultDbPath,
  defaultWorkflowsDir,
} from "../paths.js";
import { scanQmlDirectory, type QmlCatalog } from "../qml-scanner.js";
import { ScenarioError } from "../scenario-parse.js";
import { ResultStore } from "../result-store.js";
import type { RunManagerEvent, StartRunRequest } from "../run-events.js";
import { WorkflowStore } from "../workflow-store.js";

export interface DashboardServerOptions {
  readonly host?: string;
  readonly port?: number;
  readonly manager?: ProcessManager;
  /** When omitted and manager has no store, opens {@link defaultResultDbPath}. */
  readonly resultStore?: ResultStore;
  /** Workflow YAML directory; defaults to {@link defaultWorkflowsDir}. */
  readonly workflowsDir?: string;
  /** QML source root for the catalog scanner; defaults to {@link defaultQmlRootDir}. */
  readonly qmlRootDir?: string;
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
  const workflowStore = new WorkflowStore(options.workflowsDir ?? defaultWorkflowsDir());
  const qmlRootDir = options.qmlRootDir ?? defaultQmlRootDir();
  const catalogPath = defaultCatalogPath();

  const server = createServer((req, res) => {
    void handleHttp(req, res, manager, store, workflowStore, qmlRootDir, catalogPath, nextHandler);
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
  workflowStore: WorkflowStore,
  qmlRootDir: string,
  catalogPath: string,
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

    if (req.method === "GET" && url.pathname === "/api/runs/active/snapshot") {
      try {
        const snapshot = await manager.captureLiveSnapshot();
        sendJson(res, 200, { snapshot });
      } catch (error) {
        sendJson(res, 409, { error: (error as Error).message });
      }
      return;
    }

    if (req.method === "GET" && url.pathname === "/api/workflows") {
      sendJson(res, 200, { workflows: await workflowStore.list() });
      return;
    }

    const workflowMatch = /^\/api\/workflows\/([^/]+)$/.exec(url.pathname);
    if (workflowMatch !== null) {
      const name = decodeURIComponent(workflowMatch[1]!);
      if (req.method === "GET") {
        try {
          sendJson(res, 200, await workflowStore.read(name));
        } catch (error) {
          const code = (error as NodeJS.ErrnoException).code === "ENOENT" ? 404 : 400;
          sendJson(res, code, { error: (error as Error).message });
        }
        return;
      }
      if (req.method === "PUT") {
        const body = (await readJson(req)) as { yaml?: unknown };
        if (typeof body?.yaml !== "string" || body.yaml.trim().length === 0) {
          sendJson(res, 400, { error: "Request body must carry the scenario YAML text in 'yaml'." });
          return;
        }
        try {
          const document = await workflowStore.save(name, body.yaml);
          sendJson(res, 200, { ...document, validation: { valid: true, errors: [] } });
        } catch (error) {
          if (error instanceof ScenarioError) {
            sendJson(res, 400, { error: error.message, validation: { valid: false } });
          } else {
            sendJson(res, 400, { error: (error as Error).message });
          }
        }
        return;
      }
    }

    if (req.method === "GET" && url.pathname === "/api/catalog") {
      const catalog = await scanAndPersistCatalog(qmlRootDir, catalogPath);
      sendJson(res, 200, catalog);
      return;
    }

    if (req.method === "GET" && url.pathname === "/api/catalog/staleness") {
      try {
        const snapshot = await manager.captureLiveSnapshot();
        const catalog = await scanAndPersistCatalog(qmlRootDir, catalogPath);
        sendJson(res, 200, stalenessPayload(catalog, snapshot));
      } catch (error) {
        sendJson(res, 409, { error: (error as Error).message });
      }
      return;
    }

    if (req.method === "POST" && url.pathname === "/api/catalog/staleness") {
      const body = (await readJson(req)) as { snapshot?: unknown };
      if (body?.snapshot === undefined) {
        sendJson(res, 400, { error: "Request body must carry a probe snapshot result in 'snapshot'." });
        return;
      }
      const catalog = await scanAndPersistCatalog(qmlRootDir, catalogPath);
      sendJson(res, 200, stalenessPayload(catalog, body.snapshot));
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

/** Rescans the QML root and persists the generated catalog artifact. */
async function scanAndPersistCatalog(qmlRootDir: string, catalogPath: string): Promise<QmlCatalog> {
  const catalog = await scanQmlDirectory(qmlRootDir);
  await mkdir(dirname(catalogPath), { recursive: true });
  await writeFile(catalogPath, JSON.stringify(catalog, null, 2), "utf8");
  return catalog;
}

function stalenessPayload(catalog: QmlCatalog, snapshot: unknown): {
  catalogRoot: string;
  generatedAt: number;
  report: StalenessReport;
} {
  return {
    catalogRoot: catalog.root,
    generatedAt: catalog.generatedAt,
    report: diffCatalogAgainstSnapshot(catalog.entries, snapshot),
  };
}

async function readJson(req: IncomingMessage): Promise<unknown> {
  const chunks: Buffer[] = [];
  for await (const chunk of req) {
    chunks.push(typeof chunk === "string" ? Buffer.from(chunk) : chunk);
  }
  if (chunks.length === 0) return {};
  return JSON.parse(Buffer.concat(chunks).toString("utf8")) as unknown;
}
