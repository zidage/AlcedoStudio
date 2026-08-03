//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

/**
 * Scenario loader: reads a YAML file from disk and hands the text to the pure
 * parser in {@link ./scenario-parse.js}, which validates it against the JSON
 * Schema and semantic rules and normalizes inline matcher keys into typed
 * {@link Scenario} fields with an insertion-ordered node map.
 */

import { readFile } from "node:fs/promises";

import { parseScenario, ScenarioError } from "./scenario-parse.js";
import type { Scenario } from "./scenario.js";

export { parseScenario, ScenarioError };

/**
 * Loads and validates a scenario file from disk.
 *
 * @throws {ScenarioError} when the YAML is malformed or validation fails.
 */
export async function loadScenario(path: string): Promise<Scenario> {
  const text = await readFile(path, "utf8");
  return parseScenario(text);
}
