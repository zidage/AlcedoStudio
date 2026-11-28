#!/usr/bin/env python3
#  Copyright 2026 Yurun Zi
#  SPDX-License-Identifier: GPL-3.0-only
#  Additional permission under GPLv3 section 7 applies; see the LICENSE file.
"""The file management fuzz (#1188, #1192): random operation sequences on the sleeve folder
tree, the photo membership, and the mounted folders, driven through a headless alcedo-cli
session and checked against a reference model after each step.

Commands:
  run     --seed N --steps S --mode {watcher-off,watcher-on,concurrent} --out DIR
          Generates the operations from the seed and the model, runs and checks each step, and
          writes ops.jsonl, steps.jsonl, the session logs, the disk directory, and the project
          into DIR. Exit 0: every step passed; 1: a step failed (DIR/failure.json); 2: a usage
          or setup error.
  replay  FILE [--out DIR]
          Runs an operation log in a new session. The same exit codes.
  shrink  FILE [--out DIR]
          Removes the operations that the failure of FILE does not need, with a replay in a new
          session for each try, and writes the shortest log that still fails the same check of
          the same operation kind to DIR/shrunk.jsonl.

See README.md for the parameters and the checks.
"""

from __future__ import annotations

import argparse
import json
import os
import random
import shutil
import sys
import time
import traceback
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
sys.path.insert(0, str(SCRIPT_DIR))

import fuzz_disk  # noqa: E402
import fuzz_ops  # noqa: E402
from fuzz_ops import CheckFailure, Driver, Generator, RunOptions, weights_for  # noqa: E402
from fuzz_session import HostFailure, Session, find_host_binaries  # noqa: E402

REPO = SCRIPT_DIR.parents[1]
EXE = ".exe" if sys.platform == "win32" else ""


def log(message: str) -> None:
    print(f"[{time.strftime('%H:%M:%S')}] {message}", flush=True)


def default_build_dirs() -> list[Path]:
    return [REPO / "build" / name for name in
            ("release", "macos-arm-metal-ci", "macos-release", "debug", "macos-debug")]


def find_invariant_check(explicit: str | None, build_dir: Path | None) -> Path | None:
    """StorageInvariantCheck of a build tree (tests/app). 'none' runs check 4 without it: the
    save and the read of the packed database still run."""
    if explicit == "none":
        return None
    if explicit:
        return Path(explicit)
    trees = ([build_dir] if build_dir else []) + default_build_dirs()
    for tree in trees:
        for path in (tree / "alcedo_studio" / "tests" / "app" / "StorageInvariantCheck_runtime" /
                     f"StorageInvariantCheck{EXE}",
                     tree / "alcedo_studio" / "tests" / "app" / f"StorageInvariantCheck{EXE}"):
            if path.is_file():
                return path
    raise FileNotFoundError("no StorageInvariantCheck in the build trees; build the target or "
                            "give --invariant-check PATH")


def locate_binaries(args) -> tuple[Path, Path]:
    cli = Path(args.cli) if args.cli else None
    build = Path(args.build_dir) if args.build_dir else None
    if cli or build:
        return find_host_binaries(cli, build)
    for tree in default_build_dirs():
        try:
            return find_host_binaries(None, tree)
        except FileNotFoundError:
            continue
    raise FileNotFoundError("no alcedo-cli build; give --cli or --build-dir")


def parse_weights(items: list[str]) -> dict[str, float]:
    weights = {}
    for item in items or []:
        kind, _, value = item.partition("=")
        if kind not in fuzz_ops.WEIGHTS and kind not in ("concurrent_round", "settle"):
            raise SystemExit(f"unknown operation kind {kind}")
        weights[kind] = float(value)
    return weights


