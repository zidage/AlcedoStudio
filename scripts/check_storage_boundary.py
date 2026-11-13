#!/usr/bin/env python3
#  Copyright 2026 Yurun Zi
#  SPDX-License-Identifier: GPL-3.0-only
#  Additional permission under GPLv3 section 7 applies; see the LICENSE file.
"""Check that code outside the storage layer does not reach the project database directly.

Scans the C++ and Objective-C++ sources of ``alcedo_studio/src`` outside ``storage/``,
``include/storage/``, and the vendored ``third_party/``. The Storage facade
(``sleeve/storage.cpp`` and ``include/sleeve/storage.hpp``) belongs to the Storage target and is
not scanned. A finding is a line that:

- includes ``duckdb.h``;
- includes a header under ``storage/mapper/`` (duckorm and the mappers);
- calls a DuckDB C API function (``duckdb_<name>(``);
- reads a connection (``GetConnectionGuard``);
- names the ``duckorm::`` namespace;
- names ``StorageTestAccess``, the database access of the test fixtures.

Code outside storage reaches the database through the stores of ``Storage``,
``Storage::OpenSession``, and ``Storage::Maintenance``. See
docs/roadmap/alcedo_studio/storage/storage_query_boundary_plan.md (Phase S6).

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

SOURCE_SUFFIXES = {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".inl", ".ipp", ".m", ".mm"}

# Directories (relative to the source root) that are not scanned: the storage layer, and the
# vendored third-party sources, which are not Alcedo code.
SKIPPED_DIRECTORIES = {Path("storage"), Path("include") / "storage", Path("third_party")}

# The Storage facade is compiled into the Storage target (alcedo_studio/src/storage/CMakeLists.txt).
STORAGE_FACADE_FILES = {Path("sleeve") / "storage.cpp", Path("include") / "sleeve" / "storage.hpp"}


@dataclass(frozen=True)
class Rule:
    name: str
    pattern: re.Pattern[str]


RULES = (
    Rule("duckdb.h include", re.compile(r'#\s*include\s*[<"]duckdb\.h[>"]')),
    Rule("storage/mapper include", re.compile(r'#\s*include\s*[<"]storage/mapper/')),
    Rule("DuckDB C API call", re.compile(r"\bduckdb_[a-z_]+\s*\(")),
    Rule("connection read", re.compile(r"\bGetConnectionGuard\b")),
    Rule("duckorm reference", re.compile(r"\bduckorm::")),
    Rule("test database access", re.compile(r"\bStorageTestAccess\b")),
)


@dataclass(frozen=True)
class Finding:
    path: Path
    line: int
    rule: str
    text: str


def is_skipped_directory(relative: Path) -> bool:
    return relative in SKIPPED_DIRECTORIES


def source_files(source_root: Path) -> list[Path]:
    """Source files under @p source_root outside the skipped directories, sorted."""
    files: list[Path] = []
    for directory, subdirectories, names in os.walk(source_root):
        relative_directory = Path(directory).relative_to(source_root)
        subdirectories[:] = sorted(
            d for d in subdirectories if not is_skipped_directory(relative_directory / d)
        )
        for name in sorted(names):
            relative = relative_directory / name
            if Path(name).suffix.lower() not in SOURCE_SUFFIXES:
                continue
            if relative not in STORAGE_FACADE_FILES:
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
        print(f"check_storage_boundary: source root not found: {source_root}", file=sys.stderr)
        return 2

    findings = scan(source_root)
    for finding in findings:
        print(f"{finding.path.as_posix()}:{finding.line}: {finding.rule}: {finding.text}")
    if findings:
        print(
            f"check_storage_boundary: {len(findings)} finding(s). Code outside storage must use "
            "the stores of Storage, Storage::OpenSession, or Storage::Maintenance.",
            file=sys.stderr,
        )
        return 1
    print(f"check_storage_boundary: no findings in {source_root}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
