//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

/** Shared fixtures for UI fuzz platform tests: temp directories and scenario builders. */

import { mkdtempSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";

import { pipePath } from "../../src/probe-client.js";

/** Creates a unique temp directory under the OS temp dir (keeps the repo clean). */
export function makeTempDir(prefix = "fuzz-test-"): string {
  return mkdtempSync(join(tmpdir(), prefix));
}

/** A unique pipe name for a test server/client pair. */
export function uniqueSocketName(): string {
  return `fuzz-test-${process.pid}-${Math.floor(Math.random() * 1e9)}`;
}

/** The named-pipe path for a test socket name (platform-aware). */
export function testPipePath(socketName: string): string {
  return pipePath(socketName);
}

/** The hand-written acceptance scenario YAML: open project -> import -> open first image -> drag exposure slider. */
export const ACCEPTANCE_SCENARIO_YAML = `name: library_to_editor_exposure
start: workspace_ready
defaults:
  expectTimeoutMs: 8000
nodes:
  workspace_ready:
    op: { action: wait, target: workspaceHost, property: visible, eq: true }
    expect:
      - { target: workspaceHost, property: visible, eq: true }
    next:
      - { to: open_first_image, weight: 1 }
  open_first_image:
    op: { action: doubleClick, target: thumbnailGridView_firstCard }
    expect:
      - { target: editorWorkspace, property: visible, eq: true }
    next:
      - { to: switch_to_tone, weight: 2 }
      - { to: back_to_library, weight: 1 }
  switch_to_tone:
    op: { action: click, target: editorAdjustmentNav_tone }
    expect:
      - { target: editorAdjustmentPanel_tone, property: visible, eq: true }
    next:
      - { to: drag_exposure_slider, weight: 1 }
  drag_exposure_slider:
    op: { action: drag, target: toneExposureSlider, fromNx: 0.2, toNx: 0.8, steps: 10 }
    expect:
      - { target: toneExposureSlider, property: visible, eq: true }
    next: []
  back_to_library:
    op: { action: click, target: libraryNavButton }
    expect:
      - { target: libraryWorkspace, property: visible, eq: true }
`;

/** A scenario whose final expect can never hold (the host produces "Ready", not "Finished"),
 *  used to prove failure-bundle capture on a correctness verdict. */
export const WRONG_EXPECT_SCENARIO_YAML = `name: wrong_expect_fails
start: step_one
defaults:
  expectTimeoutMs: 500
nodes:
  step_one:
    op: { action: doubleClick, target: thumbnailGridView_firstCard }
    expect:
      - { target: editorSessionStatus, property: text, eq: "Finished", timeoutMs: 400 }
    next: []
`;

/** A minimal 1x1 transparent PNG used by the fake host's screenshot response. */
export const PNG_1X1_BASE64 =
  "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mNk+M8AAAMBAQDJ/pLvAAAAAElFTkSuQmCC";
