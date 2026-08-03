//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

/**
 * Catalog staleness diffing. Compares the static QML catalog against a live
 * probe `snapshot` result and marks each entry:
 *
 * - `present` — a literal candidate or the derived regex pattern matched a
 *   runtime element name (`id`, `objectName`, `testId`, or dot path);
 * - `stale` — the binding had resolvable candidates but none matched anything
 *   in the live tree;
 * - `dynamic` — the binding is unresolvable statically (no literal candidates,
 *   e.g. `String(entry.itemObjectName || "")`), so runtime presence cannot be
 *   decided and the entry is never marked stale.
 *
 * The runner never consults the catalog at execution time; staleness is an
 * authoring aid surfaced on the catalog page.
 */

import type { CatalogEntry } from "./qml-scanner.js";

export type StalenessStatus = "present" | "stale" | "dynamic";

/** One catalog entry annotated with its runtime presence verdict. */
export interface StalenessEntry {
  readonly entry: CatalogEntry;
  readonly status: StalenessStatus;
  /** The runtime name that satisfied the entry, when present. */
  readonly matchedBy: string | null;
}

export interface StalenessReport {
  readonly entries: readonly StalenessEntry[];
  readonly present: number;
  readonly stale: number;
  readonly dynamic: number;
  /** Runtime element names that no catalog entry accounted for. */
  readonly unmatchedRuntimeNames: readonly string[];
}

/**
 * Extracts the set of matchable names from a probe snapshot result. Accepts
 * the raw `result` object of a `snapshot` reply (`{ elements: [...] }`).
 */
export function runtimeNamesFromSnapshot(snapshot: unknown): Set<string> {
  const names = new Set<string>();
  if (typeof snapshot !== "object" || snapshot === null) return names;
  const elements = (snapshot as { elements?: unknown }).elements;
  if (!Array.isArray(elements)) return names;
  for (const element of elements) {
    if (typeof element !== "object" || element === null) continue;
    const record = element as Record<string, unknown>;
    for (const key of ["id", "objectName", "testId", "path"] as const) {
      const value = record[key];
      if (typeof value === "string" && value.length > 0) {
        names.add(value);
      }
    }
  }
  return names;
}

/** Diffs the catalog against the runtime names collected from a live snapshot. */
export function diffCatalog(entries: readonly CatalogEntry[], runtimeNames: ReadonlySet<string>): StalenessReport {
  const claimed = new Set<string>();
  const result: StalenessEntry[] = [];

  for (const entry of entries) {
    const matchedBy = matchEntry(entry, runtimeNames);
    if (matchedBy !== null) {
      claimed.add(matchedBy);
      result.push({ entry, status: "present", matchedBy });
      continue;
    }
    if (entry.candidates.length === 0 && entry.pattern === null) {
      result.push({ entry, status: "dynamic", matchedBy: null });
    } else {
      result.push({ entry, status: "stale", matchedBy: null });
    }
  }

  const unmatchedRuntimeNames = [...runtimeNames].filter((name) => !claimed.has(name)).sort();

  let present = 0;
  let stale = 0;
  let dynamic = 0;
  for (const item of result) {
    if (item.status === "present") present++;
    else if (item.status === "stale") stale++;
    else dynamic++;
  }

  return { entries: result, present, stale, dynamic, unmatchedRuntimeNames };
}

/** Convenience wrapper: diff against the raw probe snapshot result object. */
export function diffCatalogAgainstSnapshot(
  entries: readonly CatalogEntry[],
  snapshot: unknown,
): StalenessReport {
  return diffCatalog(entries, runtimeNamesFromSnapshot(snapshot));
}

function matchEntry(entry: CatalogEntry, runtimeNames: ReadonlySet<string>): string | null {
  for (const candidate of entry.candidates) {
    if (runtimeNames.has(candidate)) return candidate;
  }
  if (entry.pattern !== null) {
    const regex = new RegExp(entry.pattern);
    for (const name of runtimeNames) {
      if (regex.test(name)) return name;
    }
  }
  return null;
}
