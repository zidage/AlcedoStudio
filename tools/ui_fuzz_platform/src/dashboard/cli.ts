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
  const rootDir = join(dirname(fileURLToPath(import.meta.url)), "..", "..");

  const app = next({ dev, hostname: host, port, dir: rootDir });
  await app.prepare();
  const handle = app.getRequestHandler();

  const dashboard = await startDashboardServer({
    host,
    port,
    nextHandler: (req, res) => handle(req, res),
  });

  process.stdout.write(`UI fuzz dashboard listening on http://${dashboard.host}:${dashboard.port}\n`);
  process.stdout.write(`WebSocket stream: ws://${dashboard.host}:${dashboard.port}/ws/runs\n`);

  const shutdown = async () => {
    await dashboard.close();
    process.exit(0);
  };
  process.on("SIGINT", () => void shutdown());
  process.on("SIGTERM", () => void shutdown());
}

main().catch((error: Error) => {
  process.stderr.write(`dashboard: ${error.message}\n`);
  process.exit(1);
});
