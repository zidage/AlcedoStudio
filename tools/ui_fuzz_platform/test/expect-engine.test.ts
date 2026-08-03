//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

import { describe, expect, it } from "vitest";

import { compileExpect, compileOp, DEFAULT_EXPECT_TIMEOUT_MS } from "../src/expect-engine.js";
import type { Expect, ScenarioDefaults } from "../src/scenario.js";

const noDefaults: ScenarioDefaults = {};

describe("compileExpect", () => {
  it("emits a wait request carrying the matcher key and expected value", () => {
    const spec: Expect = { target: "editorWorkspace", property: "visible", matcher: "eq", expected: true };
    const request = compileExpect(spec, noDefaults);
    expect(request.method).toBe("wait");
    expect(request.target).toBe("editorWorkspace");
    expect(request.property).toBe("visible");
    expect(request.eq).toBe(true);
    expect(request.timeoutMs).toBe(DEFAULT_EXPECT_TIMEOUT_MS);
  });

  it("omits the expected value for the truthy matcher", () => {
    const spec: Expect = { target: "panel", property: "visible", matcher: "truthy", expected: true };
    const request = compileExpect(spec, noDefaults);
    expect(request).not.toHaveProperty("truthy");
    expect(request.truthy).toBeUndefined();
  });

  it("carries a string expected value under the contains matcher", () => {
    const spec: Expect = { target: "status", property: "text", matcher: "contains", expected: "Ready" };
    const request = compileExpect(spec, noDefaults);
    expect(request.contains).toBe("Ready");
  });

  it("applies a per-expect timeoutMs over the scenario default", () => {
    const spec: Expect = { target: "x", property: "y", matcher: "eq", expected: 1, timeoutMs: 3000 };
    const request = compileExpect(spec, { expectTimeoutMs: 12000 });
    expect(request.timeoutMs).toBe(3000);
  });

  it("falls back to the scenario default when the expect omits timeoutMs", () => {
    const spec: Expect = { target: "x", property: "y", matcher: "eq", expected: 1 };
    const request = compileExpect(spec, { expectTimeoutMs: 12000 });
    expect(request.timeoutMs).toBe(12000);
  });

  it("falls back to the built-in default when neither expect nor scenario specify a timeout", () => {
    const spec: Expect = { target: "x", property: "y", matcher: "eq", expected: 1 };
    const request = compileExpect(spec, noDefaults);
    expect(request.timeoutMs).toBe(DEFAULT_EXPECT_TIMEOUT_MS);
  });
});

describe("compileOp", () => {
  it("builds a click request from a target", () => {
    const request = compileOp({ action: "click", target: "libraryNavButton" });
    expect(request?.method).toBe("click");
    expect(request?.target).toBe("libraryNavButton");
  });

  it("builds a doubleClick request", () => {
    const request = compileOp({ action: "doubleClick", target: "thumbnailGridView_firstCard" });
    expect(request?.method).toBe("doubleClick");
  });

  it("builds a drag request with normalized coordinates", () => {
    const request = compileOp({ action: "drag", target: "toneExposureSlider", fromNx: 0.2, toNx: 0.8, ny: 0.5, steps: 10 });
    expect(request?.method).toBe("drag");
    expect(request?.fromNx).toBe(0.2);
    expect(request?.toNx).toBe(0.8);
    expect(request?.steps).toBe(10);
  });

  it("builds a key request with modifiers", () => {
    const request = compileOp({ action: "key", key: 65, text: "a", ctrl: true, shift: false });
    expect(request?.method).toBe("key");
    expect(request?.key).toBe(65);
    expect(request?.text).toBe("a");
    expect(request?.ctrl).toBe(true);
    expect(request?.shift).toBeUndefined();
  });

  it("builds a typeText request", () => {
    const request = compileOp({ action: "typeText", text: "hello" });
    expect(request?.method).toBe("typeText");
    expect(request?.text).toBe("hello");
  });

  it("builds a wait request from a wait op", () => {
    const request = compileOp({ action: "wait", target: "panel", property: "count", matcher: "gte", expected: 1, timeoutMs: 5000 });
    expect(request?.method).toBe("wait");
    expect(request?.gte).toBe(1);
    expect(request?.timeoutMs).toBe(5000);
  });

  it("returns undefined for waitMs so the walker pauses without a probe round-trip", () => {
    const request = compileOp({ action: "waitMs", ms: 250 });
    expect(request).toBeUndefined();
  });

  it("throws when a click op lacks a target", () => {
    expect(() => compileOp({ action: "click" })).toThrow(/target/);
  });

  it("throws when a key op lacks a key code", () => {
    expect(() => compileOp({ action: "key" })).toThrow(/key/);
  });
});