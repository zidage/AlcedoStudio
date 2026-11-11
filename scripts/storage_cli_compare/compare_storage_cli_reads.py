#!/usr/bin/env python3
#  Copyright 2026 Yurun Zi
#  SPDX-License-Identifier: GPL-3.0-only
#  Additional permission under GPLv3 section 7 applies; see the LICENSE file.
"""Compares the alcedo-cli library reads of two Release builds on copies of the same projects.

The storage refactor of #785 (merge d0f89c738) changed the query code of the library reads. This
script (#807) runs the same read commands with the Release build of the baseline commit (the
commit before that merge) and with the Release build of the working tree, on fresh copies of the
same projects, and reports each command whose JSON answer differs. Both builds produce the
answers at run time; the script stores no expected value.

Steps (``--steps``, in this order):

build     Builds alcedo_main and alcedo-cli of the working tree (win_release) and installs them
          to <out>/install/current; builds StorageCliCompareProjectWriter (win_debug). Then
          checks out the baseline commit in the same working tree, applies
          baseline_library_stats.patch (the library.stats command and the bucket filter
          parameters of library.list for that commit), builds and installs the same targets to
          <out>/install/baseline, and checks out the starting branch again. The working tree
          must have no changes of tracked files.
projects  Writes the projects to <out>/projects: ``imported`` (the folders of --raw-folders of
          the sample RAW folder, imported with the baseline build), ``compat``
          (tests/resources/compat/raw_project_abdb000c8.alcd), and ``marked`` (ratings, color
          labels, Delete marks, semantic labels, mask assets, several commits, and albums,
          written by StorageCliCompareProjectWriter).
run       For each project and each build: opens a fresh copy in a headless session and reads
          the folders, the statistics, the full photo lists, and the search totals. The
          commands come from these reads of both builds: each bucket of each library.stats
          dimension (in library.stats and library.list), case and space variants of the
          labels, the empty values, each color label code and the Delete mark, each sort in
          both directions with pages, library.info of every photo, search texts with quotes,
          wildcard characters, and Chinese text, and parameters that the commands reject. Then
          each build answers the whole list on another fresh copy. <out>/report.md lists each
          command whose answer differs; <out>/differences.json holds both answers.

Exit code: 0 when every answer is the same, 1 when an answer differs, 2 when a step fails.

Run from the repository root on Windows, for example:
  python scripts/storage_cli_compare/compare_storage_cli_reads.py
  python scripts/storage_cli_compare/compare_storage_cli_reads.py --steps run
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

SCRIPT_DIR = Path(__file__).resolve().parent
REPO = SCRIPT_DIR.parents[1]
BASELINE_COMMIT = "d0f89c738^1"
PATCH_FILE = SCRIPT_DIR / "baseline_library_stats.patch"
SAMPLE_RAW = REPO / "alcedo_studio" / "tests" / "resources" / "sample_images" / "raw"
COMPAT_PROJECT = REPO / "alcedo_studio" / "tests" / "resources" / "compat" / "raw_project_abdb000c8.alcd"
WRITER_EXE = (REPO / "build" / "debug" / "alcedo_studio" / "tests" / "app" /
              "StorageCliCompareProjectWriter_runtime" / "StorageCliCompareProjectWriter.exe")
BUILDS = ("baseline", "current")
PROJECTS = ("imported", "compat", "marked")
DEFAULT_RAW_FOLDERS = ["camera", "cc sa 4.0", "daylight", "night", "portrait", "landscape",
                       "film_emulation", "he_compression_edge", "linear_dng", "xtrans_edge",
                       "gps", "bad_lens", "street", "city", "misc"]
TIME_ZONES = ("America/New_York", "Asia/Shanghai", "UTC")
SORTS = ("none", "capture_time", "import_time", "camera_model", "lens", "rating", "labels",
         "edit_time")
COLOR_CODES = ("red", "orange", "yellow", "green", "blue", "purple", "gray")
TEXT_DIMENSIONS = ("camera", "lens", "label")
LIST_LIMIT = 1000
SEARCH_PAGE = 48
INFO_BATCH = 100
FIXED_SEARCH_TEXTS = [
    "_", "%", "%_%", "a_b", "100%", '"', "'", "''", 'quote"d', "it's", '"Canon EOS R5"',
    "' OR 1=1 --", "\\", "*", "风景", "相机", "沙漠", "城市 夜景", "相册", "sky", "SKY", "Sky ",
    "2026", "2026-06-07", "2026:06:07", "2026-03-08", "Canon", "canon", "EOS R5", "NIKON",
    "iso 100", "f/2.8", "35mm", "ARW", ".dng", "DSC", "IMG_", "unknown", "(unknown)",
]


class StepFailure(RuntimeError):
    pass


def log(message: str) -> None:
    print(f"[{time.strftime('%H:%M:%S')}] {message}", flush=True)


# ------------------------------------------------------------------------------------------------
# Build
# ------------------------------------------------------------------------------------------------


def git(*args: str, check: bool = True) -> str:
    result = subprocess.run(["git", *args], cwd=REPO, capture_output=True, text=True,
                            encoding="utf-8", errors="replace")
    if check and result.returncode != 0:
        raise StepFailure(f"git {' '.join(args)} failed: {result.stderr.strip()}")
    return result.stdout.strip()


def msvc(args: list[str], log_path: Path) -> None:
    """Runs scripts\\msvc_env.cmd (CMake in the MSVC environment) and writes its output to
    @p log_path."""
    log_path.parent.mkdir(parents=True, exist_ok=True)
    log(f"cmake {' '.join(args)} (log: {log_path})")
    with log_path.open("w", encoding="utf-8", errors="replace") as stream:
        result = subprocess.run(["cmd", "/c", r"scripts\msvc_env.cmd", *args], cwd=REPO,
                                stdout=stream, stderr=subprocess.STDOUT)
    if result.returncode != 0:
        raise StepFailure(f"cmake {' '.join(args)} failed with {result.returncode}; see {log_path}")


def build_and_install(install_dir: Path, logs: Path, name: str, jobs: int) -> None:
    msvc(["--build", "--preset", "win_release", "--target", "alcedo_main", "alcedo_cli",
          "--parallel", str(jobs)], logs / f"{name}_release_build.log")
    if install_dir.exists():
        shutil.rmtree(install_dir)
    msvc(["--install", "build/release", "--prefix", str(install_dir)],
         logs / f"{name}_install.log")
    for exe in ("alcedo-cli.exe", "alcedo_main.exe"):
        if not (install_dir / "bin" / exe).exists():
            raise StepFailure(f"the install of {name} has no bin/{exe}")


def step_build(out: Path, jobs: int, baseline: str) -> None:
    if git("status", "--porcelain", "--untracked-files=no", "--ignore-submodules=all"):
        raise StepFailure("the working tree has changes of tracked files; commit them first")
    start_ref = git("rev-parse", "--abbrev-ref", "HEAD")
    if start_ref == "HEAD":
        start_ref = git("rev-parse", "HEAD")
    logs = out / "logs"
    build_and_install(out / "install" / "current", logs, "current", jobs)
    msvc(["--build", "--preset", "win_debug", "--target", "StorageCliCompareProjectWriter",
          "--parallel", str(jobs)], logs / "writer_debug_build.log")

    patch_copy = out / "baseline_library_stats.patch"
    patch_copy.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(PATCH_FILE, patch_copy)
    patched = [line[len("+++ b/"):] for line in patch_copy.read_text(encoding="utf-8").splitlines()
               if line.startswith("+++ b/")]
    baseline_sha = git("rev-parse", baseline)
    log(f"checking out the baseline {baseline} ({baseline_sha})")
    git("checkout", "--detach", baseline_sha)
    try:
        git("apply", str(patch_copy))
        build_and_install(out / "install" / "baseline", logs, "baseline", jobs)
    finally:
        git("checkout", "--", *patched, check=False)
        git("checkout", start_ref)
        log(f"checked out {start_ref} again")
    (out / "install" / "builds.json").write_text(
        json.dumps({"baseline": baseline_sha, "current": git("rev-parse", "HEAD")}, indent=2),
        encoding="utf-8")


# ------------------------------------------------------------------------------------------------
# Sessions
# ------------------------------------------------------------------------------------------------


@dataclass
class Session:
    """One headless alcedo-cli session of one installed build."""

    install: Path
    run_dir: Path
    name: str = "compare"
    started: bool = False

    def cli(self, args: list[str], stdin: bytes | None = None, timeout: int = 3600):
        command = [str(self.install / "bin" / "alcedo-cli.exe"), "--session-dir",
                   str(self.run_dir / "sessions"), "--session", self.name, "--json", *args]
        environment = dict(os.environ)
        environment.pop("ALCEDO_SESSION", None)
        return subprocess.run(command, input=stdin, capture_output=True, timeout=timeout,
                              env=environment)

    def start(self, project: Path | None = None, create: tuple[Path, str] | None = None) -> dict:
        self.run_dir.mkdir(parents=True, exist_ok=True)
        args = ["--timeout", "600000", "session", "start", "--headless"]
        if project is not None:
            args += ["--project", str(project)]
        else:
            args += ["--create", f"{create[0]},{create[1]}"]
        args += ["--settings-dir", str(self.run_dir / "settings"), "--log-file",
                 str(self.run_dir / "host.log"), "--host-binary",
                 str(self.install / "bin" / "alcedo_main.exe")]
        result = self.cli(args)
        if result.returncode != 0:
            raise StepFailure(f"session start failed in {self.run_dir}: "
                              f"{result.stdout.decode('utf-8', 'replace')}"
                              f"{result.stderr.decode('utf-8', 'replace')}")
        self.started = True
        return json.loads(result.stdout.decode("utf-8"))

    def call(self, method: str, params: dict | None = None) -> dict:
        result = self.cli(["call", method, json.dumps(params or {}, ensure_ascii=False)])
        answer = json.loads(result.stdout.decode("utf-8") or "{}")
        if result.returncode != 0:
            raise StepFailure(f"{method} failed: {answer} "
                              f"{result.stderr.decode('utf-8', 'replace')}")
        return answer

    def batch(self, requests: list[dict]) -> dict[Any, dict]:
        """Sends @p requests ({id, method, params}) over one connection; returns the answer of
        each id: {"result": ...} or {"error": ...}."""
        lines = "".join(json.dumps({"id": r["id"], "method": r["method"], "params": r["params"]},
                                   ensure_ascii=False) + "\n" for r in requests)
        result = self.cli(["batch"], stdin=lines.encode("utf-8"), timeout=7200)
        if result.returncode not in (0, 1):
            raise StepFailure(f"batch failed with {result.returncode}: "
                              f"{result.stderr.decode('utf-8', 'replace')}")
        answers: dict[Any, dict] = {}
        for line in result.stdout.decode("utf-8").splitlines():
            if line.strip():
                answer = json.loads(line)
                answers[answer.pop("id")] = answer
        missing = [r["id"] for r in requests if r["id"] not in answers]
        if missing:
            raise StepFailure(f"batch returned no answer for {len(missing)} requests "
                              f"(first id {missing[0]})")
        return answers

    def stop(self) -> None:
        if self.started:
            self.cli(["session", "stop"], timeout=600)
            self.started = False


# ------------------------------------------------------------------------------------------------
# Projects
# ------------------------------------------------------------------------------------------------


def step_projects(out: Path, raw_folders: list[str]) -> None:
    projects = out / "projects"
    projects.mkdir(parents=True, exist_ok=True)

    shutil.copyfile(COMPAT_PROJECT, projects / "compat.alcd")
    log("compat project copied")

    if not WRITER_EXE.exists():
        raise StepFailure(f"{WRITER_EXE} is missing; run the build step")
    marked = projects / "marked.alcd"
    if marked.exists():
        marked.unlink()
    environment = dict(os.environ, ALCEDO_STORAGE_COMPARE_PROJECT=str(marked))
    with (out / "logs" / "writer_run.log").open("w", encoding="utf-8", errors="replace") as stream:
        result = subprocess.run([str(WRITER_EXE)], cwd=WRITER_EXE.parent, env=environment,
                                stdout=stream, stderr=subprocess.STDOUT)
    if result.returncode != 0 or not marked.exists():
        raise StepFailure(f"the project writer failed; see {out / 'logs' / 'writer_run.log'}")
    log("marked project written")

    work = out / "projects_work" / f"imported_{int(time.time())}"
    session = Session(out / "install" / "baseline", work / "run")
    try:
        started = session.start(create=(work, "imported"))
        project_path = Path(started["project_path"])
        for folder in raw_folders:
            source = SAMPLE_RAW / folder
            if not source.is_dir():
                raise StepFailure(f"{source} is not a folder")
            task = session.call("library.import", {"folder": str(source), "recursive": True})
            final = session.call("tasks.wait", {"task_id": task["task_id"],
                                                "timeout_ms": 3600000})
            log(f"imported {folder}: {final.get('state')}")
        total = session.call("library.list", {"limit": 1})["total"]
        log(f"imported project holds {total} photos")
    finally:
        session.stop()
    shutil.copyfile(project_path, projects / "imported.alcd")


# ------------------------------------------------------------------------------------------------
# Commands
# ------------------------------------------------------------------------------------------------


@dataclass
class Discovery:
    """Union of the reads of both builds that the command list is generated from."""

    folders: list[int] = field(default_factory=list)
    labels: dict[tuple[int, str, str], list[str]] = field(default_factory=dict)
    totals: dict[int, int] = field(default_factory=dict)
    element_ids: list[int] = field(default_factory=list)
    file_names: list[str] = field(default_factory=list)
    search_totals: dict[str, int] = field(default_factory=dict)

    def add_labels(self, key: tuple[int, str, str], labels: list[str]) -> None:
        known = self.labels.setdefault(key, [])
        for label in labels:
            if label not in known:
                known.append(label)


def stats_key(folder: int, dimension: str, zone: str = "") -> tuple[int, str, str]:
    return (folder, dimension, zone if dimension == "import_date" else "")


def discovery_folder_requests(folders: list[int]) -> list[dict]:
    requests = []
    for folder in folders:
        for zone in TIME_ZONES:
            requests.append({"id": f"stats|{folder}|{zone}", "method": "library.stats",
                             "params": {"folder_id": folder, "time_zone": zone}})
        requests.append({"id": f"list|{folder}", "method": "library.list",
                         "params": {"folder_id": folder, "limit": LIST_LIMIT}})
    return requests


def search_texts(discovery: Discovery) -> list[str]:
    texts = list(FIXED_SEARCH_TEXTS)
    for (folder, dimension, _), labels in discovery.labels.items():
        if dimension in ("camera", "lens", "label") and folder == discovery.folders[0]:
            texts += [label for label in labels if label]
    for name in discovery.file_names:
        stem = name.rsplit(".", 1)[0]
        texts += [stem[:4], stem[-4:]]
    unique: list[str] = []
    for text in texts:
        if text not in unique:
            unique.append(text)
    return unique


def discover(out: Path, project: str) -> Discovery:
    discovery = Discovery()
    for build in BUILDS:
        run_dir = out / "runs" / project / f"discovery_{build}"
        if run_dir.exists():
            shutil.rmtree(run_dir)
        copy = run_dir / f"{project}.alcd"
        run_dir.mkdir(parents=True)
        shutil.copyfile(out / "projects" / f"{project}.alcd", copy)
        session = Session(out / "install" / build, run_dir)
        try:
            session.start(project=copy)
            folders_answer = session.batch([{"id": "folders", "method": "library.folders",
                                             "params": {}}])["folders"]
            folders = [f["folder_id"] for f in folders_answer["result"]["folders"]]
            for folder in folders:
                if folder not in discovery.folders:
                    discovery.folders.append(folder)
            answers = session.batch(discovery_folder_requests(folders))
            for folder in folders:
                for zone in TIME_ZONES:
                    stats = answers[f"stats|{folder}|{zone}"].get("result", {})
                    for dimension in ("capture_date", "camera", "lens", "label", "rating",
                                      "import_date"):
                        labels = [b["label"] for b in stats.get(dimension, [])]
                        discovery.add_labels(stats_key(folder, dimension, zone), labels)
                listing = answers[f"list|{folder}"].get("result", {})
                discovery.totals[folder] = max(discovery.totals.get(folder, 0),
                                               listing.get("total", 0))
                for item in listing.get("items", []):
                    if item["element_id"] not in discovery.element_ids:
                        discovery.element_ids.append(item["element_id"])
                    if item["file_name"] not in discovery.file_names:
                        discovery.file_names.append(item["file_name"])
            texts = search_texts(discovery)
            search_answers = session.batch(
                [{"id": f"search|{i}", "method": "search.query",
                  "params": {"text": text, "mode": "fuzzy", "limit": SEARCH_PAGE}}
                 for i, text in enumerate(texts)])
            for i, text in enumerate(texts):
                total = search_answers[f"search|{i}"].get("result", {}).get("total", 0)
                discovery.search_totals[text] = max(discovery.search_totals.get(text, 0), total)
        finally:
            session.stop()
    return discovery


def label_variants(dimension: str, labels: list[str]) -> list[str]:
    """The bucket labels, the empty values, and case and space variants of the text labels."""
    values = list(labels) + ["", "(unknown)"]
    if dimension in TEXT_DIMENSIONS:
        for label in labels:
            values += [label.upper(), label.lower(), f" {label}", f"{label} "]
    if dimension == "capture_date":
        values += ["2026-02-30", "2026-6-7"]
    if dimension == "rating":
        values += ["0", "6", "-1", "x"]
    unique: list[str] = []
    for value in values:
        if value not in unique:
            unique.append(value)
    return unique


def generate_commands(discovery: Discovery) -> list[dict]:
    commands: list[dict] = []

    def add(method: str, params: dict) -> None:
        commands.append({"id": len(commands), "method": method, "params": params})

    add("library.folders", {})
    for folder in discovery.folders:
        add("library.stats", {"folder_id": folder})
        for zone in TIME_ZONES:
            add("library.stats", {"folder_id": folder, "time_zone": zone})

        # Each bucket of each dimension, the empty values, and variants of the labels.
        for dimension in ("capture_date", "camera", "lens", "label", "rating"):
            for value in label_variants(dimension, discovery.labels.get(
                    stats_key(folder, dimension), [])):
                add("library.stats", {"folder_id": folder, dimension: value})
                add("library.list", {"folder_id": folder, dimension: value, "limit": LIST_LIMIT})
        for zone in TIME_ZONES:
            labels = discovery.labels.get(stats_key(folder, "import_date", zone), [])
            for value in label_variants("import_date", labels):
                if value == "(unknown)":
                    continue
                params = {"folder_id": folder, "import_date": value, "time_zone": zone}
                add("library.stats", params)
                add("library.list", {**params, "limit": LIST_LIMIT, "sort": "import_time"})

        # Color label codes and the Delete mark, alone and together.
        for code in COLOR_CODES:
            add("library.stats", {"folder_id": folder, "color_label": code})
            add("library.list", {"folder_id": folder, "color_label": code, "limit": LIST_LIMIT})
            add("library.list", {"folder_id": folder, "color_label": code, "deleted": False,
                                 "limit": LIST_LIMIT})
        for deleted in (True, False):
            add("library.stats", {"folder_id": folder, "deleted": deleted})
            add("library.list", {"folder_id": folder, "deleted": deleted, "limit": LIST_LIMIT,
                                 "sort": "rating", "descending": True})

        # Two buckets together.
        cameras = discovery.labels.get(stats_key(folder, "camera"), [])[:4]
        ratings = discovery.labels.get(stats_key(folder, "rating"), [])
        for camera in cameras:
            for rating in ratings:
                add("library.list", {"folder_id": folder, "camera": camera, "rating": rating,
                                     "limit": LIST_LIMIT, "sort": "capture_time"})

        # Every sort in both directions: the full list and pages of 7 photos.
        total = discovery.totals.get(folder, 0)
        for sort in SORTS:
            for descending in (False, True):
                base = {"folder_id": folder, "sort": sort, "descending": descending}
                add("library.list", {**base, "limit": LIST_LIMIT})
                for offset in range(0, min(total, 70) + 1, 7):
                    add("library.list", {**base, "offset": offset, "limit": 7})
                add("library.list", {**base, "offset": total, "limit": 7})

    # library.info of every photo, an unknown id, and a repeated id.
    ids = discovery.element_ids
    for begin in range(0, len(ids), INFO_BATCH):
        add("library.info", {"element_ids": ids[begin:begin + INFO_BATCH]})
    if ids:
        add("library.info", {"element_ids": [ids[0], max(ids) + 1000]})
        add("library.info", {"element_ids": [ids[0], ids[0]]})

    # Search texts: every page of the text search, and the first page of the auto mode.
    add("search.query", {"text": ""})
    add("search.query", {"text": "   "})
    for text, total in discovery.search_totals.items():
        pages = max(1, min(6, -(-total // SEARCH_PAGE)))
        for page in range(pages):
            add("search.query", {"text": text, "mode": "fuzzy", "offset": page * SEARCH_PAGE,
                                 "limit": SEARCH_PAGE})
        add("search.query", {"text": text, "mode": "fuzzy", "offset": 3, "limit": 5})
        add("search.query", {"text": text})

    # Parameters that the commands reject.
    add("library.list", {"import_date": "2026-03-08"})
    add("library.stats", {"import_date": "2026-02-30", "time_zone": "America/New_York"})
    add("library.stats", {"import_date": "2026-03-08", "time_zone": "Mars/Olympus_Mons"})
    add("library.stats", {"time_zone": "Mars/Olympus_Mons"})
    add("library.list", {"color_label": "Red"})
    add("library.list", {"folder_id": 4294967295})
    add("library.list", {"limit": LIST_LIMIT + 1})
    return commands


def run_commands(out: Path, project: str, build: str, commands: list[dict]) -> dict:
    run_dir = out / "runs" / project / f"commands_{build}"
    if run_dir.exists():
        shutil.rmtree(run_dir)
    run_dir.mkdir(parents=True)
    copy = run_dir / f"{project}.alcd"
    shutil.copyfile(out / "projects" / f"{project}.alcd", copy)
    session = Session(out / "install" / build, run_dir)
    try:
        session.start(project=copy)
        began = time.monotonic()
        answers = session.batch(commands)
        log(f"{project} {build}: {len(commands)} commands in {time.monotonic() - began:.1f} s")
    finally:
        session.stop()
    with (run_dir / "answers.jsonl").open("w", encoding="utf-8") as stream:
        for command in commands:
            stream.write(json.dumps({"id": command["id"], **answers[command["id"]]},
                                    ensure_ascii=False, sort_keys=True) + "\n")
    return answers


# ------------------------------------------------------------------------------------------------
# Comparison
# ------------------------------------------------------------------------------------------------


def first_difference(a: Any, b: Any, path: str = "") -> tuple[str, Any, Any]:
    """The JSON pointer of the first value that differs, with both values."""
    if isinstance(a, dict) and isinstance(b, dict):
        for key in sorted(set(a) | set(b)):
            if key not in a or key not in b or a[key] != b[key]:
                if key in a and key in b:
                    return first_difference(a[key], b[key], f"{path}/{key}")
                return f"{path}/{key}", a.get(key, "<absent>"), b.get(key, "<absent>")
    if isinstance(a, list) and isinstance(b, list):
        for index in range(min(len(a), len(b))):
            if a[index] != b[index]:
                return first_difference(a[index], b[index], f"{path}/{index}")
        if len(a) != len(b):
            return f"{path} (length)", len(a), len(b)
    return path or "/", a, b


def short(value: Any, limit: int = 240) -> str:
    text = json.dumps(value, ensure_ascii=False, sort_keys=True)
    return text if len(text) <= limit else text[:limit] + "..."


def step_run(out: Path, projects: list[str]) -> int:
    differences: list[dict] = []
    summary: list[tuple[str, int, int]] = []
    for project in projects:
        log(f"{project}: discovery")
        discovery = discover(out, project)
        commands = generate_commands(discovery)
        with (out / "runs" / project / "commands.jsonl").open("w", encoding="utf-8") as stream:
            for command in commands:
                stream.write(json.dumps(command, ensure_ascii=False) + "\n")
        answers = {build: run_commands(out, project, build, commands) for build in BUILDS}
        count = 0
        for command in commands:
            baseline = answers["baseline"][command["id"]]
            current = answers["current"][command["id"]]
            if baseline != current:
                count += 1
                pointer, old, new = first_difference(baseline, current)
                differences.append({"project": project, **command, "pointer": pointer,
                                    "baseline": baseline, "current": current,
                                    "baseline_value": old, "current_value": new})
        summary.append((project, len(commands), count))

    (out / "differences.json").write_text(
        json.dumps(differences, ensure_ascii=False, indent=2), encoding="utf-8")
    builds = json.loads((out / "install" / "builds.json").read_text(encoding="utf-8"))
    lines = ["# alcedo-cli read comparison", "",
             f"Baseline build: `{builds['baseline']}`. Current build: `{builds['current']}`.", "",
             "| Project | Commands | Different answers |", "| --- | --- | --- |"]
    lines += [f"| {p} | {n} | {d} |" for p, n, d in summary]
    lines.append("")
    for diff in differences:
        lines += [f"## {diff['project']} #{diff['id']} `{diff['method']}`", "",
                  f"Parameters: `{short(diff['params'], 400)}`", "",
                  f"First difference at `{diff['pointer']}`:", "",
                  f"- baseline: `{short(diff['baseline_value'])}`",
                  f"- current: `{short(diff['current_value'])}`", ""]
    (out / "report.md").write_text("\n".join(lines), encoding="utf-8")
    for project, count, different in summary:
        log(f"{project}: {count} commands, {different} different answers")
    log(f"report: {out / 'report.md'}")
    return 1 if differences else 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--out", type=Path, default=REPO / "build" / "tmp" / "storage_cli_compare")
    parser.add_argument("--steps", default="build,projects,run")
    parser.add_argument("--baseline", default=BASELINE_COMMIT)
    parser.add_argument("--projects", default=",".join(PROJECTS))
    parser.add_argument("--raw-folders", default=",".join(DEFAULT_RAW_FOLDERS),
                        help="comma-separated folders of the sample RAW folder to import")
    parser.add_argument("--jobs", type=int, default=6)
    args = parser.parse_args()
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    steps = [s.strip() for s in args.steps.split(",") if s.strip()]
    try:
        if "build" in steps:
            step_build(out, args.jobs, args.baseline)
        if "projects" in steps:
            step_projects(out, [f for f in args.raw_folders.split(",") if f])
        if "run" in steps:
            return step_run(out, [p for p in args.projects.split(",") if p])
    except (StepFailure, subprocess.TimeoutExpired) as error:
        log(f"failed: {error}")
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
