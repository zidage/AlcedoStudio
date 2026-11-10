#!/usr/bin/env python3
#  Copyright 2026 Yurun Zi
#  SPDX-License-Identifier: GPL-3.0-only
#  Additional permission under GPLv3 section 7 applies; see the LICENSE file.
"""Exit status and findings of scripts/check_storage_boundary.py on small source trees."""

from __future__ import annotations

import contextlib
import io
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

import check_storage_boundary  # noqa: E402


class CheckStorageBoundaryTest(unittest.TestCase):
    def setUp(self) -> None:
        self._temp = tempfile.TemporaryDirectory()
        self.root = Path(self._temp.name)

    def tearDown(self) -> None:
        self._temp.cleanup()

    def write(self, relative: str, text: str) -> None:
        path = self.root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")

    def run_check(self) -> tuple[int, str]:
        out = io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(io.StringIO()):
            status = check_storage_boundary.main(["--source-root", str(self.root)])
        return status, out.getvalue()

    def test_duckdb_query_call_in_app_fails(self) -> None:
        self.write(
            "app/service.cpp", "void F(duckdb_connection c) {\n  duckdb_query(c, sql, &r);\n}\n"
        )
        status, out = self.run_check()
        self.assertEqual(status, 1)
        self.assertIn("app/service.cpp:2: DuckDB C API call", out)

    def test_each_rule_reports_its_line(self) -> None:
        self.write(
            "ui/view.cpp",
            "#include <duckdb.h>\n"
            '#include "storage/mapper/duckorm/duckdb_orm.hpp"\n'
            "auto g = db.GetConnectionGuard();\n"
            "duckorm::SqlFragment f;\n"
            "StorageTestAccess::GetDatabase(s);\n",
        )
        status, out = self.run_check()
        self.assertEqual(status, 1)
        for line, rule in enumerate(
            (
                "duckdb.h include",
                "storage/mapper include",
                "connection read",
                "duckorm reference",
                "test database access",
            ),
            start=1,
        ):
            self.assertIn(f"ui/view.cpp:{line}: {rule}", out)

    def test_storage_layer_facade_and_third_party_are_not_scanned(self) -> None:
        call = "void F() { duckdb_query(c, sql, &r); auto g = db.GetConnectionGuard(); }\n"
        self.write("storage/store/store.cpp", call)
        self.write("include/storage/store/store.hpp", "#include <duckdb.h>\n")
        self.write("sleeve/storage.cpp", call)
        self.write("include/sleeve/storage.hpp", '#include "storage/mapper/mapper.hpp"\n')
        self.write("third_party/vendor/src/client.cc", call)
        self.write("app/service.cpp", "auto session = storage.OpenSession();\n")
        status, out = self.run_check()
        self.assertEqual(status, 0, out)

    def test_other_sleeve_file_is_scanned(self) -> None:
        self.write("sleeve/sleeve_filter/filter.cpp", "duckorm::SqlFragment f;\n")
        status, out = self.run_check()
        self.assertEqual(status, 1)
        self.assertIn("sleeve/sleeve_filter/filter.cpp:1: duckorm reference", out)

    def test_similar_names_are_not_findings(self) -> None:
        self.write(
            "app/service.cpp",
            "// The package keeps the duckdb file.\n"
            "auto LoadPackagedDuckDbExtensionName();\n"
            "my_duckdb_query(x);\n",
        )
        status, out = self.run_check()
        self.assertEqual(status, 0, out)

    def test_missing_source_root_exits_2(self) -> None:
        with contextlib.redirect_stderr(io.StringIO()):
            status = check_storage_boundary.main(["--source-root", str(self.root / "absent")])
        self.assertEqual(status, 2)

    def test_repository_sources_have_no_finding(self) -> None:
        source_root = Path(__file__).resolve().parents[2] / "alcedo_studio" / "src"
        with contextlib.redirect_stdout(io.StringIO()) as out:
            status = check_storage_boundary.main(["--source-root", str(source_root)])
        self.assertEqual(status, 0, out.getvalue())


if __name__ == "__main__":
    unittest.main()
