//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

import { fileURLToPath } from "node:url";

import yaml from "js-yaml";
import { describe, expect, it } from "vitest";

import {
  FlowGraphError,
  expectNodeId,
  flowToScenario,
  flowToYamlText,
  scenarioToFlow,
  scenarioToYamlText,
  type FlowGraphModel,
} from "../src/flow-graph.js";
import { loadScenario, parseScenario } from "../src/loader.js";
import { validateScenario } from "../src/schema.js";
import type { Scenario } from "../src/scenario.js";

const acceptanceScenarioPath = fileURLToPath(
  new URL("../scenarios/library_to_editor_exposure.yaml", import.meta.url),
);
const wrongExpectScenarioPath = fileURLToPath(
  new URL("../scenarios/wrong_expect_fails.yaml", import.meta.url),
);

function roundTrip(scenario: Scenario): Scenario {
  return flowToScenario(scenarioToFlow(scenario));
}

describe("Flow graph round-trip", () => {
  it("FlowRoundTripPreservesHandAuthoredScenarioSemantics", async () => {
    for (const path of [acceptanceScenarioPath, wrongExpectScenarioPath]) {
      const scenario = await loadScenario(path);
      expect(roundTrip(scenario)).toEqual(scenario);
    }
  });

  it("FlowRoundTripKeepsExpectAndNextEdgeDeclarationOrder", async () => {
    const scenario = await loadScenario(acceptanceScenarioPath);
    const restored = roundTrip(scenario);

    expect([...restored.nodes.keys()]).toEqual([...scenario.nodes.keys()]);
    expect(restored.nodes.get("open_first_image")!.next).toEqual([
      { to: "drag_exposure_slider", weight: 2 },
      { to: "back_to_library", weight: 1 },
    ]);
    expect(restored.nodes.get("workspace_ready")!.expect).toEqual(
      scenario.nodes.get("workspace_ready")!.expect,
    );
    expect(restored.start).toBe(scenario.start);
    expect(restored.defaults).toEqual(scenario.defaults);
  });

  it("FlowSerializedYamlValidatesAgainstScenarioSchema", async () => {
    const scenario = await loadScenario(acceptanceScenarioPath);
    const yamlText = scenarioToYamlText(scenario);
    expect(validateScenario(parseAsRaw(yamlText)).valid).toBe(true);
    expect(parseScenario(yamlText)).toEqual(scenario);
  });

  it("EditorAssembledGraphSerializesToHandAuthoredEquivalentYaml", async () => {
    // Simulate the editor path: load a hand-authored file onto the canvas,
    // then serialize the canvas state back out without any manual YAML edits.
    const handAuthored = await loadScenario(acceptanceScenarioPath);
    const canvas = scenarioToFlow(handAuthored);
    const yamlText = flowToYamlText(canvas);
    expect(parseScenario(yamlText)).toEqual(handAuthored);
  });

  it("EditorBuiltGraphWithoutLoadIndexesAppendsInCreationOrder", () => {
    const graph: FlowGraphModel = {
      name: "built_from_scratch",
      startNodeId: "first",
      defaults: { expectTimeoutMs: 5000 },
      nodes: [
        {
          kind: "op",
          id: "first",
          position: { x: 0, y: 0 },
          op: { action: "click", target: "libraryNavButton" },
        },
        {
          kind: "expect",
          id: expectNodeId("first", 0),
          owner: "first",
          position: { x: 300, y: 110 },
          expect: { target: "libraryWorkspace", property: "visible", matcher: "eq", expected: true },
        },
        {
          kind: "op",
          id: "second",
          position: { x: 320, y: 0 },
          op: { action: "doubleClick", target: "thumbnailGridView_firstCard" },
        },
      ],
      edges: [
        {
          kind: "expect",
          id: "first=>first/expect/0",
          from: "first",
          to: expectNodeId("first", 0),
        },
        { kind: "next", id: "first->second", from: "first", to: "second", weight: 3 },
      ],
    };

    const scenario = flowToScenario(graph);
    expect(scenario.nodes.get("first")!.expect).toEqual([
      {
        target: "libraryWorkspace",
        property: "visible",
        matcher: "eq",
        expected: true,
        timeoutMs: 5000,
      },
    ]);
    expect(scenario.nodes.get("first")!.next).toEqual([{ to: "second", weight: 3 }]);
    expect(scenario.nodes.get("second")!.next).toBeUndefined();

    const yamlText = scenarioToYamlText(scenario);
    const reparsed = parseScenario(yamlText);
    expect(reparsed).toEqual(scenario);
    // The expect had no explicit timeout; the scenario default must apply on parse.
    expect(reparsed.nodes.get("first")!.expect![0]!.timeoutMs).toBe(5000);
  });

  it("FlowSerializationRejectsGraphWithDanglingSuccessor", async () => {
    const canvas = scenarioToFlow(await loadScenario(acceptanceScenarioPath));
    const broken: FlowGraphModel = {
      ...canvas,
      edges: [
        ...canvas.edges,
        { kind: "next", id: "back_to_library->ghost#0", from: "back_to_library", to: "ghost", weight: 1 },
      ],
    };
    expect(() => flowToScenario(broken)).toThrow(FlowGraphError);
    expect(() => flowToScenario(broken)).toThrow(/ghost/);
  });

  it("FlowSerializationRejectsExpectNodeWithUnknownOwner", () => {
    const graph: FlowGraphModel = {
      name: "orphan_expect",
      startNodeId: "only",
      defaults: {},
      nodes: [
        { kind: "op", id: "only", position: { x: 0, y: 0 }, op: { action: "waitMs", ms: 10 } },
        {
          kind: "expect",
          id: expectNodeId("ghost", 0),
          owner: "ghost",
          position: { x: 300, y: 110 },
          expect: { target: "x", property: "visible", matcher: "truthy", expected: true },
        },
      ],
      edges: [],
    };
    expect(() => flowToScenario(graph)).toThrow(FlowGraphError);
  });
});

function parseAsRaw(yamlText: string): unknown {
  // validateScenario operates on the raw parsed shape, same as the loader.
  return yaml.load(yamlText);
}
