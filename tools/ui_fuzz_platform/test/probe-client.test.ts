//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

import { createServer, type Server, type Socket } from "node:net";

import { afterEach, describe, expect, it } from "vitest";

import { connectProbe, ProbeTimeoutError, type ProbeClient } from "../src/probe-client.js";
import { testPipePath, uniqueSocketName } from "./helpers/fixtures.js";

/** Starts a JSON Lines server on a unique pipe name. `onLine` receives each request line and a `reply` helper. */
function startServer(
  socketName: string,
  onLine: (line: string, reply: (obj: unknown) => void, socket: Socket) => void,
): Server {
  const server = createServer((socket) => {
    socket.setEncoding("utf8");
    let buffer = "";
    socket.on("data", (chunk: string) => {
      buffer += chunk;
      let newlineIndex: number;
      while ((newlineIndex = buffer.indexOf("\n")) >= 0) {
        const line = buffer.slice(0, newlineIndex).trim();
        buffer = buffer.slice(newlineIndex + 1);
        if (line.length === 0) continue;
        onLine(line, (obj) => socket.write(JSON.stringify(obj) + "\n"), socket);
      }
    });
  });
  server.listen(testPipePath(socketName));
  return server;
}

const servers: Server[] = [];

afterEach(async () => {
  await Promise.all(servers.splice(0).map((server) => closeServer(server)));
});

function closeServer(server: Server): Promise<void> {
  return new Promise((resolve) => server.close(() => resolve()));
}

describe("ProbeClient", () => {
  it("round-trips a request and reply matched by id", async () => {
    const socketName = uniqueSocketName();
    const server = startServer(socketName, (line, reply) => {
      const request = JSON.parse(line);
      reply({ id: request.id, ok: true, result: "ok" });
    });
    servers.push(server);

    const client = await connectProbe(socketName, 5000);
    const reply = await client.request({ method: "ping" });
    expect(reply.ok).toBe(true);
    expect(reply.result).toBe("ok");
    client.close();
  });

  it("routes unsolicited events to the event handler instead of the pending map", async () => {
    const socketName = uniqueSocketName();
    const server = startServer(socketName, (_line, reply, socket) => {
      socket.write(JSON.stringify({ event: "heartbeat", counter: 1 }) + "\n");
      reply({ id: 1, ok: true, result: "ok" });
    });
    servers.push(server);

    const client = await connectProbe(socketName, 5000);
    const events: unknown[] = [];
    client.on("event", (event) => events.push(event));
    const reply = await client.request({ method: "ping" });
    expect(reply.ok).toBe(true);
    expect(events.some((e) => e != null && typeof e === "object" && "event" in e && e.event === "heartbeat")).toBe(true);
    client.close();
  });

  it("reassembles a reply split across two packets", async () => {
    const socketName = uniqueSocketName();
    const server = startServer(socketName, (line, _reply, socket) => {
      const request = JSON.parse(line);
      const payload = JSON.stringify({ id: request.id, ok: true, result: "ok" }) + "\n";
      socket.write(payload.slice(0, 8));
      socket.write(payload.slice(8));
    });
    servers.push(server);

    const client = await connectProbe(socketName, 5000);
    const reply = await client.request({ method: "ping" });
    expect(reply.ok).toBe(true);
    client.close();
  });

  it("rejects with ProbeTimeoutError when no reply arrives in time", async () => {
    const socketName = uniqueSocketName();
    const server = startServer(socketName, () => {
      // Intentionally never reply.
    });
    servers.push(server);

    const client = await connectProbe(socketName, 5000);
    await expect(client.request({ method: "ping" }, 200)).rejects.toBeInstanceOf(ProbeTimeoutError);
    client.close();
  });

  it("connectProbe retries until the server appears", async () => {
    const socketName = uniqueSocketName();
    // Defer server start to the next tick so connectProbe's first connect attempt
    // fails; connectProbe then retries on its internal 50 ms cadence and connects.
    // Deterministic fake timers will not work here because the retry interleaves real
    // net I/O with the sleep, so the genuine retry cadence is exercised instead.
    setImmediate(() => servers.push(startServer(socketName, (line, reply) => reply({ id: JSON.parse(line).id, ok: true }))));

    const client = await connectProbe(socketName, 5000);
    const reply = await client.request({ method: "ping" });
    expect(reply.ok).toBe(true);
    client.close();
  });
});