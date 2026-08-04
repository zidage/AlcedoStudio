//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

import { join } from "node:path";
import { writeFile, mkdir } from "node:fs/promises";

import { describe, expect, it } from "vitest";

import {
  inferOpKinds,
  scanQmlDirectory,
  scanQmlSource,
  type CatalogEntry,
} from "../src/qml-scanner.js";
import { defaultQmlRootDir } from "../src/paths.js";
import { makeTempDir } from "./helpers/fixtures.js";

function singleEntry(source: string): CatalogEntry {
  const entries = scanQmlSource(source, "fixture.qml");
  expect(entries.length).toBe(1);
  return entries[0]!;
}

describe("QML scanner", () => {
  it("ScannerExtractsLiteralObjectNameWithComponentContext", () => {
    const entry = singleEntry(
      [
        "Item {",
        "    id: root",
        "    IconActionButton {",
        '        objectName: "libraryNavButton"',
        "    }",
        "}",
      ].join("\n"),
    );
    expect(entry.objectName).toBe("libraryNavButton");
    expect(entry.candidates).toEqual(["libraryNavButton"]);
    expect(entry.dynamic).toBe(false);
    expect(entry.pattern).toBeNull();
    expect(entry.component).toBe("IconActionButton");
    expect(entry.source).toBe("fixture.qml");
    expect(entry.line).toBe(4);
    expect(entry.opKinds).toEqual(["click", "wait"]);
  });

  it("ScannerExpandsMultilineTernaryIntoLiteralCandidates", () => {
    const entry = singleEntry(
      [
        "GridView {",
        "    delegate: Rectangle {",
        "        objectName: index === 0 ? \"thumbnailGridView_firstCard\"",
        "                                : (\"thumbnailGridView_card_\" + index)",
        "    }",
        "}",
      ].join("\n"),
    );
    expect(entry.dynamic).toBe(true);
    expect(entry.objectName).toBeNull();
    expect(entry.candidates).toEqual(["thumbnailGridView_firstCard", "thumbnailGridView_card_"]);
    expect(entry.pattern).toBe("^(?:thumbnailGridView_card_.*)$");
    expect(entry.component).toBe("Rectangle");
  });

  it("ScannerDerivesRegexPatternForConcatenatedObjectName", () => {
    const entry = singleEntry(
      [
        "ListView {",
        "    delegate: Item {",
        '        objectName: "filmstripTile_" + imageId',
        "    }",
        "}",
      ].join("\n"),
    );
    expect(entry.dynamic).toBe(true);
    expect(entry.candidates).toEqual(["filmstripTile_"]);
    expect(entry.pattern).toBe("^(?:filmstripTile_.*)$");
  });

  it("ScannerMarksUnresolvableBindingAsDynamicWithoutCandidates", () => {
    const entry = singleEntry(
      [
        "SlidingIconNav {",
        "    delegate: Item {",
        '        objectName: String(entry.itemObjectName || "")',
        "    }",
        "}",
      ].join("\n"),
    );
    expect(entry.dynamic).toBe(true);
    expect(entry.objectName).toBeNull();
    expect(entry.candidates).toEqual([]);
    expect(entry.pattern).toBeNull();
  });

  it("ScannerInfersOpKindsFromComponentType", () => {
    expect(inferOpKinds("AdjustmentSlider")).toEqual(["click", "drag", "wait"]);
    expect(inferOpKinds("AdjustmentField")).toEqual(["click", "typeText", "key", "wait"]);
    expect(inferOpKinds("SearchComboBox")).toEqual(["click", "typeText", "key", "wait"]);
    expect(inferOpKinds("IconActionButton")).toEqual(["click", "wait"]);
    expect(inferOpKinds("AdjustmentCombo")).toEqual(["click", "wait"]);
    expect(inferOpKinds("MouseArea")).toEqual(["click", "doubleClick", "rightClick", "wait"]);
    expect(inferOpKinds("GridView")).toEqual(["click", "doubleClick", "wait"]);
    expect(inferOpKinds("Rectangle")).toEqual(["wait"]);
  });

  it("ScannerScopesEntriesToTheirEnclosingComponent", () => {
    const entries = scanQmlSource(
      [
        "Item {",
        '    objectName: "outer"',
        "    Column {",
        "        AdjustmentSlider {",
        '            objectName: "toneExposureSlider"',
        "        }",
        "    }",
        '    objectName: "ignoredSiblingContext"',
        "}",
      ].join("\n"),
      "panel.qml",
    );
    expect(entries.map((entry) => entry.objectName)).toEqual([
      "outer",
      "toneExposureSlider",
      "ignoredSiblingContext",
    ]);
    expect(entries[0]!.component).toBe("Item");
    expect(entries[1]!.component).toBe("AdjustmentSlider");
    expect(entries[2]!.component).toBe("Item");
  });

  it("ScannerIgnoresCommentsAndBracesInsideStrings", () => {
    const entries = scanQmlSource(
      [
        "Item {",
        '    // objectName: "commentedOut"',
        '    property string note: "braces { } in string"',
        '    objectName: "realEntry" // trailing comment',
        "    /*",
        '       objectName: "blockCommented"',
        "    */",
        "}",
      ].join("\n"),
      "comments.qml",
    );
    expect(entries.map((entry) => entry.objectName)).toEqual(["realEntry"]);
  });

  it("ScannerFindsEntriesAcrossNestedQmlDirectories", async () => {
    const root = makeTempDir();
    await mkdir(join(root, "nested"), { recursive: true });
    await writeFile(join(root, "Alpha.qml"), 'Item {\n    objectName: "alpha"\n}\n');
    await writeFile(join(root, "nested", "Beta.qml"), 'Button {\n    objectName: "beta"\n}\n');
    await writeFile(join(root, "nested", "notes.txt"), "not qml");

    const catalog = await scanQmlDirectory(root);
    expect(catalog.filesScanned).toBe(2);
    expect(catalog.entries.map((entry) => entry.objectName)).toEqual(["alpha", "beta"]);
    expect(catalog.entries.map((entry) => entry.source)).toEqual(["Alpha.qml", "nested/Beta.qml"]);
  });

  it("ScannerFindsAutomationTargetsAcrossRealAlcedoQmlTree", async () => {
    const catalog = await scanQmlDirectory(defaultQmlRootDir());
    expect(catalog.filesScanned).toBeGreaterThan(70);
    expect(catalog.entries.length).toBeGreaterThan(200);

    const byName = new Map(catalog.entries.map((entry) => [entry.objectName, entry]));
    // Targets the acceptance scenario and the Phase 0/1 probe tests rely on.
    for (const name of [
      "workspaceHost",
      "libraryNavButton",
      "libraryWorkspace",
      "editorWorkspace",
      "toneExposureSlider",
      "mainWindow",
    ]) {
      expect(byName.has(name), `expected catalog entry for ${name}`).toBe(true);
    }
    expect(byName.get("toneExposureSlider")!.opKinds).toEqual(["click", "drag", "wait"]);
    expect(byName.get("libraryNavButton")!.opKinds).toContain("click");

    // The delegate ternary must surface both the stable first-card id and the card pattern.
    const firstCard = catalog.entries.find((entry) =>
      entry.candidates.includes("thumbnailGridView_firstCard"),
    );
    expect(firstCard).toBeDefined();
    expect(firstCard!.dynamic).toBe(true);
    expect(firstCard!.pattern).toBe("^(?:thumbnailGridView_card_.*)$");
  });
});
