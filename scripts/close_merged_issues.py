#!/usr/bin/env python3
#  Copyright 2026 Yurun Zi
#  SPDX-License-Identifier: GPL-3.0-only
#  Additional permission under GPLv3 section 7 applies; see the LICENSE file.
"""Close the issues that the merged pull requests of a push to main name with a closing keyword.

GitHub closes an issue from ``Closes #N`` only when the pull request merges into the default
branch. A pull request of a base-chained stack merges into the branch below it, so its issues stay
open after its commits reach ``main``. The workflow ``close-merged-issues.yml`` runs this script on
each push to ``main``:

1. It lists the commits of the push (the compare of ``--before`` and ``--after``; only
   ``--after`` when ``--before`` is the zero SHA).
2. For each commit it reads the merged pull requests that GitHub associates with the commit.
3. It reads the closing keywords of each pull request body: ``close``, ``closes``, ``closed``,
   ``fix``, ``fixes``, ``fixed``, ``resolve``, ``resolves``, ``resolved``, then ``#N``,
   ``owner/repo#N`` of this repository, or the issue URL of this repository. Text in code blocks,
   inline code, and HTML comments is not read.
4. It closes each named issue that is still open, with a comment that names the pull request and
   the commit. A closed issue, a pull request, and an issue of another repository stay as they
   are.

Environment: ``GITHUB_TOKEN`` (issues: write, contents: read, pull-requests: read).

Exit status: 0 when each named open issue was closed (or, with ``--dry-run``, listed), 1 when a
request to GitHub failed, 2 for a usage error.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import sys
import urllib.error
import urllib.request
from dataclasses import dataclass, field
from typing import Any, Callable

ZERO_SHA = "0" * 40
API_ROOT = "https://api.github.com"

# Closing keywords of GitHub ("Linking a pull request to an issue"). A colon after the keyword is
# allowed, as GitHub allows it.
_KEYWORD = r"(?:close[sd]?|fix(?:e[sd])?|resolve[sd]?)"
_REFERENCE = r"(?:(?P<url>https://github\.com/(?P<url_repo>[\w.-]+/[\w.-]+)/issues/(?P<url_number>\d+))" \
             r"|(?:(?P<repo>[\w.-]+/[\w.-]+))?#(?P<number>\d+))"
_CLOSING = re.compile(rf"(?<![\w/-]){_KEYWORD}:?\s+{_REFERENCE}(?![\w/])", re.IGNORECASE)

_FENCED_CODE = re.compile(r"^[ \t]*(```|~~~).*?^[ \t]*\1[^\n]*$", re.DOTALL | re.MULTILINE)
_INLINE_CODE = re.compile(r"`[^`\n]*`")
_HTML_COMMENT = re.compile(r"<!--.*?-->", re.DOTALL)


def readable_text(body: str) -> str:
    """The text of a pull request body without code blocks, inline code, and HTML comments."""
    text = _FENCED_CODE.sub("", body)
    text = _HTML_COMMENT.sub("", text)
    return _INLINE_CODE.sub("", text)


def closing_issue_numbers(body: str | None, repository: str) -> list[int]:
    """The issue numbers of @p repository that @p body names with a closing keyword, in order of
    their first appearance."""
    if not body:
        return []
    numbers: list[int] = []
    for match in _CLOSING.finditer(readable_text(body)):
        if match.group("url"):
            target_repo = match.group("url_repo")
            number = int(match.group("url_number"))
        else:
            target_repo = match.group("repo") or repository
            number = int(match.group("number"))
        if target_repo.lower() != repository.lower():
            continue
        if number not in numbers:
            numbers.append(number)
    return numbers


@dataclass
class MergedPullRequest:
    number: int
    body: str
    commit: str  # A commit of the push that GitHub associates with the pull request.


@dataclass
class Plan:
    """What the script does for one push: the issues to close, and why each other one stays."""

    to_close: list[tuple[int, MergedPullRequest]] = field(default_factory=list)
    skipped: list[tuple[int, int, str]] = field(default_factory=list)  # (issue, pull, reason)


class GitHub:
    """The REST requests of the script. Tests replace @ref request."""

    def __init__(self, repository: str, token: str,
                 request: Callable[[str, str, dict | None], Any] | None = None) -> None:
        self.repository = repository
        self._token = token
        self.request = request or self._urllib_request

    def _urllib_request(self, method: str, path: str, payload: dict | None) -> Any:
        data = json.dumps(payload).encode("utf-8") if payload is not None else None
        req = urllib.request.Request(f"{API_ROOT}{path}", data=data, method=method)
        req.add_header("Accept", "application/vnd.github+json")
        req.add_header("X-GitHub-Api-Version", "2022-11-28")
        req.add_header("User-Agent", "close-merged-issues")
        if self._token:
            req.add_header("Authorization", f"Bearer {self._token}")
        if data is not None:
            req.add_header("Content-Type", "application/json")
        with urllib.request.urlopen(req, timeout=60) as response:
            text = response.read().decode("utf-8")
        return json.loads(text) if text else None

    def push_commits(self, before: str, after: str) -> list[str]:
        """The commit SHAs of the push, oldest first."""
        if before == ZERO_SHA:
            return [after]
        commits: list[str] = []
        page = 1
        while True:
            result = self.request(
                "GET", f"/repos/{self.repository}/compare/{before}...{after}"
                       f"?per_page=100&page={page}", None)
            batch = [commit["sha"] for commit in result.get("commits", [])]
            commits.extend(sha for sha in batch if sha not in commits)
            if len(batch) < 100:
                break
            page += 1
        return commits

    def merged_pull_requests(self, commit: str) -> list[dict]:
        result = self.request("GET", f"/repos/{self.repository}/commits/{commit}/pulls", None)
        return [pull for pull in result if pull.get("merged_at")]

    def issue(self, number: int) -> dict:
        return self.request("GET", f"/repos/{self.repository}/issues/{number}", None)

    def close_issue(self, number: int, comment: str) -> None:
        self.request("POST", f"/repos/{self.repository}/issues/{number}/comments",
                     {"body": comment})
        self.request("PATCH", f"/repos/{self.repository}/issues/{number}",
                     {"state": "closed", "state_reason": "completed"})


def collect_pull_requests(github: GitHub, commits: list[str]) -> list[MergedPullRequest]:
    """The merged pull requests of @p commits, each once, in order of their first commit."""
    pulls: dict[int, MergedPullRequest] = {}
    for commit in commits:
        for pull in github.merged_pull_requests(commit):
            number = int(pull["number"])
            if number not in pulls:
                pulls[number] = MergedPullRequest(number, pull.get("body") or "", commit)
    return list(pulls.values())


def plan_closes(github: GitHub, pulls: list[MergedPullRequest]) -> Plan:
    plan = Plan()
    seen: set[int] = set()
    for pull in pulls:
        for number in closing_issue_numbers(pull.body, github.repository):
            if number in seen:
                continue
            seen.add(number)
            issue = github.issue(number)
            if "pull_request" in issue:
                plan.skipped.append((number, pull.number, "is a pull request"))
            elif issue.get("state") != "open":
                plan.skipped.append((number, pull.number, "is closed"))
            else:
                plan.to_close.append((number, pull))
    return plan


def close_comment(pull: MergedPullRequest) -> str:
    return (f"#{pull.number} names this issue with a closing keyword, and its commits are on "
            f"`main` ({pull.commit[:9]}). GitHub did not close the issue (a pull request that "
            f"merges into another branch closes nothing), so the close-merged-issues workflow "
            f"closes it.")


def run(github: GitHub, before: str, after: str, dry_run: bool, out=sys.stdout) -> None:
    commits = github.push_commits(before, after)
    pulls = collect_pull_requests(github, commits)
    plan = plan_closes(github, pulls)
    print(f"{len(commits)} commits, {len(pulls)} merged pull requests", file=out)
    for number, pull_number, reason in plan.skipped:
        print(f"#{number} (from #{pull_number}): {reason}", file=out)
    for number, pull in plan.to_close:
        if dry_run:
            print(f"#{number} (from #{pull.number}): would close", file=out)
            continue
        github.close_issue(number, close_comment(pull))
        print(f"#{number} (from #{pull.number}): closed", file=out)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--repository", required=True, help="owner/name")
    parser.add_argument("--before", required=True, help="SHA of main before the push")
    parser.add_argument("--after", required=True, help="SHA of main after the push")
    parser.add_argument("--dry-run", action="store_true",
                        help="List the issues to close; change nothing")
    args = parser.parse_args(argv)

    token = os.environ.get("GITHUB_TOKEN", "")
    if not token and not args.dry_run:
        print("close_merged_issues: GITHUB_TOKEN is not set", file=sys.stderr)
        return 2
    try:
        run(GitHub(args.repository, token), args.before, args.after, args.dry_run)
    except urllib.error.HTTPError as error:
        print(f"close_merged_issues: {error.code} {error.reason} for {error.url}", file=sys.stderr)
        return 1
    except urllib.error.URLError as error:
        print(f"close_merged_issues: {error.reason}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
