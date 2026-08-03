//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

/**
 * Normalized scenario domain types. The loader parses a YAML file, validates it
 * against {@link ./schema.js SCENARIO_SCHEMA}, and returns a {@link Scenario} with
 * inline matcher keys flattened into {@link Expect}/{@link Op} fields and `nodes`
 * stored in an insertion-ordered `Map` so edge and node ordering is deterministic.
 *
 * The value types (Op/Expect/Edge/ScenarioNode) are mutable during construction in
 * the loader; {@link Scenario} is the immutable snapshot handed to the walker.
 */

import type { Matcher } from "./protocol.js";

/** Operation actions supported in v1 scenario nodes. */
export type Action =
  | "click"
  | "rightClick"
  | "doubleClick"
  | "key"
  | "typeText"
  | "drag"
  | "wait"
  | "waitMs"
  | "screenshot";

/** Scenario-wide defaults applied to expects without an explicit timeout. */
export interface ScenarioDefaults {
  expectTimeoutMs?: number;
}

/**
 * A node operation. `wait` carries a matcher/expected pair (compiled to a probe
 * `wait`); `waitMs` carries `ms` (a runner-side pause with no probe round-trip);
 * `screenshot` marks a key node whose capture is recorded in the artifact bundle.
 */
export interface Op {
  action: Action;
  target?: string;
  property?: string;
  matcher?: Matcher;
  expected?: unknown;
  timeoutMs?: number;
  text?: string;
  key?: number;
  ctrl?: boolean;
  shift?: boolean;
  alt?: boolean;
  fromNx?: number;
  toNx?: number;
  ny?: number;
  steps?: number;
  ms?: number;
  /// Click/drag readiness retry budget (ms). The probe waits up to this long
  /// for the target to transition from disabled/invisible to clickable before
  /// returning target_disabled. Defaults to 5000 on the probe side.
  readyTimeoutMs?: number;
}

/** A post-op assertion compiled to a probe `wait` call. */
export interface Expect {
  target: string;
  property: string;
  matcher: Matcher;
  expected: unknown;
  timeoutMs?: number;
}

/** A weighted edge to a successor node. Phase 2 walks edges in declaration order. */
export interface Edge {
  to: string;
  weight: number;
}

/** A scenario node: one operation, optional post-op expects, optional successor edges. */
export interface ScenarioNode {
  op: Op;
  expect?: Expect[];
  next?: Edge[];
}

/**
 * A validated scenario. `nodes` is an insertion-ordered map keyed by node id so
 * that "first edge in declaration order" and node enumeration stay deterministic.
 */
export interface Scenario {
  readonly name: string;
  readonly start: string;
  readonly defaults: ScenarioDefaults;
  readonly nodes: ReadonlyMap<string, ScenarioNode>;
}
/** Run configuration supplied alongside a scenario. */
export interface RunConfig {
  seed: number;
  maxSteps: number;
  maxDurationMs: number;
  livenessThresholdMs: number;
  startupTimeoutMs: number;
  hostPath?: string;
  hostCommand?: readonly string[];
  projectPath?: string;
  importDir?: string;
  reuseProject: boolean;
  outDir: string;
}

/** Defaults applied when the CLI omits run configuration. */
export const DEFAULT_RUN_CONFIG = {
  seed: 0,
  maxSteps: 1000,
  maxDurationMs: 300_000,
  livenessThresholdMs: 5000,
  startupTimeoutMs: 120_000,
  reuseProject: false,
} as const satisfies Partial<RunConfig>;
