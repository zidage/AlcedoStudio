#!/usr/bin/env python3
#  Copyright 2026 Yurun Zi
#  SPDX-License-Identifier: GPL-3.0-only
#  Additional permission under GPLv3 section 7 applies; see the LICENSE file.
"""Exit status and findings of scripts/check_std_jthread.py on small source trees."""

from __future__ import annotations

import contextlib
import io
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

import check_std_jthread  # noqa: E402


class CheckStdJthreadTest(unittest.TestCase):
    def setUp(self) -> None:
        self._temp = tempfile.TemporaryDirectory()
        self.root = Path(self._temp.name)
        self.src = self.root / "src"
        self.tests = self.root / "tests"
        self.src.mkdir()
        self.tests.mkdir()

    def tearDown(self) -> None:
        self._temp.cleanup()

    def write(self, relative: str, text: str) -> None:
        path = self.root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")

    def run_check(self) -> tuple[int, str]:
        out = io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(io.StringIO()):
            status = check_std_jthread.main(
                ["--source-root", str(self.src), "--source-root", str(self.tests)]
            )
        return status, out.getvalue()

    def test_each_rule_reports_its_line(self) -> None:
        self.write(
            "src/ui/worker.cpp",
            "#include <stop_token>\n"
            "std::vector<std::jthread> threads;\n"
            "void Run(std::stop_token token);\n"
            "std::stop_source source;\n"
            "std::stop_callback callback(token, [] {});\n",
        )
        status, out = self.run_check()
        self.assertEqual(status, 1)
        for line, rule in enumerate(
            ("<stop_token>", "std::jthread", "std::stop_token", "std::stop_source",
             "std::stop_callback"),
            start=1,
        ):
            self.assertIn(f"src/ui/worker.cpp:{line}: {rule}", out)

    def test_tests_and_objective_cpp_sources_are_scanned(self) -> None:
        self.write("tests/app/store_test.cpp", "std::jthread worker([] {});\n")
        self.write("src/edit/metal/backend.mm", "std::jthread worker([] {});\n")
        status, out = self.run_check()
        self.assertEqual(status, 1)
        self.assertIn("tests/app/store_test.cpp:1: std::jthread", out)
        self.assertIn("src/edit/metal/backend.mm:1: std::jthread", out)

    def test_comments_third_party_and_thread_pass(self) -> None:
        self.write(
            "src/ui/worker.cpp",
            "std::thread worker([] {});  // not std::jthread: Apple libc++\n"
            "// Apple libc++ has no std::jthread.\n"
            "std::atomic<bool> stop_requested{false};\n",
        )
        self.write("src/third_party/lib/a.cpp", "std::jthread vendor;\n")
        status, out = self.run_check()
        self.assertEqual(status, 0)
        self.assertNotIn(": std::jthread", out)

    def test_missing_source_root_exits_2(self) -> None:
        with contextlib.redirect_stderr(io.StringIO()):
            status = check_std_jthread.main(["--source-root", str(self.root / "absent")])
        self.assertEqual(status, 2)


if __name__ == "__main__":
    unittest.main()
