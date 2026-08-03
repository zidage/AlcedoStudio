#!/usr/bin/env node
//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

/**
 * CLI entry: `fuzz run <scenario.yaml> [options]`. Loads a scenario, spawns the
 * test host, walks the DAG, and prints the verdict plus the failure-bundle path
 * on a non-pass verdict. Exit codes: 0 pass, 1 run failure (correctness/deadlock/
 * crash), 2 configuration or startup error.
 */

import { mkdir } from "node:fs/promises";
import { join, resolve } from "node:path";

import { loadScenario } from "./loader.js";
import { runScenario } from "./run.js";
import { DEFAULT_RUN_CONFIG, type RunConfig } from "./scenario.js";

const HELP = `Usage: fuzz run <scenario.yaml> [options]

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
  -h, --help               Show this help
`;

interface CliArgs {
  scenarioPath: string | undefined;
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
}

function parseArgs(argv: string[]): CliArgs {
  const args: CliArgs = {
    scenarioPath: undefined,
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
  };

  const positional: string[] = [];
  for (let index = 0; index < argv.length; index++) {
    const arg = argv[index]!;
    if (arg === "-h" || arg === "--help") {
      process.stdout.write(HELP);
      process.exit(0);
    }
    if (arg === "run") continue;
    if (arg.startsWith("--")) {
      const value = argv[index + 1];
      switch (arg) {
        case "--host": args.hostPath = value; index++; break;
        case "--project": args.projectPath = value; index++; break;
        case "--import": args.importDir = value; index++; break;
        case "--socket": args.probeSocket = value; index++; break;
        case "--seed": args.seed = Number.parseInt(value ?? "", 10); index++; break;
        case "--max-steps": args.maxSteps = Number.parseInt(value ?? "", 10); index++; break;
        case "--max-duration": args.maxDurationMs = Number.parseInt(value ?? "", 10); index++; break;
        case "--liveness": args.livenessThresholdMs = Number.parseInt(value ?? "", 10); index++; break;
        case "--startup": args.startupTimeoutMs = Number.parseInt(value ?? "", 10); index++; break;
        case "--out": args.outDir = value; index++; break;
        case "--reuse-project": args.reuseProject = true; break;
        default:
          throw new Error(`Unknown option: ${arg}`);
      }
    } else {
      positional.push(arg);
    }
  }

  args.scenarioPath = positional[0];
  return args;
}

async function main(argv: string[]): Promise<number> {
  const args = parseArgs(argv);
  if (args.scenarioPath === undefined) {
    process.stderr.write(HELP);
    return 2;
  }

  const scenario = await loadScenario(resolve(args.scenarioPath));
  const timestamp = new Date().toISOString().replace(/[:.]/g, "-");
  const outDir = resolve(args.outDir ?? join("build", "tmp", "ui_fuzz_platform", `${scenario.name}-${args.seed}-${timestamp}`));

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
  process.stdout.write(formatResult(result));
  return result.verdict === "pass" ? 0 : 1;
}

function formatResult(result: { verdict: string; steps: readonly unknown[]; failure?: { reason: string }; bundle?: { dir: string } }): string {
  const lines = [
    `Verdict: ${result.verdict}`,
    `Steps: ${result.steps.length}`,
  ];
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