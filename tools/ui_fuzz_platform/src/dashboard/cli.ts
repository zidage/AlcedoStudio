#!/usr/bin/env node
//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

/**
 * Dashboard entry: Next.js UI (Ant Design Pro components) + HTTP/WS API that
 * owns the {@link ProcessManager}. Usage: `pnpm dashboard` or `pnpm dev`.
 */

import { createRequire } from "node:module";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

import { ProcessManager } from "../process-manager.js";
import { defaultResultDbPath } from "../paths.js";
import { ResultStore } from "../result-store.js";
import { startDashboardServer } from "./http-server.js";

const require = createRequire(import.meta.url);
const next = require("next") as (options: { dev: boolean; hostname: string; port: number; dir: string }) => {
  prepare: () => Promise<void>;
  getRequestHandler: () => (req: import("node:http").IncomingMessage, res: import("node:http").ServerResponse) => Promise<void>;
};

async function main(): Promise<void> {
  const host = process.env.HOST ?? "127.0.0.1";
  const port = Number.parseInt(process.env.PORT ?? "3030", 10);
  const dev = process.env.NODE_ENV !== "production";
  // Default to development so next.config.mjs picks the dev-only distDir;
  // `next build` forces NODE_ENV=production itself.
  process.env.NODE_ENV = process.env.NODE_ENV ?? "development";
  const rootDir = join(dirname(fileURLToPath(import.meta.url)), "..", "..");
  const dbPath = defaultResultDbPath();
  const resultStore = new ResultStore(dbPath);
  const manager = new ProcessManager({ resultStore });

  const app = next({ dev, hostname: host, port, dir: rootDir });
  await app.prepare();
  const handle = app.getRequestHandler();

  const dashboard = await startDashboardServer({
    host,
    port,
    manager,
    resultStore,
    nextHandler: (req, res) => handle(req, res),
  });

  process.stdout.write(`UI fuzz dashboard listening on http://${dashboard.host}:${dashboard.port}\n`);
  process.stdout.write(`WebSocket stream: ws://${dashboard.host}:${dashboard.port}/ws/runs\n`);
  process.stdout.write(`Result store: ${dbPath}\n`);
  process.stdout.write(`Results browser: http://${dashboard.host}:${dashboard.port}/runs\n`);

  const shutdown = async () => {
    await dashboard.close();
    resultStore.close();
    process.exit(0);
  };
  process.on("SIGINT", () => void shutdown());
  process.on("SIGTERM", () => void shutdown());
}

main().catch((error: Error) => {
  process.stderr.write(`dashboard: ${error.message}\n`);
  process.exit(1);
});
