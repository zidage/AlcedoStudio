//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

/**
 * Default filesystem locations for the UI fuzz platform data directory and
 * SQLite result store. Override with `UI_FUZZ_DATA_DIR` / `UI_FUZZ_DB`.
 */

import { join, resolve } from "node:path";
import { fileURLToPath } from "node:url";

/** Absolute path to `tools/ui_fuzz_platform`. */
export function platformRootDir(): string {
  return resolve(fileURLToPath(new URL("..", import.meta.url)));
}

/**
 * Platform data directory. Defaults to `<platform>/data` (gitignored) so the
 * SQLite file lives next to the tool rather than under the repo root.
 */
export function platformDataDir(): string {
  if (process.env.UI_FUZZ_DATA_DIR !== undefined && process.env.UI_FUZZ_DATA_DIR.length > 0) {
    return resolve(process.env.UI_FUZZ_DATA_DIR);
  }
  return join(platformRootDir(), "data");
}

/** Default SQLite path: `<data>/results.sqlite`. */
export function defaultResultDbPath(): string {
  if (process.env.UI_FUZZ_DB !== undefined && process.env.UI_FUZZ_DB.length > 0) {
    return resolve(process.env.UI_FUZZ_DB);
  }
  return join(platformDataDir(), "results.sqlite");
}

/** Default workflow directory: `<platform>/scenarios` (editor-saved YAML lives here). */
export function defaultWorkflowsDir(): string {
  if (process.env.UI_FUZZ_WORKFLOWS_DIR !== undefined && process.env.UI_FUZZ_WORKFLOWS_DIR.length > 0) {
    return resolve(process.env.UI_FUZZ_WORKFLOWS_DIR);
  }
  return join(platformRootDir(), "scenarios");
}

/**
 * Default QML source root scanned for the element catalog:
 * `<repo>/alcedo_studio/src/ui/alcedo_main/qml`.
 */
export function defaultQmlRootDir(): string {
  if (process.env.UI_FUZZ_QML_ROOT !== undefined && process.env.UI_FUZZ_QML_ROOT.length > 0) {
    return resolve(process.env.UI_FUZZ_QML_ROOT);
  }
  return resolve(platformRootDir(), "..", "..", "alcedo_studio", "src", "ui", "alcedo_main", "qml");
}

/** Path of the generated catalog artifact: `<data>/catalog.json`. */
export function defaultCatalogPath(): string {
  return join(platformDataDir(), "catalog.json");
}
