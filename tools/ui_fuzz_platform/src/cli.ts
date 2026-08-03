#!/usr/bin/env node
//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

/**
 * CLI entry: `fuzz run <scenario.yaml> [options]` and `fuzz replay <run-id>`.
 * Loads a scenario, spawns the test host, walks the DAG, archives the run to
 * SQLite, and prints the verdict plus the failure-bundle path on a non-pass
 * verdict. Exit codes: 0 pass, 1 run failure (correctness/deadlock/crash),
 * 2 configuration or startup error.
 */

import { mkdir } from "node:fs/promises";
import { join, resolve } from "node:path";

import { loadScenario } from "./loader.js";
import { defaultResultDbPath } from "./paths.js";
import { replayRun } from "./replay.js";
import { ResultStore } from "./result-store.js";
import { runScenario } from "./run.js";
import { DEFAULT_RUN_CONFIG, type RunConfig } from "./scenario.js";

const HELP = `Usage:
  fuzz run <scenario.yaml> [options]
  fuzz replay <run-id> [options]

Options:
  --host <path>           Test host executable (alcedo_studio_test_host)
  --project <dir>          Project storage directory
  --import <dir>           Directory to import recursively
  --socket <name>          Probe socket name override
  --reuse-project          Skip re-import when the project already has the set
  --seed <n>               Run seed (recorded for replay; Phase 2 walks linearly)
  --max-steps <n>          Step bound before a pass stop (default 1000)
  --max-duration <ms>      Duration bound before a pass stop (default 300000)
  --liveness <ms>          Heartbeat gap threshold for deadlock (default 5000)
  --startup <ms>           Host startup/ready timeout (default 120000)
  --out <dir>              Run output directory (default build/tmp/ui_fuzz_platform/<name>-<seed>-<ts>)
  --db <path>              SQLite result store (default data/results.sqlite)
  --no-persist             Skip SQLite archival
  -h, --help               Show this help
`;

interface SharedArgs {
  hostPath: string | undefined;
  projectPath: string | undefined;
  importDir: string | undefined;
  probeSocket: string | undefined;
  reuseProject: boolean;
  seed: number;
  maxSteps: number;
  maxDurationMs: number;
  livenessThresholdMs: number;
  startupTimeoutMs: number;
  outDir: string | undefined;
  dbPath: string;
  persist: boolean;
}

interface RunCliArgs extends SharedArgs {
  command: "run";
  scenarioPath: string | undefined;
}

interface ReplayCliArgs extends SharedArgs {
  command: "replay";
  runId: string | undefined;
}

type CliArgs = RunCliArgs | ReplayCliArgs;

function parseArgs(argv: string[]): CliArgs {
  const shared: SharedArgs = {
    hostPath: undefined,
    projectPath: undefined,
    importDir: undefined,
    probeSocket: undefined,
    reuseProject: false,
    seed: DEFAULT_RUN_CONFIG.seed,
    maxSteps: DEFAULT_RUN_CONFIG.maxSteps,
    maxDurationMs: DEFAULT_RUN_CONFIG.maxDurationMs,
    livenessThresholdMs: DEFAULT_RUN_CONFIG.livenessThresholdMs,
    startupTimeoutMs: DEFAULT_RUN_CONFIG.startupTimeoutMs,
    outDir: undefined,
    dbPath: defaultResultDbPath(),
    persist: true,
  };

  let command: "run" | "replay" | undefined;
  const positional: string[] = [];

  for (let index = 0; index < argv.length; index++) {
    const arg = argv[index]!;
    if (arg === "-h" || arg === "--help") {
      process.stdout.write(HELP);
      process.exit(0);
    }
    if (arg === "run" || arg === "replay") {
      command = arg;
      continue;
    }
    if (arg.startsWith("--")) {
      const value = argv[index + 1];
      switch (arg) {
        case "--host":
          shared.hostPath = value;
          index++;
          break;
        case "--project":
          shared.projectPath = value;
          index++;
          break;
        case "--import":
          shared.importDir = value;
          index++;
          break;
        case "--socket":
          shared.probeSocket = value;
          index++;
          break;
        case "--seed":
          shared.seed = Number.parseInt(value ?? "", 10);
          index++;
          break;
        case "--max-steps":
          shared.maxSteps = Number.parseInt(value ?? "", 10);
          index++;
          break;
        case "--max-duration":
          shared.maxDurationMs = Number.parseInt(value ?? "", 10);
          index++;
          break;
        case "--liveness":
          shared.livenessThresholdMs = Number.parseInt(value ?? "", 10);
          index++;
          break;
        case "--startup":
          shared.startupTimeoutMs = Number.parseInt(value ?? "", 10);
          index++;
          break;
        case "--out":
          shared.outDir = value;
          index++;
          break;
        case "--db":
          shared.dbPath = resolve(value ?? "");
          index++;
          break;
        case "--reuse-project":
          shared.reuseProject = true;
          break;
        case "--no-persist":
          shared.persist = false;
          break;
        default:
          throw new Error(`Unknown option: ${arg}`);
      }
    } else {
      positional.push(arg);
    }
  }

  if (command === "replay") {
    return { command: "replay", runId: positional[0], ...shared };
  }
  return { command: "run", scenarioPath: positional[0], ...shared };
}

