//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

/**
 * Phase 5 acceptance: a workflow assembled on the flow-editor canvas saves to
 * schema-valid YAML identical in semantics to the hand-authored acceptance
 * scenario, and the runner executes the saved file without any manual edits.
 */

import { writeFile } from "node:fs/promises";
import { join } from "node:path";
import { fileURLToPath } from "node:url";

import { describe, expect, it } from "vitest";

import { flowToYamlText, scenarioToFlow } from "../src/flow-graph.js";
import { loadScenario, parseScenario } from "../src/loader.js";
import { runScenario } from "../src/run.js";
import { DEFAULT_RUN_CONFIG } from "../src/scenario.js";
import { makeTempDir } from "./helpers/fixtures.js";

const fakeHostPath = fileURLToPath(new URL("./helpers/fake-host.mjs", import.meta.url));
const acceptanceScenarioPath = fileURLToPath(
  new URL("../scenarios/library_to_editor_exposure.yaml", import.meta.url),
);

describe("Editor-assembled workflow acceptance", () => {
  it("EditorAssembledWorkflowRunsUneditedThroughRunner", async () => {
    // Assemble on the "canvas": load the hand-authored scenario into the
    // editor graph, then serialize straight back out — the exact path the
    // /workflows/[name] page takes on save.
    const handAuthored = await loadScenario(acceptanceScenarioPath);
    const editorYaml = flowToYamlText(scenarioToFlow(handAuthored));

    // Saved YAML is schema-valid and semantically identical to the source.
    const savedScenario = parseScenario(editorYaml);
    expect(savedScenario).toEqual(handAuthored);

    // Written to a workflow file and executed by the runner with no edits.
    const workflowsDir = makeTempDir();
    const savedPath = join(workflowsDir, "editor_assembled.yaml");
    await writeFile(savedPath, editorYaml, "utf8");

    const result = await runScenario(await loadScenario(savedPath), {
      ...DEFAULT_RUN_CONFIG,
      seed: 5,
      maxSteps: 10,
      maxDurationMs: 60_000,
      startupTimeoutMs: 15_000,
      hostCommand: [process.execPath, fakeHostPath],
      projectPath: makeTempDir(),
      importDir: makeTempDir(),
      reuseProject: false,
      outDir: makeTempDir(),
    });

    expect(result.verdict).toBe("pass");
    expect(result.steps.map((step) => step.nodeId)).toEqual([
      "workspace_ready",
      "open_first_image",
      "drag_exposure_slider",
    ]);
    expect(result.steps.every((step) => step.opOk)).toBe(true);
  });
});
