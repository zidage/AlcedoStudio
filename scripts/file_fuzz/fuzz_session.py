#  Copyright 2026 Yurun Zi
#  SPDX-License-Identifier: GPL-3.0-only
#  Additional permission under GPLv3 section 7 applies; see the LICENSE file.
"""The headless alcedo-cli session of one fuzz run (#1192), after the Session class of
scripts/storage_cli_compare.

Each `call` is one alcedo-cli process and one connection to the headless host; `batch` sends
several requests over one connection. An answer is an `Answer`: a result or a JSON-RPC error.
A connection error, a crash of the host, or a timeout raises `HostFailure`, which fails the run.
"""

from __future__ import annotations

import json
import os
import signal
import subprocess
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

IS_WINDOWS = sys.platform == "win32"
IS_MACOS = sys.platform == "darwin"

# JSON-RPC error codes of the automation protocol (automation_protocol.hpp).
INVALID_PARAMS = -32602
NOT_READY = -32001
REJECTED = -32002
BUSY = -32005
TIMEOUT = -32006
FAILED = -32007
INTERNAL_ERROR = -32603

# alcedo-cli exit codes (cli_commands.cpp).
CLI_OK = 0
CLI_COMMAND_ERROR = 1
CLI_CONNECTION_ERROR = 3


class HostFailure(RuntimeError):
    """The host crashed, stopped answering, or the CLI could not reach it."""


@dataclass
class Answer:
    """The answer to one request: `result`, or `error` with `code`, `message`, and `data`."""

    result: Any = None
    error: dict | None = None

    @property
    def ok(self) -> bool:
        return self.error is None

    @property
    def code(self) -> int | None:
        return None if self.error is None else self.error.get("code")

    @property
    def data(self) -> dict:
        if self.error is None:
            return {}
        data = self.error.get("data")
        return data if isinstance(data, dict) else {}

    @property
    def outcome(self) -> str:
        """A short name of the answer: 'ok', 'invalid_params', 'rejected:<data.error>', 'busy',
        'not_ready', or 'error:<code>' for the codes that fail a run."""
        if self.error is None:
            return "ok"
        code = self.code
        if code == INVALID_PARAMS:
            return "invalid_params"
        if code == REJECTED:
            return "rejected:" + str(self.data.get("error", ""))
        if code == BUSY:
            return "busy"
        if code == NOT_READY:
            return "not_ready"
        return f"error:{code}"

    def to_json(self) -> dict:
        return {"result": self.result} if self.error is None else {"error": self.error}


def find_host_binaries(cli: Path | None, build_dir: Path | None) -> tuple[Path, Path]:
    """The alcedo-cli and alcedo_main executables: next to @p cli, or in the build tree
    @p build_dir (`alcedo_studio/src/`), or in an installed tree (`bin/`)."""
    exe = ".exe" if IS_WINDOWS else ""
    candidates: list[Path] = []
    if cli is not None:
        candidates.append(cli.parent)
    if build_dir is not None:
        candidates += [build_dir / "alcedo_studio" / "src", build_dir / "bin", build_dir]
    for directory in candidates:
        cli_path = cli if cli is not None else directory / f"alcedo-cli{exe}"
        host = directory / f"alcedo_main{exe}"
        bundle = directory / "alcedo_main.app" / "Contents" / "MacOS" / "alcedo_main"
        if cli_path.is_file() and host.is_file():
            return cli_path, host
        if cli_path.is_file() and bundle.is_file():
            return cli_path, bundle
    raise FileNotFoundError(
        f"no alcedo-cli and alcedo_main in {', '.join(str(c) for c in candidates)}")


