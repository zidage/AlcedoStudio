//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

import { describe, expect, it } from "vitest";

import { parseScenario, ScenarioError } from "../src/loader.js";
import { ACCEPTANCE_SCENARIO_YAML } from "./helpers/fixtures.js";

describe("parseScenario", () => {
  it("loads and normalizes the acceptance scenario into a typed Scenario", () => {
    const scenario = parseScenario(ACCEPTANCE_SCENARIO_YAML);
    expect(scenario.name).toBe("library_to_editor_exposure");
    expect(scenario.start).toBe("workspace_ready");
    expect(scenario.defaults.expectTimeoutMs).toBe(8000);
    expect([...scenario.nodes.keys()]).toEqual([
      "workspace_ready",
      "open_first_image",
      "drag_exposure_slider",
      "back_to_library",
    ]);
  });

  it("flattens an inline eq matcher into Expect.matcher and Expect.expected", () => {
    const scenario = parseScenario(`
name: t
start: a
nodes:
  a:
    op: { action: click, target: btn }
    expect:
      - { target: panel, property: visible, eq: true }
    next: []
`);
    const node = scenario.nodes.get("a")!;
    expect(node.expect?.[0]?.matcher).toBe("eq");
    expect(node.expect?.[0]?.expected).toBe(true);
  });

  it("flattens a truthy matcher to expected true", () => {
    const scenario = parseScenario(`
name: t
start: a
nodes:
  a:
    op: { action: click, target: btn }
    expect:
      - { target: panel, property: visible, truthy: true }
    next: []
`);
    const node = scenario.nodes.get("a")!;
    expect(node.expect?.[0]?.matcher).toBe("truthy");
    expect(node.expect?.[0]?.expected).toBe(true);
  });

  it("applies the scenario default expectTimeoutMs when an expect omits timeoutMs", () => {
    const scenario = parseScenario(`
name: t
start: a
defaults:
  expectTimeoutMs: 12000
nodes:
  a:
    op: { action: click, target: btn }
    expect:
      - { target: panel, property: visible, eq: true }
    next: []
`);
    expect(scenario.nodes.get("a")!.expect?.[0]?.timeoutMs).toBe(12000);
  });

  it("keeps a per-expect timeoutMs override over the scenario default", () => {
    const scenario = parseScenario(`
name: t
start: a
defaults:
  expectTimeoutMs: 12000
nodes:
  a:
    op: { action: click, target: btn }
    expect:
      - { target: panel, property: visible, eq: true, timeoutMs: 3000 }
    next: []
`);
    expect(scenario.nodes.get("a")!.expect?.[0]?.timeoutMs).toBe(3000);
  });

  it("preserves drag normalized-coordinate fields on the op", () => {
    const scenario = parseScenario(`
name: t
start: a
nodes:
  a:
    op: { action: drag, target: slider, fromNx: 0.2, toNx: 0.8, ny: 0.5, steps: 10 }
    next: []
`);
    const op = scenario.nodes.get("a")!.op;
    expect(op.action).toBe("drag");
    expect(op.fromNx).toBe(0.2);
    expect(op.toNx).toBe(0.8);
    expect(op.steps).toBe(10);
  });

  it("throws ScenarioError on malformed YAML", () => {
    expect(() => parseScenario("name: t\n  bad: : :")).toThrow(ScenarioError);
  });

  it("throws ScenarioError listing validation errors for an unknown start node", () => {
    expect(() => parseScenario("name: t\nstart: gone\nnodes:\n  a:\n    op: { action: click, target: b }\n    next: []\n")).toThrow(
      /unknown node 'gone'/,
    );
  });
});