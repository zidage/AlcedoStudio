#!/usr/bin/env node
//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

/**
 * Fake test host for runner integration tests. Implements the same JSON Lines
 * probe protocol as the C++ TestProbe over a real named pipe, with a small
 * simulated state machine so the acceptance scenario (open first image -> drag
 * exposure slider) can run end-to-end without the Qt shell.
 *
 * Run: node fake-host.mjs [--probe-socket <name>] [--project-path <dir>] [--import-dir <dir>] [--reuse-project]
 * Prints `PROBE_SOCKET=<name>` to stdout, serves the protocol, and emits ready +
 * heartbeats. Exits when the client disconnects and the run is over.
 */

import { createServer } from "node:net";

const PNG_1X1_BASE64 =
  "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mNk+M8AAAMBAQDJ/pLvAAAAAElFTkSuQmCC";

const MATCHERS = ["eq", "ne", "contains", "gt", "gte", "lt", "lte", "truthy"];

function parseArgs(argv) {
  const out = {};
  for (let i = 0; i < argv.length; i++) {
    const arg = argv[i];
    if (arg.startsWith("--")) {
      const key = arg.slice(2);
      out[key] = argv[++i];
    }
  }
  return out;
}

const args = parseArgs(process.argv.slice(2));
const socketName = args["probe-socket"] ?? `fake-host-${process.pid}-${Math.floor(Math.random() * 1e9)}`;
const pipePath = process.platform === "win32" ? `\\\\.\\pipe\\${socketName}` : socketName;

/** Simulated QML element properties. doubleClick on the first card opens the editor. */
const props = {
  workspaceHost: { visible: true },
  libraryNavButton: { visible: true },
  libraryWorkspace: { visible: true },
  thumbnailGridView_firstCard: { visible: true },
  editorWorkspace: { visible: false },
  editorSessionStatus: { text: "Running" },
  toneExposureSlider: { visible: true },
};

let heartbeatCounter = 0;

function send(socket, obj) {
  socket.write(JSON.stringify(obj) + "\n");
}

function okReply(socket, id) {
  send(socket, { id, ok: true, result: "ok" });
}

function errorReply(socket, id, code, message, extra = {}) {
  send(socket, { id, ok: false, error: { code, message, ...extra } });
}

function findMatcher(req) {
  for (const m of MATCHERS) {
    if (Object.prototype.hasOwnProperty.call(req, m)) return m;
  }
  return undefined;
}

function matcherHolds(matcher, actual, expected) {
  switch (matcher) {
    case "truthy":
      return actual === true || (typeof actual === "number" && actual !== 0) || (typeof actual === "string" && actual.length > 0);
    case "eq":
      return actual === expected;
    case "ne":
      return actual !== expected;
    case "contains":
      return typeof actual === "string" && typeof expected === "string" && actual.includes(expected);
    case "gt":
      return actual > expected;
    case "gte":
      return actual >= expected;
    case "lt":
      return actual < expected;
    case "lte":
      return actual <= expected;
    default:
      return false;
  }
}

function handleRequest(socket, req) {
  const id = req.id;
  const method = req.method;
  switch (method) {
    case "ping":
      send(socket, { id, ok: true, result: { status: "ok", heartbeat: heartbeatCounter, guiThread: true, ready: true } });
      return;
    case "snapshot":
      send(socket, {
        id,
        ok: true,
        result: {
          window: "Fake Host",
          elements: Object.entries(props).map(([name, p]) => ({ objectName: name, ...p })),
        },
      });
      return;
    case "find": {
      const target = req.target;
      if (props[target]) {
        send(socket, { id, ok: true, result: { found: true, element: { objectName: target, ...props[target] }, path: [target] } });
      } else {
        errorReply(socket, id, "target_not_found", `No live QML item matched '${target}'.`);
      }
      return;
    }
    case "read": {
      const target = req.target;
      const property = req.property;
      const value = props[target]?.[property];
      if (value === undefined) {
        errorReply(socket, id, "property_not_found", `Item '${target}' has no readable property '${property}'.`);
      } else {
        send(socket, { id, ok: true, result: { target, property, value } });
      }
      return;
    }
    case "click":
    case "doubleClick":
    case "rightClick":
      if (method === "doubleClick" && req.target === "thumbnailGridView_firstCard") {
        props.editorWorkspace.visible = true;
        props.editorSessionStatus.text = "Ready";
      }
      if (!props[req.target]) {
        errorReply(socket, id, "target_not_found", `No live QML item matched '${req.target}'.`);
        return;
      }
      okReply(socket, id);
      return;
    case "drag":
    case "key":
    case "typeText":
      okReply(socket, id);
      return;
    case "wait": {
      const target = req.target;
      const property = req.property;
      const matcher = findMatcher(req);
      const expected = matcher === "truthy" ? true : req[matcher];
      const value = props[target]?.[property];
      if (matcher === undefined) {
        errorReply(socket, id, "invalid_matcher", "wait requires one matcher such as eq, gte, or truthy.");
        return;
      }
      if (matcherHolds(matcher, value, expected)) {
        send(socket, { id, ok: true, result: "ok", actual: value });
      } else {
        errorReply(socket, id, "wait_timeout", `Timed out waiting for '${target}.${property}'.`, { actual: value, target, property });
      }
      return;
    }
    case "screenshot":
      send(socket, {
        id,
        ok: true,
        result: {
          format: "png",
          byteLength: Buffer.from(PNG_1X1_BASE64, "base64").length,
          width: 1,
          height: 1,
          pngBase64: PNG_1X1_BASE64,
        },
      });
      return;
    default:
      errorReply(socket, id, "unsupported_method", `Unsupported method '${method}'.`);
  }
}

const server = createServer((socket) => {
  socket.setEncoding("utf8");
  send(socket, { event: "ready", windowVisible: true, windowTitle: "Fake Host" });
  heartbeatCounter += 1;
  send(socket, { event: "heartbeat", counter: heartbeatCounter, guiTimeMs: 0, guiThread: true, ready: true });
  const heartbeat = setInterval(() => {
    heartbeatCounter += 1;
    send(socket, { event: "heartbeat", counter: heartbeatCounter, guiTimeMs: 0, guiThread: true, ready: true });
  }, 100);
  let buffer = "";
  socket.on("data", (chunk) => {
    buffer += chunk;
    let newlineIndex;
    while ((newlineIndex = buffer.indexOf("\n")) >= 0) {
      const line = buffer.slice(0, newlineIndex).trim();
      buffer = buffer.slice(newlineIndex + 1);
      if (line.length === 0) continue;
      try {
        handleRequest(socket, JSON.parse(line));
      } catch (error) {
        send(socket, { ok: false, error: { code: "invalid_json", message: String(error) } });
      }
    }
  });
  socket.on("close", () => clearInterval(heartbeat));
  socket.on("error", () => undefined);
});

server.listen(pipePath, () => {
  process.stdout.write(`PROBE_SOCKET=${socketName}\n`);
  process.stdout.write(`FAKE_HOST_LISTENING pid=${process.pid}\n`);
});

process.stdin.resume();