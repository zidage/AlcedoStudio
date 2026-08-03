//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

/**
 * Public API surface of the UI fuzz runner core. The Next.js platform (Phase 3+)
 * imports from here; the CLI ({@link ./cli.js}) and tests do the same.
 */

export * from "./protocol.js";
export * from "./scenario.js";
export * from "./schema.js";
export * from "./loader.js";
export * from "./expect-engine.js";
export * from "./probe-client.js";
export * from "./host-process.js";
export * from "./liveness.js";
export * from "./walker.js";
export * from "./failure-bundle.js";
export * from "./run.js";