class Run:
    """One run: a session, a driver, and the log files of DIR."""

    def __init__(self, header: dict, out: Path, cli: Path, host: Path,
                 invariant_check: Path | None) -> None:
        self.header = header
        self.out = out
        if out.exists():
            fuzz_disk.remove_tree(out)
        out.mkdir(parents=True)
        env = {}
        if header.get("clean_file_limit"):
            env["ALCEDO_TEST_SLEEVE_CLEAN_FILE_LIMIT"] = str(header["clean_file_limit"])
        self.session = Session(cli, host, out / "run", name=f"fuzz{os.getpid()}", env=env)
        options = RunOptions(mode=header["mode"], check_every=header.get("check_every", 1),
                             save_every=header.get("save_every", 25),
                             reopen_every=header.get("reopen_every", 60),
                             inject_failure=header.get("inject_failure"),
                             scale=header.get("scale", 1.0),
                             avoid_known_issues=header.get("avoid_known_issues", True))
        self.driver = Driver(self.session, out, options, invariant_check)
        self.ops_file = (out / "ops.jsonl").open("w", encoding="utf-8")
        self.steps_file = (out / "steps.jsonl").open("w", encoding="utf-8")
        self.ops_file.write(json.dumps({"header": header}, ensure_ascii=False) + "\n")

    def start(self) -> None:
        project = self.out / "project"
        project.mkdir()
        started = self.session.start(create=(project, "fuzz"),
                                     watcher="on" if self.header["mode"] == "watcher-on" else "off")
        self.driver.project_path = Path(started["project_path"])
        self.driver.check_state()

    def step(self, index: int, op: dict) -> None:
        self.ops_file.write(json.dumps(op, ensure_ascii=False) + "\n")
        self.ops_file.flush()
        record = self.driver.run(index, op)
        self.steps_file.write(json.dumps({"step": index, "op": op["op"], "id": op.get("id"),
                                          "seconds": round(record.seconds, 3),
                                          "skipped": record.skipped,
                                          "outcomes": record.outcomes},
                                         ensure_ascii=False) + "\n")
        self.steps_file.flush()

    def finish(self, failure: dict | None) -> int:
        try:
            if failure is None and not self.driver.unsettled:
                # The last checks: model equality, then the saved database.
                self.driver.check_state()
                self.driver.invariant_check_now()
        except (CheckFailure, HostFailure) as error:
            failure = describe_failure(error, len(self.driver.records), {"op": "final_check"})
        finally:
            self.driver.tools.restore_all()
            self.driver.volumes.detach_all()
            self.session.stop()
            self.ops_file.close()
            self.steps_file.close()
            self.collect_logs()
        summary = {"header": self.header, "steps": len(self.driver.records),
                   "skipped": sum(1 for r in self.driver.records if r.skipped),
                   "seconds": round(sum(r.seconds for r in self.driver.records), 1),
                   "photos": len(self.driver.model.photos),
                   "folders": len(self.driver.model.folders),
                   "mounts": len(self.driver.model.mounts),
                   "kinds": count_kinds(self.driver.records), "failure": failure}
        (self.out / "summary.json").write_text(json.dumps(summary, indent=2, ensure_ascii=False),
                                               encoding="utf-8")
        if failure is not None:
            (self.out / "failure.json").write_text(json.dumps(failure, indent=2,
                                                              ensure_ascii=False),
                                                   encoding="utf-8")
            return 1
        return 0

    def collect_logs(self) -> None:
        logs = self.out / "logs"
        logs.mkdir(exist_ok=True)
        for path in self.session.log_files:
            if path.exists():
                shutil.copy2(path, logs / path.name)


def count_kinds(records) -> dict:
    kinds: dict[str, int] = {}
    for record in records:
        kinds[record.op["op"]] = kinds.get(record.op["op"], 0) + 1
    return dict(sorted(kinds.items()))


def describe_failure(error: BaseException, step: int, op: dict) -> dict:
    if isinstance(error, CheckFailure):
        check, message, detail = error.check, error.message, error.detail
    elif isinstance(error, HostFailure):
        check, message, detail = "host", str(error), None
    else:
        check, message, detail = "driver", f"{type(error).__name__}: {error}", \
            traceback.format_exc()
    return {"step": step, "op": op.get("op"), "id": op.get("id"), "check": check,
            "message": message, "detail": detail, "operation": op}


def signature(failure: dict | None) -> tuple | None:
    if failure is None:
        return None
    return failure.get("op"), failure.get("check")


