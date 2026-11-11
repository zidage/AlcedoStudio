#!/usr/bin/env python3
#  Copyright 2026 Yurun Zi
#  SPDX-License-Identifier: GPL-3.0-only
#  Additional permission under GPLv3 section 7 applies; see the LICENSE file.
"""Check that production code writes diagnostics through the log, not to stdout or stderr.

Scans the C++, CUDA, and Objective-C++ sources of ``alcedo_studio/src`` outside the vendored
``third_party/`` and the resources in ``config/``. A finding is a line that names ``std::cout``, ``std::cerr``, or ``std::clog``, or
that calls ``fprintf(stderr``.

``alcedo_main`` is a Windows GUI program: in a release build stdout and stderr have no console, so
such a line is lost. Core libraries log with ``ALCEDO_LOG_*`` (``utils/logging/log.hpp``), Qt code with
``qC*`` and a category of ``utils/diagnostics/app_logging.hpp`` (issue #822).

The files in ``PROGRAM_OUTPUT_FILES`` write program output, such as a protocol line, help text, or
tool results, and are not scanned.

Exit status: 0 without findings, 1 with findings (each printed as ``path:line: rule: text``),
2 when the source root does not exist.
"""

from __future__ import annotations

import argparse
import os
import re
import sys
from dataclasses import dataclass
from pathlib import Path

SOURCE_SUFFIXES = {
    ".c",
    ".cc",
    ".cpp",
    ".cu",
    ".cuh",
    ".cxx",
    ".h",
    ".hh",
    ".hpp",
    ".inl",
    ".ipp",
    ".m",
    ".mm",
}

# Directories (relative to the source root) that are not scanned: the vendored third-party
# sources, and the resources in config/, whose reference sources (ref_impl/, CLFs/) are copies of
# external implementations outside the build.
SKIPPED_DIRECTORIES = {Path("third_party"), Path("config")}

# Program output: stdout and stderr are the interface of these programs.
PROGRAM_OUTPUT_FILES = {
    Path("tools") / "gpu_kernel_check.cpp",  # kernel verification results
    Path("ui") / "alcedo_studio_test_host" / "main.cpp",  # --help, version, PROBE_SOCKET=
    Path("ui") / "alcedo_main" / "main.cpp",  # qml.imports.ok probe
    Path("app") / "update_signer_main.cpp",  # signing tool
    Path("ui") / "editor_rhi" / "editor_rhi_harness_main.cpp",  # presentation harness report
}


@dataclass(frozen=True)
class Rule:
    name: str
    pattern: re.Pattern[str]


RULES = (
    Rule("std::cout", re.compile(r"\bstd::cout\b")),
    Rule("std::cerr", re.compile(r"\bstd::cerr\b")),
    Rule("std::clog", re.compile(r"\bstd::clog\b")),
    Rule("fprintf(stderr", re.compile(r"\bfprintf\s*\(\s*stderr\b")),
)


@dataclass(frozen=True)
class Finding:
    path: Path
    line: int
    rule: str
    text: str


def source_files(source_root: Path) -> list[Path]:
    """Source files under @p source_root outside the skipped directories, sorted."""
    files: list[Path] = []
    for directory, subdirectories, names in os.walk(source_root):
        relative_directory = Path(directory).relative_to(source_root)
        subdirectories[:] = sorted(
            d for d in subdirectories if relative_directory / d not in SKIPPED_DIRECTORIES
        )
        for name in sorted(names):
            relative = relative_directory / name
            if Path(name).suffix.lower() not in SOURCE_SUFFIXES:
                continue
            if relative not in PROGRAM_OUTPUT_FILES:
                files.append(relative)
    return files


def scan(source_root: Path) -> list[Finding]:
    findings: list[Finding] = []
    for relative in source_files(source_root):
        text = (source_root / relative).read_text(encoding="utf-8", errors="replace")
        for number, line in enumerate(text.splitlines(), start=1):
            for rule in RULES:
                if rule.pattern.search(line):
                    findings.append(Finding(relative, number, rule.name, line.strip()))
    return findings


def main(argv: list[str] | None = None) -> int:
    default_root = Path(__file__).resolve().parent.parent / "alcedo_studio" / "src"
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument(
        "--source-root",
        type=Path,
        default=default_root,
        help="source root to scan (default: alcedo_studio/src of this repository)",
    )
    args = parser.parse_args(argv)
    source_root: Path = args.source_root
    if not source_root.is_dir():
        print(f"check_console_output: source root not found: {source_root}", file=sys.stderr)
        return 2

    findings = scan(source_root)
    for finding in findings:
        print(f"{finding.path.as_posix()}:{finding.line}: {finding.rule}: {finding.text}")
    if findings:
        print(
            f"check_console_output: {len(findings)} finding(s). Write diagnostics with "
            "ALCEDO_LOG_* (utils/logging/log.hpp) or qC* with an alcedo.* category.",
            file=sys.stderr,
        )
        return 1
    print(f"check_console_output: no findings in {source_root}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
