//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

import type { ActiveRunSnapshot, StartRunFormValues } from "./types";

export async function fetchActiveRun(): Promise<ActiveRunSnapshot> {
  const response = await fetch("/api/runs/active", { cache: "no-store" });
  if (!response.ok) {
    throw new Error(`Failed to load active run (${response.status})`);
  }
  return (await response.json()) as ActiveRunSnapshot;
}

export async function startRun(values: StartRunFormValues): Promise<{ runId: string }> {
  const response = await fetch("/api/runs/start", {
    method: "POST",
    headers: { "content-type": "application/json" },
    body: JSON.stringify(values),
  });
  const body = (await response.json()) as { runId?: string; error?: string };
  if (!response.ok) {
    throw new Error(body.error ?? `Start failed (${response.status})`);
  }
  return { runId: body.runId! };
}

export async function stopRun(): Promise<ActiveRunSnapshot> {
  const response = await fetch("/api/runs/stop", { method: "POST" });
  const body = (await response.json()) as ActiveRunSnapshot & { error?: string };
  if (!response.ok) {
    throw new Error(body.error ?? `Stop failed (${response.status})`);
  }
  return body;
}

/** Builds the browser WebSocket URL for the live run event stream. */
export function runsWebSocketUrl(): string {
  if (typeof window === "undefined") return "ws://127.0.0.1/ws/runs";
  const protocol = window.location.protocol === "https:" ? "wss:" : "ws:";
  return `${protocol}//${window.location.host}/ws/runs`;
}