def execute(header: dict, ops: list[dict] | None, out: Path, cli: Path, host: Path,
            invariant_check: Path | None, quiet: bool = False) -> tuple[int, dict | None]:
    """Runs a generated sequence (@p ops None) or the operations @p ops."""
    run = Run(header, out, cli, host, invariant_check)
    failure = None
    deadline = time.monotonic() + header["time_limit"] if header.get("time_limit") else None
    op: dict = {"op": "start"}
    index = 0
    try:
        run.start()
        generator = Generator(header["seed"], run.driver,
                              weights_for(header.get("areas"), header.get("weights", {}),
                                          header["mode"])) if ops is None else None
        total = header["steps"] if ops is None else len(ops)
        for index in range(total):
            if deadline is not None and time.monotonic() > deadline:
                if not quiet:
                    log(f"the time limit ends the run after {index} steps")
                break
            op = generator.next(index + 1) if generator else ops[index]
            run.step(index + 1, op)
            if not quiet and (index + 1) % 25 == 0:
                log(f"step {index + 1}/{total}: {len(run.driver.model.photos)} photos, "
                    f"{len(run.driver.model.folders)} folders, "
                    f"{len(run.driver.model.mounts)} mounts")
        if run.driver.unsettled:
            run.step(index + 2, {"op": "settle", "delay_s": 1.0, "id": 10 ** 9})
    except (CheckFailure, HostFailure) as error:
        failure = describe_failure(error, index + 1, op)
    except Exception as error:  # noqa: BLE001 - a driver error fails the run with its trace
        failure = describe_failure(error, index + 1, op)
    code = run.finish(failure)
    if code and failure is None:
        failure = json.loads((out / "failure.json").read_text(encoding="utf-8"))
    return code, failure


def read_log(path: Path) -> tuple[dict, list[dict]]:
    lines = [json.loads(line) for line in path.read_text(encoding="utf-8").splitlines()
             if line.strip()]
    if not lines or "header" not in lines[0]:
        raise SystemExit(f"{path} has no header line")
    return lines[0]["header"], lines[1:]


def write_log(path: Path, header: dict, ops: list[dict]) -> None:
    with path.open("w", encoding="utf-8") as stream:
        stream.write(json.dumps({"header": header}, ensure_ascii=False) + "\n")
        for op in ops:
            stream.write(json.dumps(op, ensure_ascii=False) + "\n")


