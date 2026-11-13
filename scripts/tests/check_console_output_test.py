#!/usr/bin/env python3
#  Copyright 2026 Yurun Zi
#  SPDX-License-Identifier: GPL-3.0-only
#  Additional permission under GPLv3 section 7 applies; see the LICENSE file.
"""Exit status and findings of scripts/check_console_output.py on small source trees."""

from __future__ import annotations

import contextlib
import io
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

import check_console_output  # noqa: E402


class CheckConsoleOutputTest(unittest.TestCase):
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
            status = check_console_output.main(["--source-root", str(self.root)])
        return status, out.getvalue()

    def test_each_rule_reports_its_line(self) -> None:
        self.write(
            "edit/runtime/op.cpp",
            'std::cout << "a";\n'
            'std::cerr << "b";\n'
            'std::clog << "c";\n'
            'std::fprintf(stderr, "d");\n'
            'fprintf( stderr, "e");\n',
        )
        status, out = self.run_check()
        self.assertEqual(status, 1)
        for line, rule in enumerate(
            ("std::cout", "std::cerr", "std::clog", "fprintf(stderr", "fprintf(stderr"), start=1
        ):
            self.assertIn(f"edit/runtime/op.cpp:{line}: {rule}", out)

    def test_cuda_and_objective_cpp_sources_are_scanned(self) -> None:
        self.write("decoders/kernel.cu", 'std::cout << "x";\n')
        self.write("edit/runtime/metal/backend.mm", 'std::cerr << "y";\n')
        status, out = self.run_check()
        self.assertEqual(status, 1)
        self.assertIn("decoders/kernel.cu:1: std::cout", out)
        self.assertIn("edit/runtime/metal/backend.mm:1: std::cerr", out)

    def test_program_output_files_and_third_party_are_not_scanned(self) -> None:
        self.write("tools/gpu_kernel_check.cpp", 'std::cout << "gpu.kernels.ok";\n')
        self.write("ui/alcedo_studio_test_host/main.cpp", 'std::cout << "PROBE_SOCKET=";\n')
        self.write("third_party/lib/a.cpp", 'std::cerr << "vendor";\n')
        status, out = self.run_check()
        self.assertEqual(status, 0)
        self.assertEqual(out.count(": std::"), 0)

    def test_log_macros_and_names_that_contain_cout_pass(self) -> None:
        self.write(
            "cuda/nn/conv2d.cu",
            "int cout = 8;\n"
            'ALCEDO_LOG_WARN(Gpu) << "cudaMalloc failed";\n'
            "std::fwrite(bytes, 1, size, stderr);\n",
        )
        status, _ = self.run_check()
        self.assertEqual(status, 0)

    def test_missing_source_root_exits_with_two(self) -> None:
        with contextlib.redirect_stderr(io.StringIO()):
            status = check_console_output.main(["--source-root", str(self.root / "absent")])
        self.assertEqual(status, 2)

    def test_repository_source_tree_has_no_findings(self) -> None:
        source_root = Path(__file__).resolve().parents[2] / "alcedo_studio" / "src"
        out = io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(io.StringIO()):
            status = check_console_output.main(["--source-root", str(source_root)])
        self.assertEqual(status, 0, out.getvalue())


if __name__ == "__main__":
    unittest.main()
