//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

import { describe, expect, it } from "vitest";

import {
  diffCatalogAgainstSnapshot,
  runtimeNamesFromSnapshot,
} from "../src/catalog-staleness.js";
import { scanQmlSource } from "../src/qml-scanner.js";

const qml = [
  "Item {",
  '    objectName: "workspaceHost"',
  "    IconActionButton {",
  '        objectName: "libraryNavButton"',
  "    }",
  "    IconActionButton {",
  '        objectName: "retiredButton"',
  "    }",
  "    GridView {",
  "        delegate: Rectangle {",
  "            objectName: index === 0 ? \"thumbnailGridView_firstCard\"",
  "                                    : (\"thumbnailGridView_card_\" + index)",
  "        }",
  "    }",
  "    SlidingIconNav {",
  "        delegate: Item {",
  '            objectName: String(entry.itemObjectName || "")',
  "        }",
  "    }",
  "}",
].join("\n");

function snapshotWith(names: string[]): unknown {
  return { elements: names.map((objectName) => ({ objectName })) };
}

describe("Catalog staleness diffing", () => {
  it("StalenessMarksLiteralEntryMissingFromSnapshotAsStale", () => {
    const entries = scanQmlSource(qml, "fixture.qml");
    const report = diffCatalogAgainstSnapshot(
      entries,
      snapshotWith(["workspaceHost", "libraryNavButton", "thumbnailGridView_firstCard"]),
    );

    const byName = new Map(
      report.entries.map((item) => [item.entry.objectName ?? item.entry.expression, item]),
    );
    expect(byName.get("workspaceHost")!.status).toBe("present");
    expect(byName.get("libraryNavButton")!.status).toBe("present");
    expect(byName.get("retiredButton")!.status).toBe("stale");
    expect(report.stale).toBe(1);
  });

  it("StalenessMatchesDynamicPatternAgainstRuntimeElementNames", () => {
    const entries = scanQmlSource(qml, "fixture.qml");
    const report = diffCatalogAgainstSnapshot(
      entries,
      snapshotWith(["workspaceHost", "libraryNavButton", "retiredButton", "thumbnailGridView_card_12"]),
    );

    const ternary = report.entries.find((item) =>
      item.entry.candidates.includes("thumbnailGridView_firstCard"),
    );
    expect(ternary!.status).toBe("present");
    expect(ternary!.matchedBy).toBe("thumbnailGridView_card_12");
  });

  it("StalenessLeavesUnresolvableDynamicEntriesUnmarked", () => {
    const entries = scanQmlSource(qml, "fixture.qml");
    const report = diffCatalogAgainstSnapshot(entries, snapshotWith([]));

    const dynamic = report.entries.filter((item) => item.status === "dynamic");
    expect(dynamic.length).toBe(1);
    expect(dynamic[0]!.entry.expression).toContain("entry.itemObjectName");
    expect(report.stale).toBeGreaterThan(0);
    expect(report.unmatchedRuntimeNames).toEqual([]);
  });

  it("StalenessReportsRuntimeNamesWithoutCatalogEntries", () => {
    const entries = scanQmlSource(qml, "fixture.qml");
    const report = diffCatalogAgainstSnapshot(
      entries,
      snapshotWith(["workspaceHost", "brandNewRuntimeElement"]),
    );
    expect(report.unmatchedRuntimeNames).toEqual(["brandNewRuntimeElement"]);
  });

  it("StalenessExtractsNamesFromIdTestIdAndPathFields", () => {
    const snapshot = {
      elements: [
        { id: "byObjectName", objectName: "byObjectName" },
        { objectName: "", testId: "project.import" },
        { objectName: "", path: "mainWindow.workspaceHost.deepItem" },
      ],
    };
    const names = runtimeNamesFromSnapshot(snapshot);
    expect(names.has("byObjectName")).toBe(true);
    expect(names.has("project.import")).toBe(true);
    expect(names.has("mainWindow.workspaceHost.deepItem")).toBe(true);
    expect(runtimeNamesFromSnapshot({}).size).toBe(0);
    expect(runtimeNamesFromSnapshot(undefined).size).toBe(0);
  });
});
