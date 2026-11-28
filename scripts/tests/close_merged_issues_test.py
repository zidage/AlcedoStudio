#!/usr/bin/env python3
#  Copyright 2026 Yurun Zi
#  SPDX-License-Identifier: GPL-3.0-only
#  Additional permission under GPLv3 section 7 applies; see the LICENSE file.
"""Closing keyword parse and close plan of scripts/close_merged_issues.py against a fake GitHub."""

from __future__ import annotations

import io
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

import close_merged_issues as cmi  # noqa: E402

REPO = "zidage/AlcedoStudio"


class ClosingIssueNumbersTest(unittest.TestCase):
    def test_every_keyword_form_names_the_issue(self) -> None:
        for keyword in ("close", "closes", "closed", "fix", "fixes", "fixed", "resolve",
                        "resolves", "resolved", "Closes", "FIXES", "Resolves:"):
            with self.subTest(keyword=keyword):
                self.assertEqual(cmi.closing_issue_numbers(f"{keyword} #12", REPO), [12])

    def test_text_that_is_not_a_closing_keyword_names_no_issue(self) -> None:
        body = ("Issue: #1158. Tracking issue: #1152. Stacked on #1157.\n"
                "Part of #10. Refs #11. Found while: #12. prefixes #13. closes#14.")
        self.assertEqual(cmi.closing_issue_numbers(body, REPO), [])

    def test_repository_references_and_urls(self) -> None:
        body = ("Fixes zidage/AlcedoStudio#5\n"
                "Closes https://github.com/zidage/AlcedoStudio/issues/6\n"
                "Closes other/repo#7\n"
                "Closes https://github.com/other/repo/issues/8\n"
                "Closes zidage/alcedostudio#9")
        self.assertEqual(cmi.closing_issue_numbers(body, REPO), [5, 6, 9])

    def test_code_and_html_comments_are_not_read(self) -> None:
        body = ("Use `Closes #1` in the body.\n"
                "```text\nCloses #2\n```\n"
                "<!-- Closes #3 -->\n"
                "Closes #4")
        self.assertEqual(cmi.closing_issue_numbers(body, REPO), [4])

    def test_each_issue_once_in_order(self) -> None:
        body = "Closes #3\nFixes #1\nCloses #3\nResolves #2"
        self.assertEqual(cmi.closing_issue_numbers(body, REPO), [3, 1, 2])

    def test_empty_body(self) -> None:
        self.assertEqual(cmi.closing_issue_numbers(None, REPO), [])
        self.assertEqual(cmi.closing_issue_numbers("", REPO), [])


class FakeGitHub:
    """The REST answers of one push: compare pages, commit pulls, and issues."""

    def __init__(self, commits: list[str], pulls_by_commit: dict[str, list[dict]],
                 issues: dict[int, dict]) -> None:
        self.commits = commits
        self.pulls_by_commit = pulls_by_commit
        self.issues = issues
        self.writes: list[tuple[str, str, dict | None]] = []

    def __call__(self, method: str, path: str, payload: dict | None):
        prefix = f"/repos/{REPO}"
        self.assert_prefix(path, prefix)
        tail = path[len(prefix):]
        if method == "GET" and tail.startswith("/compare/"):
            page = int(tail.rsplit("page=", 1)[1])
            batch = self.commits[(page - 1) * 100:page * 100]
            return {"commits": [{"sha": sha} for sha in batch]}
        if method == "GET" and tail.startswith("/commits/") and tail.endswith("/pulls"):
            return self.pulls_by_commit.get(tail.split("/")[2], [])
        if method == "GET" and tail.startswith("/issues/"):
            return self.issues[int(tail.split("/")[2])]
        self.writes.append((method, tail, payload))
        if method == "PATCH":
            self.issues[int(tail.split("/")[2])]["state"] = payload["state"]
        return {}

    @staticmethod
    def assert_prefix(path: str, prefix: str) -> None:
        if not path.startswith(prefix):
            raise AssertionError(f"request outside the repository: {path}")


def merged(number: int, body: str) -> dict:
    return {"number": number, "body": body, "merged_at": "2026-11-27T00:00:00Z"}


class RunTest(unittest.TestCase):
    def run_push(self, fake: FakeGitHub, before: str = "a" * 40, dry_run: bool = False) -> str:
        out = io.StringIO()
        cmi.run(cmi.GitHub(REPO, "token", request=fake), before, "f" * 40, dry_run, out=out)
        return out.getvalue()

    def test_closes_open_issues_of_merged_pull_requests_of_the_push(self) -> None:
        fake = FakeGitHub(
            commits=["c1", "c2", "c3"],
            pulls_by_commit={
                "c1": [merged(100, "Closes #10")],
                # The second commit of the same pull request names it again.
                "c2": [merged(100, "Closes #10")],
                "c3": [merged(101, "Fixes #11\nCloses #12"),
                       {"number": 102, "body": "Closes #13", "merged_at": None}],
            },
            issues={10: {"state": "open"}, 11: {"state": "closed"}, 12: {"state": "open"},
                    13: {"state": "open"}})
        output = self.run_push(fake)
        closed = [tail for method, tail, _ in fake.writes if method == "PATCH"]
        self.assertEqual(closed, ["/issues/10", "/issues/12"])
        comments = [(tail, payload["body"]) for method, tail, payload in fake.writes
                    if method == "POST"]
        self.assertEqual([tail for tail, _ in comments], ["/issues/10/comments",
                                                          "/issues/12/comments"])
        self.assertIn("#100", comments[0][1])
        self.assertIn("c1", comments[0][1])
        self.assertIn("#101", comments[1][1])
        # The open pull request #102 closes nothing.
        self.assertEqual(fake.issues[13]["state"], "open")
        self.assertIn("#11 (from #101): is closed", output)

    def test_a_pull_request_reference_stays(self) -> None:
        fake = FakeGitHub(commits=["c1"],
                          pulls_by_commit={"c1": [merged(100, "Closes #50")]},
                          issues={50: {"state": "open", "pull_request": {}}})
        output = self.run_push(fake)
        self.assertEqual(fake.writes, [])
        self.assertIn("#50 (from #100): is a pull request", output)

    def test_dry_run_writes_nothing(self) -> None:
        fake = FakeGitHub(commits=["c1"],
                          pulls_by_commit={"c1": [merged(100, "Closes #10")]},
                          issues={10: {"state": "open"}})
        output = self.run_push(fake, dry_run=True)
        self.assertEqual(fake.writes, [])
        self.assertIn("#10 (from #100): would close", output)

    def test_compare_pages_are_read_to_the_end(self) -> None:
        commits = [f"c{index}" for index in range(230)]
        fake = FakeGitHub(commits=commits,
                          pulls_by_commit={"c229": [merged(100, "Closes #10")]},
                          issues={10: {"state": "open"}})
        output = self.run_push(fake)
        self.assertIn("230 commits, 1 merged pull requests", output)
        self.assertEqual(fake.issues[10]["state"], "closed")

    def test_zero_before_reads_only_the_after_commit(self) -> None:
        fake = FakeGitHub(commits=[],
                          pulls_by_commit={"f" * 40: [merged(100, "Closes #10")]},
                          issues={10: {"state": "open"}})
        self.run_push(fake, before=cmi.ZERO_SHA)
        self.assertEqual(fake.issues[10]["state"], "closed")


if __name__ == "__main__":
    unittest.main()