async function main(argv: string[]): Promise<number> {
  const args = parseArgs(argv);

  if (args.command === "replay") {
    return runReplayCommand(args);
  }
  return runScenarioCommand(args);
}

async function runScenarioCommand(args: RunCliArgs): Promise<number> {
  if (args.scenarioPath === undefined) {
    process.stderr.write(HELP);
    return 2;
  }

  const scenarioPath = resolve(args.scenarioPath);
  const scenario = await loadScenario(scenarioPath);
  const timestamp = new Date().toISOString().replace(/[:.]/g, "-");
  const outDir = resolve(
    args.outDir ?? join("build", "tmp", "ui_fuzz_platform", `${scenario.name}-${args.seed}-${timestamp}`),
  );

  const config: RunConfig = {
    seed: args.seed,
    maxSteps: args.maxSteps,
    maxDurationMs: args.maxDurationMs,
    livenessThresholdMs: args.livenessThresholdMs,
    startupTimeoutMs: args.startupTimeoutMs,
    hostPath: args.hostPath,
    projectPath: args.projectPath,
    importDir: args.importDir,
    reuseProject: args.reuseProject,
    outDir,
  };

  process.stdout.write(`Running scenario '${scenario.name}' (seed=${config.seed}) -> ${outDir}\n`);
  const result = await runScenario(scenario, config);
  await mkdir(outDir, { recursive: true });

  if (args.persist) {
    const store = new ResultStore(args.dbPath);
    try {
      const runId = store.archiveRun({
        result,
        config,
        scenarioPath,
        logTail: result.logTail,
        treeSnapshot: result.treeSnapshot,
        screenshotPath: result.bundle?.screenshotPath,
      });
      process.stdout.write(`Archived run id: ${runId}\n`);
      process.stdout.write(`Result store: ${args.dbPath}\n`);
    } finally {
      store.close();
    }
  }

  process.stdout.write(formatResult(result));
  return result.verdict === "pass" ? 0 : 1;
}

async function runReplayCommand(args: ReplayCliArgs): Promise<number> {
  if (args.runId === undefined) {
    process.stderr.write(HELP);
    return 2;
  }

  const store = new ResultStore(args.dbPath);
  try {
    process.stdout.write(`Replaying run ${args.runId} from ${args.dbPath}\n`);
    const replay = await replayRun({
      store,
      originalRunId: args.runId,
      hostPathOverride: args.hostPath,
      outDir: args.outDir,
    });
    process.stdout.write(
      [
        `Replay run id: ${replay.replayRunId}`,
        `Seed: ${replay.seed}`,
        `Steps match original: ${replay.sequencesMatch ? "yes" : "no"}`,
        `Original steps: ${replay.originalFingerprints.map((s) => s.nodeId).join(" -> ")}`,
        `Replay steps:   ${replay.replayFingerprints.map((s) => s.nodeId).join(" -> ")}`,
        formatResult(replay.runResult).trimEnd(),
      ].join("\n") + "\n",
    );
    if (!replay.sequencesMatch) return 1;
    return replay.runResult.verdict === "pass" ? 0 : 1;
  } finally {
    store.close();
  }
}

function formatResult(result: {
  verdict: string;
  steps: readonly unknown[];
  failure?: { reason: string };
  bundle?: { dir: string };
}): string {
  const lines = [`Verdict: ${result.verdict}`, `Steps: ${result.steps.length}`];
  if (result.failure !== undefined) {
    lines.push(`Failure: ${result.failure.reason}`);
  }
  if (result.bundle !== undefined) {
    lines.push(`Bundle: ${result.bundle.dir}`);
  }
  return lines.join("\n") + "\n";
}

main(process.argv.slice(2))
  .then((code) => process.exit(code))
  .catch((error: Error) => {
    process.stderr.write(`fuzz: ${error.message}\n`);
    process.exit(2);
  });
