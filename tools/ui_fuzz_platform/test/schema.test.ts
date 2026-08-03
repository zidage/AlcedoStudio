//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

import { describe, expect, it } from "vitest";

import { validateScenario } from "../src/schema.js";

/** A minimal valid raw scenario used as the base for negative variations. */
function validScenario(): Record<string, unknown> {
  return {
    name: "linear",
    start: "a",
    defaults: { expectTimeoutMs: 8000 },
    nodes: {
      a: {
        op: { action: "click", target: "btn" },
        expect: [{ target: "panel", property: "visible", eq: true }],
        next: [{ to: "b", weight: 1 }],
      },
      b: {
        op: { action: "wait", target: "panel", property: "count", gte: 1 },
        next: [],
      },
    },
  };
}

describe("validateScenario", () => {
  it("accepts a well-formed linear scenario", () => {
    const result = validateScenario(validScenario());
    expect(result.valid).toBe(true);
    expect(result.errors).toEqual([]);
  });

  it("rejects a scenario missing the name field", () => {
    const raw = validScenario();
    delete raw.name;
    const result = validateScenario(raw);
    expect(result.valid).toBe(false);
    expect(result.errors.some((e) => e.includes("name"))).toBe(true);
  });

  it("rejects a scenario whose start references an unknown node", () => {
    const raw = validScenario();
    raw.start = "missing";
    const result = validateScenario(raw);
    expect(result.valid).toBe(false);
    expect(result.errors.some((e) => e.includes("'start'") && e.includes("missing"))).toBe(true);
  });

  it("rejects a next edge pointing at an unknown node", () => {
    const raw = validScenario();
    (raw.nodes as Record<string, unknown>).a = {
      op: { action: "click", target: "btn" },
      next: [{ to: "nowhere", weight: 1 }],
    };
    const result = validateScenario(raw);
    expect(result.valid).toBe(false);
    expect(result.errors.some((e) => e.includes("nowhere"))).toBe(true);
  });

  it("rejects an unknown op action", () => {
    const raw = validScenario();
    (raw.nodes as Record<string, unknown>).a = { op: { action: "poke", target: "btn" } };
    const result = validateScenario(raw);
    expect(result.valid).toBe(false);
  });

  it("rejects an expect without any matcher", () => {
    const raw = validScenario();
    (raw.nodes as Record<string, unknown>).a = {
      op: { action: "click", target: "btn" },
      expect: [{ target: "panel", property: "visible" }],
    };
    const result = validateScenario(raw);
    expect(result.valid).toBe(false);
  });

  it("rejects an expect carrying two matchers", () => {
    const raw = validScenario();
    (raw.nodes as Record<string, unknown>).a = {
      op: { action: "click", target: "btn" },
      expect: [{ target: "panel", property: "visible", eq: true, gte: 1 }],
    };
    const result = validateScenario(raw);
    expect(result.valid).toBe(false);
    expect(result.errors.some((e) => e.includes("2 matchers"))).toBe(true);
  });

  it("rejects a wait op without a matcher", () => {
    const raw = validScenario();
    (raw.nodes as Record<string, unknown>).a = { op: { action: "wait", target: "panel", property: "visible" } };
    const result = validateScenario(raw);
    expect(result.valid).toBe(false);
    expect(result.errors.some((e) => e.includes("wait") && e.includes("matcher"))).toBe(true);
  });

  it("accepts every supported matcher key on an expect", () => {
    for (const matcher of ["eq", "ne", "contains", "gt", "gte", "lt", "lte", "truthy"]) {
      const raw = validScenario();
      (raw.nodes as Record<string, unknown>).a = {
        op: { action: "click", target: "btn" },
        expect: [{ target: "panel", property: "visible", [matcher]: matcher === "contains" ? "x" : 1 }],
      };
      expect(validateScenario(raw).valid, `matcher ${matcher}`).toBe(true);
    }
  });

  it("rejects an unknown top-level property", () => {
    const raw = validScenario();
    (raw as Record<string, unknown>).bogus = true;
    const result = validateScenario(raw);
    expect(result.valid).toBe(false);
  });

  it("rejects a non-number edge weight", () => {
    const raw = validScenario();
    (raw.nodes as Record<string, unknown>).a = {
      op: { action: "click", target: "btn" },
      next: [{ to: "b", weight: "heavy" }],
    };
    const result = validateScenario(raw);
    expect(result.valid).toBe(false);
  });
});