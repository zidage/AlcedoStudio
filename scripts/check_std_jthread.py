#!/usr/bin/env python3
#  Copyright 2026 Yurun Zi
#  SPDX-License-Identifier: GPL-3.0-only
#  Additional permission under GPLv3 section 7 applies; see the LICENSE file.
"""Check that project code does not use std::jthread or the std::stop_token family.

Apple libc++ on the macOS CI target puts ``std::jthread``, ``std::stop_token``,
``std::stop_source``, and ``std::stop_callback`` behind ``-fexperimental-library``. Under
``-std=c++20`` they are not declared, so a use compiles with MSVC and fails the macOS build
(``no member named 'jthread' in namespace 'std'``). Use ``std::thread`` and join it explicitly,
and a ``std::atomic<bool>`` for a stop request.

Scans the C++, CUDA, and Objective-C++ sources of ``alcedo_studio/src`` and ``alcedo_studio/tests``
outside the vendored ``third_party/`` and the resources in ``config/``. A finding is a line that
names one of these types or includes ``<stop_token>``. Text after ``//`` is not scanned, so a
comment can name the types.

Exit status: 0 without findings, 1 with findings (each printed as ``path:line: rule: text``),
2 when a source root does not exist.
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

# Directory names that are not scanned at any depth: the vendored third-party sources, and the
# resources in config/, whose reference sources are copies of external implementations.
SKIPPED_DIRECTORY_NAMES = {"third_party", "config"}


@dataclass(frozen=True)
class Rule:
    name: str
    pattern: re.Pattern[str]


RULES = (
    Rule("std::jthread", re.compile(r"\bstd::jthread\b")),
    Rule("std::stop_token", re.compile(r"\bstd::stop_token\b")),
    Rule("std::stop_source", re.compile(r"\bstd::stop_source\b")),
    Rule("std::stop_callback", re.compile(r"\bstd::stop_callback\b")),
    Rule("<stop_token>", re.compile(r"#\s*include\s*<stop_token>")),
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
        subdirectories[:] = sorted(d for d in subdirectories if d not in SKIPPED_DIRECTORY_NAMES)
        relative_directory = Path(directory).relative_to(source_root)
        for name in sorted(names):
            if Path(name).suffix.lower() in SOURCE_SUFFIXES:
                files.append(relative_directory / name)
    return files


def code_part(line: str) -> str:
    """@p line without its trailing ``//`` comment."""
    index = line.find("//")
    return line if index < 0 else line[:index]


def scan(source_root: Path) -> list[Finding]:
    findings: list[Finding] = []
    for relative in source_files(source_root):
        text = (source_root / relative).read_text(encoding="utf-8", errors="replace")
        for number, line in enumerate(text.splitlines(), start=1):
            code = code_part(line)
            for rule in RULES:
                if rule.pattern.search(code):
                    findings.append(Finding(relative, number, rule.name, line.strip()))
    return findings


def main(argv: list[str] | None = None) -> int:
    repository = Path(__file__).resolve().parent.parent
    default_roots = [repository / "alcedo_studio" / "src", repository / "alcedo_studio" / "tests"]
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument(
        "--source-root",
        type=Path,
        action="append",
        dest="source_roots",
        help="source root to scan; repeat for more than one "
        "(default: alcedo_studio/src and alcedo_studio/tests of this repository)",
    )
    args = parser.parse_args(argv)
    source_roots: list[Path] = args.source_roots or default_roots
    for source_root in source_roots:
        if not source_root.is_dir():
            print(f"check_std_jthread: source root not found: {source_root}", file=sys.stderr)
            return 2

    count = 0
    for source_root in source_roots:
        for finding in scan(source_root):
            count += 1
            path = (source_root / finding.path).relative_to(source_root.parent).as_posix()
            print(f"{path}:{finding.line}: {finding.rule}: {finding.text}")
    if count:
        print(
            f"check_std_jthread: {count} finding(s). Apple libc++ does not declare std::jthread "
            "or std::stop_token under -std=c++20: use std::thread with an explicit join and "
            "std::atomic<bool> for a stop request.",
            file=sys.stderr,
        )
        return 1
    roots = ", ".join(str(root) for root in source_roots)
    print(f"check_std_jthread: no findings in {roots}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
