//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

/**
 * Workflow store: owns the scenario YAML files the flow editor lists, loads,
 * and saves. Files live in one flat directory (the platform `scenarios/`
 * folder by default) so a saved workflow is runnable by the CLI and the
 * dashboard with no further edits. Saving always validates through
 * {@link parseScenario}, the exact path the runner uses, so an invalid file
 * can never land on disk via the editor.
 */

import { mkdir, readdir, readFile, writeFile } from "node:fs/promises";
import { join } from "node:path";

import { parseScenario } from "./scenario-parse.js";

/** One stored workflow file. */
export interface WorkflowSummary {
  /** File name without the `.yaml` suffix; also the editor route key. */
  readonly name: string;
  readonly path: string;
  /** `name` field inside the YAML, when it parses. */
  readonly scenarioName: string | null;
  /** `start` node id inside the YAML, when it parses. */
  readonly start: string | null;
  /** Validation errors when the file does not parse; null when valid. */
  readonly errors: readonly string[] | null;
}

export interface WorkflowDocument {
  readonly name: string;
  readonly path: string;
  readonly yaml: string;
}

/** Workflow names must be simple slugs; this also blocks path traversal. */
const WORKFLOW_NAME = /^[A-Za-z0-9][A-Za-z0-9_-]*$/;

export class WorkflowStore {
  constructor(private readonly directory: string) {}

  /** Lists every `*.yaml` workflow in the store directory, sorted by name. */
  async list(): Promise<WorkflowSummary[]> {
    await mkdir(this.directory, { recursive: true });
    const files = (await readdir(this.directory))
      .filter((file) => file.endsWith(".yaml") || file.endsWith(".yml"))
      .sort();
    const summaries: WorkflowSummary[] = [];
    for (const file of files) {
      const path = join(this.directory, file);
      const name = file.replace(/\.(yaml|yml)$/, "");
      let scenarioName: string | null = null;
      let start: string | null = null;
      let errors: readonly string[] | null = null;
      try {
        const scenario = parseScenario(await readFile(path, "utf8"));
        scenarioName = scenario.name;
        start = scenario.start;
      } catch (error) {
        errors = [(error as Error).message];
      }
      summaries.push({ name, path, scenarioName, start, errors });
    }
    return summaries;
  }

  /** Reads one workflow's raw YAML text. */
  async read(name: string): Promise<WorkflowDocument> {
    const path = this.pathFor(name);
    const yaml = await readFile(path, "utf8");
    return { name, path, yaml };
  }

  /**
   * Validates and writes a workflow. Returns the stored document.
   *
   * @throws {ScenarioError} when the YAML fails validation; nothing is written.
   */
  async save(name: string, yamlText: string): Promise<WorkflowDocument> {
    const scenario = parseScenario(yamlText);
    const path = this.pathFor(name);
    await mkdir(this.directory, { recursive: true });
    await writeFile(path, yamlText, "utf8");
    return { name, path, yaml: yamlText };
  }

  /** Resolves a workflow name to an absolute path inside the store directory. */
  pathFor(name: string): string {
    if (!WORKFLOW_NAME.test(name)) {
      throw new Error(
        `Invalid workflow name '${name}': use letters, digits, '-' and '_' (first char alphanumeric).`,
      );
    }
    return join(this.directory, `${name}.yaml`);
  }
}