@dataclass
class Session:
    """One headless session of the run. @p run_dir holds the session files, the settings, and
    the host log of each start."""

    cli_path: Path
    host_path: Path
    run_dir: Path
    name: str = "fuzz"
    call_timeout_ms: int = 300000
    env: dict[str, str] = field(default_factory=dict)
    started: bool = False
    pid: int | None = None
    starts: int = 0
    log_files: list[Path] = field(default_factory=list)

    def _environment(self) -> dict[str, str]:
        environment = dict(os.environ)
        environment.pop("ALCEDO_SESSION", None)
        environment.update(self.env)
        return environment

    def _cli(self, args: list[str], stdin: bytes | None = None,
             timeout_s: float | None = None) -> subprocess.CompletedProcess:
        command = [str(self.cli_path), "--session-dir", str(self.run_dir / "sessions"),
                   "--session", self.name, "--json", *args]
        try:
            return subprocess.run(command, input=stdin, capture_output=True,
                                  timeout=timeout_s or self.call_timeout_ms / 1000 + 60,
                                  env=self._environment())
        except subprocess.TimeoutExpired as error:
            raise HostFailure(f"alcedo-cli {' '.join(args[:2])} did not end in "
                              f"{error.timeout} s") from error

    def start(self, project: Path | None = None, create: tuple[Path, str] | None = None,
              watcher: str = "off") -> dict:
        self.run_dir.mkdir(parents=True, exist_ok=True)
        self.starts += 1
        log_file = self.run_dir / f"host_{self.starts}.log"
        self.log_files.append(log_file)
        args = ["--timeout", "600000", "session", "start", "--headless"]
        if project is not None:
            args += ["--project", str(project)]
        else:
            assert create is not None
            args += ["--create", f"{create[0]},{create[1]}"]
        args += ["--settings-dir", str(self.run_dir / "settings"), "--log-file", str(log_file),
                 "--host-binary", str(self.host_path), "--mount-watcher", watcher]
        result = self._cli(args, timeout_s=700)
        if result.returncode != CLI_OK:
            raise HostFailure("session start failed: "
                              f"{result.stdout.decode('utf-8', 'replace')}"
                              f"{result.stderr.decode('utf-8', 'replace')}")
        self.started = True
        started = json.loads(result.stdout.decode("utf-8"))
        self.pid = started.get("pid")
        return started

    @staticmethod
    def _answer_of(obj: dict) -> Answer:
        if "error" in obj and obj["error"] is not None:
            return Answer(error=obj["error"])
        return Answer(result=obj.get("result", obj))

    def call(self, method: str, params: dict | None = None,
             timeout_ms: int | None = None) -> Answer:
        timeout_ms = timeout_ms or self.call_timeout_ms
        result = self._cli(["--timeout", str(timeout_ms), "call", method,
                            json.dumps(params or {}, ensure_ascii=True)],
                           timeout_s=timeout_ms / 1000 + 60)
        text = result.stdout.decode("utf-8", "replace").strip()
        if result.returncode not in (CLI_OK, CLI_COMMAND_ERROR) or not text:
            raise HostFailure(f"{method}: alcedo-cli exit {result.returncode}: {text} "
                              f"{result.stderr.decode('utf-8', 'replace').strip()}")
        obj = json.loads(text)
        if result.returncode == CLI_OK and "error" not in obj:
            return Answer(result=obj)
        answer = Answer(error=obj.get("error", obj))
        if answer.code == TIMEOUT:
            raise HostFailure(f"{method} timed out after {timeout_ms} ms: {obj}")
        return answer

    def batch(self, requests: list[tuple[str, str, dict]],
              timeout_ms: int | None = None) -> dict[str, Answer]:
        """Sends (id, method, params) requests over one connection."""
        timeout_ms = timeout_ms or self.call_timeout_ms
        lines = "".join(json.dumps({"id": rid, "method": method, "params": params},
                                   ensure_ascii=False) + "\n" for rid, method, params in requests)
        result = self._cli(["--timeout", str(timeout_ms), "batch"], stdin=lines.encode("utf-8"),
                           timeout_s=timeout_ms / 1000 * max(1, len(requests)) + 60)
        if result.returncode not in (CLI_OK, CLI_COMMAND_ERROR):
            raise HostFailure(f"batch: alcedo-cli exit {result.returncode}: "
                              f"{result.stderr.decode('utf-8', 'replace').strip()}")
        answers: dict[str, Answer] = {}
        for line in result.stdout.decode("utf-8", "replace").splitlines():
            if line.strip():
                obj = json.loads(line)
                answers[str(obj.pop("id"))] = self._answer_of(obj)
        missing = [rid for rid, _, _ in requests if rid not in answers]
        if missing:
            raise HostFailure(f"batch: no answer for {missing[:3]}")
        for rid, answer in answers.items():
            if answer.code == TIMEOUT:
                raise HostFailure(f"batch request {rid} timed out: {answer.error}")
        return answers

    def alive(self) -> bool:
        if self.pid is None:
            return False
        if IS_WINDOWS:
            result = subprocess.run(["tasklist", "/FI", f"PID eq {self.pid}", "/NH"],
                                    capture_output=True, text=True)
            return str(self.pid) in result.stdout
        try:
            os.kill(self.pid, 0)
        except OSError:
            return False
        return True

    def kill(self) -> None:
        """Ends the host process at once, as a crash does."""
        if self.pid is None:
            return
        if IS_WINDOWS:
            subprocess.run(["taskkill", "/F", "/PID", str(self.pid)], capture_output=True)
        else:
            try:
                os.kill(self.pid, signal.SIGKILL)
            except OSError:
                pass
        for _ in range(100):
            if not self.alive():
                break
            time.sleep(0.1)
        self.started = False
        self.pid = None
        # The session record of the killed host is stale.
        self._cli(["session", "prune"], timeout_s=60)

    def stop(self) -> None:
        if self.started:
            try:
                self._cli(["session", "stop"], timeout_s=600)
            except HostFailure:
                self.kill()
            self.started = False
            self.pid = None
