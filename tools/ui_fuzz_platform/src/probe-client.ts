//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

/**
 * QLocalSocket JSON Lines client for the test-host TestProbe.
 *
 * Owns the socket connection, a monotonic request id counter, and a pending-reply
 * map keyed by id. Writes one JSON object per line; reads line-delimited JSON and
 * routes each message to either its awaiting request or an event handler. Unsolicited
 * events (`ready`, `heartbeat`, `fatal`) carry no `id` and never enter the pending map.
 *
 * On Windows the QLocalSocket is a named pipe at `\\.\pipe\<name>`; Node's `net`
 * module connects to that path directly. The client tolerates a short connect-retry
 * window because the host may still be initializing its pipe when the runner parses
 * `PROBE_SOCKET` from stdout.
 */

import { EventEmitter } from "node:events";
import { createConnection, type Socket } from "node:net";

import { isEvent, type ProbeEvent, type ProbeReply } from "./protocol.js";
import type { OutgoingRequest } from "./expect-engine.js";

/** Default per-request reply timeout when the caller does not override it. */
export const DEFAULT_REQUEST_TIMEOUT_MS = 30_000;

/** Fired when a request exceeds its reply timeout. */
export class ProbeTimeoutError extends Error {
  constructor(message: string) {
    super(message);
    this.name = "ProbeTimeoutError";
  }
}

interface PendingRequest {
  resolve: (reply: ProbeReply) => void;
  reject: (error: Error) => void;
  timer: NodeJS.Timeout;
}

/**
 * Events emitted by {@link ProbeClient}: `event` carries every unsolicited probe
 * event; `disconnect` fires when the socket closes; `error` fires on socket errors.
 */
export interface ProbeClientEvents {
  event: (event: ProbeEvent) => void;
  disconnect: () => void;
  error: (error: Error) => void;
}

/** Builds the named-pipe path the host listens on from its socket name. */
export function pipePath(socketName: string): string {
  return process.platform === "win32" ? `\\\\.\\pipe\\${socketName}` : socketName;
}

/** Connects to a probe socket name, retrying briefly while the host initializes. */
export async function connectProbe(socketName: string, timeoutMs = 10_000): Promise<ProbeClient> {
  const path = pipePath(socketName);
  const deadline = Date.now() + timeoutMs;
  let lastError: Error | undefined;
  while (Date.now() < deadline) {
    try {
      return await connectOnce(path);
    } catch (error) {
      lastError = error as Error;
      await sleep(50);
    }
  }
  throw new Error(
    `Could not connect to probe socket '${socketName}' within ${timeoutMs} ms: ${lastError?.message ?? "unknown error"}`,
  );
}

function sleep(ms: number): Promise<void> {
  const { promise, resolve } = Promise.withResolvers<void>();
  setTimeout(resolve, ms);
  return promise;
}

function connectOnce(path: string): Promise<ProbeClient> {
  const { promise, resolve, reject } = Promise.withResolvers<ProbeClient>();
  const socket = createConnection(path);
  let settled = false;
  const onConnect = () => {
    if (settled) return;
    settled = true;
    socket.removeListener("error", onError);
    resolve(new ProbeClient(socket));
  };
  const onError = (error: Error) => {
    if (settled) return;
    settled = true;
    socket.destroy();
    reject(error);
  };
  socket.once("connect", onConnect);
  socket.once("error", onError);
  return promise;
}

/**
 * Probe client owning one socket and the pending-reply map. Callers send requests
 * via {@link request} and subscribe to events via {@link on}.
 */
export class ProbeClient extends EventEmitter {
  private readonly socket: Socket;
  private readonly pending = new Map<number, PendingRequest>();
  private readonly buffer = { text: "" };
  private nextId = 1;
  private closed = false;

  constructor(socket: Socket) {
    super();
    this.socket = socket;
    this.socket.setEncoding("utf8");
    this.socket.on("data", (chunk: string) => this.handleData(chunk));
    this.socket.on("close", () => this.handleClose());
    this.socket.on("error", (error: Error) => this.emit("error", error));
  }

  /** Subscribes to a probe-client event. */
  override on<K extends keyof ProbeClientEvents>(event: K, listener: ProbeClientEvents[K]): this {
    return super.on(event, listener);
  }

  /** Sends a request and resolves with the matching reply (ok or error). */
  request(req: OutgoingRequest, timeoutMs = DEFAULT_REQUEST_TIMEOUT_MS): Promise<ProbeReply> {
    if (this.closed) {
      return Promise.reject(new Error("Probe client is closed."));
    }
    const id = this.nextId++;
    const line = JSON.stringify({ ...req, id }) + "\n";
    const { promise, resolve, reject } = Promise.withResolvers<ProbeReply>();
    const timer = setTimeout(() => {
      if (this.pending.delete(id)) {
        reject(new ProbeTimeoutError(`Request ${id} (${req.method}) timed out after ${timeoutMs} ms.`));
      }
    }, timeoutMs);
    this.pending.set(id, { resolve, reject, timer });
    if (!this.socket.write(line)) {
      this.socket.once("drain", () => undefined);
    }
    return promise;
  }

  /** Closes the socket and rejects all pending requests. */
  close(): void {
    if (this.closed) return;
    this.closed = true;
    for (const [id, pending] of this.pending) {
      clearTimeout(pending.timer);
      pending.reject(new Error("Probe client closed."));
      this.pending.delete(id);
    }
    this.socket.destroy();
  }

  private handleData(chunk: string): void {
    this.buffer.text += chunk;
    let newlineIndex: number;
    while ((newlineIndex = this.buffer.text.indexOf("\n")) >= 0) {
      const line = this.buffer.text.slice(0, newlineIndex).trim();
      this.buffer.text = this.buffer.text.slice(newlineIndex + 1);
      if (line.length === 0) continue;
      this.handleLine(line);
    }
  }

  private handleLine(line: string): void {
    let message: unknown;
    try {
      message = JSON.parse(line);
    } catch {
      return;
    }
    if (typeof message !== "object" || message === null) return;
    if (isEvent(message)) {
      this.emit("event", message);
      return;
    }
    const reply = message as ProbeReply;
    const id = typeof reply.id === "number" ? reply.id : undefined;
    if (id === undefined) return;
    const pending = this.pending.get(id);
    if (pending === undefined) return;
    clearTimeout(pending.timer);
    this.pending.delete(id);
    pending.resolve(reply);
  }

  private handleClose(): void {
    this.closed = true;
    for (const [, pending] of this.pending) {
      clearTimeout(pending.timer);
      pending.reject(new Error("Probe socket closed before the reply arrived."));
    }
    this.pending.clear();
    this.emit("disconnect");
  }
}