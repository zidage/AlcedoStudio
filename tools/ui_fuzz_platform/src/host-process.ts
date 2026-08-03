//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

/**
 * Test-host process supervisor. Spawns the `alcedo_studio_test_host` executable,
 * parses the `PROBE_SOCKET=<name>` line from its stdout, and streams stdout/stderr
 * into a bounded log buffer. Qt logs never transit the probe; they are captured
 * here directly from the child pipes, which is also what makes the log tail
 * available for the failure bundle.
 */

import { spawn, type ChildProcess } from "node:child_process";
import { EventEmitter } from "node:events";

/** Options for spawning a test host. */
export interface HostSpawnOptions {
  readonly hostPath: string;
  readonly projectPath?: string;
  readonly importDir?: string;
  readonly probeSocket?: string;
  readonly reuseProject?: boolean;
  readonly extraArgs?: readonly string[];
  /** Full argv override; when set, replaces `hostPath` (e.g. a launcher or wrapper). */
  readonly command?: readonly string[];
  readonly env?: NodeJS.ProcessEnv;
  /** Maximum wait for the `PROBE_SOCKET=` line on stdout before giving up. */
  readonly startupTimeoutMs?: number;
}

/** A live, supervised test host plus its captured log buffer. */
export interface HostHandle {
  readonly child: ChildProcess;
  probeSocket: string;
  readonly logs: LogBuffer;
  /** Kills the child process. */
  kill(): void;
  /** Resolves with the child exit code when the process ends. */
  exited(): Promise<number | null>;
}

/** Fired by {@link LogBuffer}: `line` for each captured log line, `end` on EOF. */
export interface LogBufferEvents {
  line: (line: string, stream: "stdout" | "stderr") => void;
  end: () => void;
}

/**
 * Bounded log buffer. Keeps every line for live streaming and exposes the last
 * `maxKib` KiB as a tail for failure bundles.
 */
export class LogBuffer extends EventEmitter {
  private readonly lines: string[] = [];
  private totalBytes = 0;
  private closed = false;

  /** Appends one line (already stripped of its trailing newline). */
  append(line: string, stream: "stdout" | "stderr"): void {
    if (this.closed) return;
    this.lines.push(line);
    this.totalBytes += line.length + 1;
    this.emit("line", line, stream);
  }

  /** Returns the last `kib` KiB of captured output as a single string. */
  tail(kib: number): string {
    const maxBytes = kib * 1024;
    if (this.totalBytes <= maxBytes) return this.lines.join("\n");
    const kept: string[] = [];
    let bytes = 0;
    for (let index = this.lines.length - 1; index >= 0; index--) {
      const line = this.lines[index]!;
      bytes += line.length + 1;
      kept.unshift(line);
      if (bytes >= maxBytes) break;
    }
    return kept.join("\n");
  }

  /** Returns the full captured output joined by newlines. */
  text(): string {
    return this.lines.join("\n");
  }

  get byteLength(): number {
    return this.totalBytes;
  }

  close(): void {
    this.closed = true;
    this.emit("end");
  }
}

/** Spawns the test host and resolves once `PROBE_SOCKET=` appears on stdout. */
export function spawnHost(options: HostSpawnOptions): Promise<HostHandle> {
  const args: string[] = [];
  if (options.projectPath !== undefined) args.push("--project-path", options.projectPath);
  if (options.importDir !== undefined) args.push("--import-dir", options.importDir);
  if (options.probeSocket !== undefined) args.push("--probe-socket", options.probeSocket);
  if (options.reuseProject) args.push("--reuse-project");
  args.push(...(options.extraArgs ?? []));

  const command = options.command ?? [options.hostPath];
  const child = spawn(command[0]!, [...command.slice(1), ...args], {
    env: { ...process.env, ...options.env },
    stdio: ["ignore", "pipe", "pipe"],
  });
  const logs = new LogBuffer();

  let exitCode: number | null = null;
  const { promise: exitPromise, resolve: exitResolve } = Promise.withResolvers<number | null>();
  const handle: HostHandle = {
    child,
    probeSocket: "",
    logs,
    kill: () => child.kill(),
    exited: () => exitPromise,
  };

  const { promise, resolve, reject } = Promise.withResolvers<HostHandle>();
  const startupTimeoutMs = options.startupTimeoutMs ?? 120_000;
  let settled = false;

  const startupTimer = setTimeout(() => {
    if (settled) return;
    settled = true;
    child.kill();
    reject(new Error(`Test host did not print PROBE_SOCKET within ${startupTimeoutMs} ms.`));
  }, startupTimeoutMs);

  let probeSocket: string | undefined;
  let stdoutBuffer = "";

  child.stdout?.setEncoding("utf8");
  child.stdout?.on("data", (chunk: string) => {
    stdoutBuffer += chunk;
    let newlineIndex: number;
    while ((newlineIndex = stdoutBuffer.indexOf("\n")) >= 0) {
      const line = stdoutBuffer.slice(0, newlineIndex).replace(/\r$/, "");
      stdoutBuffer = stdoutBuffer.slice(newlineIndex + 1);
      if (line.length > 0) logs.append(line, "stdout");
      if (probeSocket === undefined) {
        const match = /^PROBE_SOCKET=(.+)$/.exec(line);
        if (match !== null) {
          probeSocket = match[1]!.trim();
          handle.probeSocket = probeSocket;
          if (!settled) {
            settled = true;
            clearTimeout(startupTimer);
            resolve(handle);
          }
        }
      }
    }
  });

  child.stderr?.setEncoding("utf8");
  child.stderr?.on("data", (chunk: string) => {
    let remaining = chunk;
    let newlineIndex: number;
    while ((newlineIndex = remaining.indexOf("\n")) >= 0) {
      const line = remaining.slice(0, newlineIndex).replace(/\r$/, "");
      remaining = remaining.slice(newlineIndex + 1);
      if (line.length > 0) logs.append(line, "stderr");
    }
    if (remaining.length > 0) logs.append(remaining, "stderr");
  });

  child.on("exit", (code, signal) => {
    exitCode = code ?? (signal ? -1 : 0);
    logs.close();
    exitResolve(exitCode);
    if (!settled) {
      settled = true;
      clearTimeout(startupTimer);
      reject(new Error(`Test host exited before printing PROBE_SOCKET (code=${code}, signal=${signal}).`));
    }
  });

  return promise;
}