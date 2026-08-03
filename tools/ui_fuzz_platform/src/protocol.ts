//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

/**
 * JSON Lines wire-protocol types for the Alcedo test-host TestProbe IPC channel.
 *
 * The transport is one JSON object per line over QLocalSocket (a Windows named
 * pipe on Windows, a Unix domain socket elsewhere). Requests carry an `id` that
 * the probe echoes on the matching reply. The probe also emits unsolicited
 * events (`ready`, `heartbeat`, `fatal`) that carry no `id`.
 *
 * These types mirror the C++ probe handlers in
 * `alcedo_studio/src/ui/alcedo_studio_test_host/`. They are the single source of
 * truth for the runner-side client: every field name here must match the probe.
 */

/** Matcher operators accepted by the probe `wait` method. */
export type Matcher = "eq" | "ne" | "contains" | "gt" | "gte" | "lt" | "lte" | "truthy";

export const MATCHERS: readonly Matcher[] = [
  "eq",
  "ne",
  "contains",
  "gt",
  "gte",
  "lt",
  "lte",
  "truthy",
] as const;

/** Probe method names supported in v1. */
export type Method =
  | "snapshot"
  | "find"
  | "read"
  | "click"
  | "doubleClick"
  | "rightClick"
  | "key"
  | "typeText"
  | "drag"
  | "wait"
  | "screenshot"
  | "ping";

/** A JSON Lines request. `id` is assigned by the runner and echoed in the reply. */
export interface ProbeRequest {
  readonly id: number;
  readonly method: Method;
  [key: string]: unknown;
}

/** Error payload carried by a failed reply. */
export interface ProbeError {
  readonly code: string;
  readonly message: string;
  readonly [key: string]: unknown;
}

/** A probe reply. `id` is present when the request carried one. */
export interface ProbeReply {
  readonly id?: number;
  readonly ok: boolean;
  readonly result?: unknown;
  readonly error?: ProbeError;
  /** Present on a successful `wait` reply: the actual matched value. */
  readonly actual?: unknown;
}

/** Unsolicited probe event (no `id`). */
export interface ProbeEvent {
  readonly event: "ready" | "heartbeat" | "fatal";
  readonly [key: string]: unknown;
}

/** A message read off the wire: either a reply (has `ok`) or an event (has `event`). */
export type WireMessage = ProbeReply | ProbeEvent;

/** Returns true when a wire message is an unsolicited event rather than a reply. */
export function isEvent(message: unknown): message is ProbeEvent {
  return typeof (message as ProbeEvent).event === "string";
}

/** Builds a `ping` request. */
export function ping(id: number): ProbeRequest {
  return { id, method: "ping" };
}

/** Builds a `snapshot` request. */
export function snapshot(id: number): ProbeRequest {
  return { id, method: "snapshot" };
}

/** Builds a `find` request. */
export function find(id: number, target: string): ProbeRequest {
  return { id, method: "find", target };
}

/** Builds a `read` request. `property` may be omitted to use the `target.property` shorthand. */
export function read(id: number, target: string, property?: string): ProbeRequest {
  return property === undefined
    ? { id, method: "read", target }
    : { id, method: "read", target, property };
}

/** Builds a `click`, `doubleClick`, or `rightClick` request. */
export function click(
  id: number,
  target: string,
  kind: "click" | "doubleClick" | "rightClick" = "click",
): ProbeRequest {
  return { id, method: kind, target };
}

/** Builds a `key` request. `key` is a Qt::Key integer; `text` is the generated character. */
export function key(
  id: number,
  keyCode: number,
  options?: { text?: string; ctrl?: boolean; shift?: boolean; alt?: boolean },
): ProbeRequest {
  const request: ProbeRequest = { id, method: "key", key: keyCode };
  if (options?.text !== undefined) request.text = options.text;
  if (options?.ctrl) request.ctrl = true;
  if (options?.shift) request.shift = true;
  if (options?.alt) request.alt = true;
  return request;
}

/** Builds a `typeText` request. */
export function typeText(id: number, text: string): ProbeRequest {
  return { id, method: "typeText", text };
}

/** Builds a `drag` request along a target's horizontal axis in normalized [0,1] coordinates. */
export function drag(
  id: number,
  target: string,
  options?: { fromNx?: number; toNx?: number; ny?: number; steps?: number },
): ProbeRequest {
  const request: ProbeRequest = { id, method: "drag", target };
  if (options?.fromNx !== undefined) request.fromNx = options.fromNx;
  if (options?.toNx !== undefined) request.toNx = options.toNx;
  if (options?.ny !== undefined) request.ny = options.ny;
  if (options?.steps !== undefined) request.steps = options.steps;
  return request;
}

/** Builds a `screenshot` request. */
export function screenshot(id: number): ProbeRequest {
  return { id, method: "screenshot" };
}

/** A compiled `wait` request: one matcher key with its expected value plus a timeout. */
export interface WaitRequest {
  readonly id: number;
  readonly method: "wait";
  readonly target: string;
  readonly property: string;
  readonly timeoutMs: number;
  [matcher: string]: unknown;
}

/**
 * Builds a `wait` request from a normalized matcher.
 *
 * `truthy` carries no expected value; every other matcher carries its expected
 * value under the matcher's own key, exactly as the probe's ParseMatcher expects.
 */
export function wait(
  id: number,
  target: string,
  property: string,
  matcher: Matcher,
  expected: unknown,
  timeoutMs: number,
): WaitRequest {
  const request: WaitRequest = {
    id,
    method: "wait",
    target,
    property,
    timeoutMs,
  };
  if (matcher !== "truthy") request[matcher] = expected;
  return request;
}

/** Result shape returned by a successful `screenshot` request. */
export interface ScreenshotResult {
  readonly format: "png";
  readonly byteLength: number;
  readonly width: number;
  readonly height: number;
  readonly pngBase64: string;
}

/** Returns the `screenshot` result when present on a successful reply. */
export function screenshotResult(reply: ProbeReply): ScreenshotResult | undefined {
  if (!reply.ok) return undefined;
  return reply.result as ScreenshotResult | undefined;
}

/**
 * Returns the `actual` value carried by a `wait` reply. On success it lives at the
 * top level; on `wait_timeout` it lives inside `error`. The location differs by
 * outcome, so callers should use this helper rather than reaching into either spot.
 */
export function waitActual(reply: ProbeReply): unknown {
  return reply.ok ? reply.actual : reply.error?.actual;
}