def shrink(header: dict, ops: list[dict], out: Path, cli: Path, host: Path,
           invariant_check: Path | None, time_limit: float, failure: dict | None) -> int:
    """Delta debugging over the operation list: removes chunks of operations while the replay
    fails with the same signature."""
    out.mkdir(parents=True, exist_ok=True)
    replay_header = dict(header, time_limit=None)
    tries = 0
    if failure is None:
        tries += 1
        _, failure = execute(replay_header, ops, out / f"try{tries}", cli, host, invariant_check,
                             quiet=True)
        fuzz_disk.remove_tree(out / f"try{tries}")
        if failure is None:
            log("the log does not fail; nothing to shrink")
            return 2
    target = signature(failure)
    log(f"shrinking {len(ops)} operations for {target}")
    # Operations after the failing one are not needed.
    failing = failure.get("id")
    if failing is not None:
        ops = [op for op in ops if op.get("id", 0) <= failing]
    deadline = time.monotonic() + time_limit
    chunks = 2
    while len(ops) >= 2 and time.monotonic() < deadline:
        size = max(1, len(ops) // chunks)
        removed = False
        for start in range(0, len(ops), size):
            if time.monotonic() >= deadline:
                break
            candidate = ops[:start] + ops[start + size:]
            if not candidate:
                continue
            tries += 1
            try_dir = out / f"try{tries}"
            _, result = execute(replay_header, candidate, try_dir, cli, host, invariant_check,
                                quiet=True)
            fuzz_disk.remove_tree(try_dir)
            if signature(result) == target:
                ops = candidate
                removed = True
                log(f"try {tries}: {len(ops)} operations still fail")
                break
        if removed:
            chunks = max(chunks - 1, 2)
        elif size == 1:
            break
        else:
            chunks = min(len(ops), chunks * 2)
    write_log(out / "shrunk.jsonl", header, ops)
    log(f"{out / 'shrunk.jsonl'}: {len(ops)} operations after {tries} tries")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)
    for name in ("run", "replay", "shrink"):
        command = sub.add_parser(name)
        command.add_argument("--out", help="the output directory")
        command.add_argument("--cli", help="alcedo-cli; alcedo_main is next to it")
        command.add_argument("--build-dir", help="a build tree or an installed tree")
        command.add_argument("--invariant-check",
                             help="StorageInvariantCheck, or 'none' to read the database only")
        if name == "run":
            command.add_argument("--seed", type=int, default=None)
            command.add_argument("--steps", type=int, default=200)
            command.add_argument("--mode", choices=["watcher-off", "watcher-on", "concurrent"],
                                 default="watcher-off")
            command.add_argument("--areas", help="comma-separated areas, for example M2,M3")
            command.add_argument("--weight", action="append", help="KIND=WEIGHT")
            command.add_argument("--check-every", type=int, default=1)
            command.add_argument("--save-every", type=int, default=25)
            command.add_argument("--reopen-every", type=int, default=60)
            command.add_argument("--clean-file-limit", type=int, default=None,
                                 help="ALCEDO_TEST_SLEEVE_CLEAN_FILE_LIMIT of the host; "
                                      "default: 2 for one seed in four, else unset")
            command.add_argument("--scale", type=float, default=1.0,
                                 help="factor of the large cases (tests 11 and 29)")
            command.add_argument("--time-limit", type=float, default=None,
                                 help="seconds; no new step starts after it")
            command.add_argument("--inject-failure", default=None,
                                 help="test only: the first operation of this kind fails")
            command.add_argument("--include-known-issues", action="store_true",
                                 help="also write the operations of the open issues of "
                                      "KNOWN_ISSUES in fuzz_ops.py")
            command.add_argument("--shrink", action="store_true",
                                 help="shrink the log after a failure")
            command.add_argument("--shrink-time-limit", type=float, default=900)
        else:
            command.add_argument("file")
            if name == "shrink":
                command.add_argument("--time-limit", type=float, default=1800)
    args = parser.parse_args()

    try:
        cli, host = locate_binaries(args)
        invariant_check = find_invariant_check(args.invariant_check,
                                               Path(args.build_dir) if args.build_dir else None)
    except FileNotFoundError as error:
        print(error, file=sys.stderr)
        return 2

    if args.command == "run":
        seed = args.seed if args.seed is not None else random.SystemRandom().getrandbits(64)
        rng = random.Random(seed)
        clean_limit = args.clean_file_limit
        if clean_limit is None:
            clean_limit = 2 if rng.random() < 0.25 else 0
        header = {"seed": seed, "steps": args.steps, "mode": args.mode,
                  "areas": args.areas.split(",") if args.areas else None,
                  "weights": parse_weights(args.weight), "check_every": args.check_every,
                  "save_every": args.save_every, "reopen_every": args.reopen_every,
                  "clean_file_limit": clean_limit, "scale": args.scale,
                  "time_limit": args.time_limit, "inject_failure": args.inject_failure,
                  "avoid_known_issues": not args.include_known_issues,
                  "known_issues": [] if args.include_known_issues else
                  [number for number, _, _ in fuzz_ops.KNOWN_ISSUES],
                  "platform": sys.platform}
        out = Path(args.out or REPO / "build" / "tmp" / "file_fuzz" / f"run_{seed}").resolve()
        log(f"run seed {seed}, {args.steps} steps, {args.mode}, clean file limit "
            f"{clean_limit or 'unset'}, out {out}")
        code, failure = execute(header, None, out, cli, host, invariant_check)
        if failure:
            log(f"FAILED at step {failure['step']} ({failure['op']}): {failure['check']}: "
                f"{failure['message'][:600]}")
            if args.shrink:
                _, ops = read_log(out / "ops.jsonl")
                shrink(header, ops, out / "shrink", cli, host, invariant_check,
                       args.shrink_time_limit, failure)
        else:
            log(f"passed: {out / 'summary.json'}")
        return code

    header, ops = read_log(Path(args.file))
    out = (Path(args.out) if args.out else
           Path(args.file).parent / f"{args.command}_{int(time.time())}").resolve()
    if args.command == "replay":
        code, failure = execute(dict(header, time_limit=None), ops, out, cli, host,
                                invariant_check)
        if failure:
            log(f"FAILED at step {failure['step']} ({failure['op']}): {failure['check']}: "
                f"{failure['message'][:600]}")
        else:
            log("passed")
        return code
    return shrink(header, ops, out, cli, host, invariant_check, args.time_limit, None)


if __name__ == "__main__":
    sys.exit(main())
