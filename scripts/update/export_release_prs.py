#!/usr/bin/env python3
"""Export pull-request evidence between two build commits for LLM review."""

from __future__ import annotations

import argparse
from concurrent.futures import ThreadPoolExecutor, as_completed
import json
from pathlib import Path
import re
import subprocess
from typing import Any


REPO_ROOT = Path(__file__).resolve().parents[2]
PR_PATTERNS = (
    re.compile(r"\bMerge pull request #(\d+)\b"),
    re.compile(r"\(#(\d+)\)\s*$"),
)
GH_FIELDS = (
    "number,title,body,url,state,mergedAt,baseRefName,headRefName,"
    "mergeCommit,commits,author,labels"
)


def run(command: list[str], *, check: bool = True) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        command,
        cwd=REPO_ROOT,
        check=check,
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
    )


def git(*arguments: str) -> str:
    return run(["git", *arguments]).stdout.strip()


def pr_numbers_from_subjects(subjects: list[str]) -> set[int]:
    numbers: set[int] = set()
    for subject in subjects:
        for pattern in PR_PATTERNS:
            match = pattern.search(subject)
            if match:
                numbers.add(int(match.group(1)))
    return numbers


def commit_rows(revision_range: str) -> list[dict[str, str]]:
    output = git("log", "--reverse", "--format=%H%x09%s", revision_range)
    rows: list[dict[str, str]] = []
    for line in output.splitlines():
        commit, separator, subject = line.partition("\t")
        if separator:
            rows.append({"oid": commit, "subject": subject})
    return rows


def history_pr_numbers(ref: str) -> set[int]:
    output = git("log", "--format=%s", ref)
    return pr_numbers_from_subjects(output.splitlines())


def infer_pr_interval(base_ref: str, head_ref: str) -> tuple[int, int] | None:
    base_numbers = history_pr_numbers(base_ref)
    head_numbers = history_pr_numbers(head_ref)
    lower = max(base_numbers, default=0)
    upper = max(head_numbers, default=0)
    if upper <= lower:
        return None
    return lower + 1, upper


def fetch_pull_request(repository: str, number: int) -> dict[str, Any] | None:
    result = run(
        [
            "gh",
            "pr",
            "view",
            str(number),
            "--repo",
            repository,
            "--json",
            GH_FIELDS,
        ],
        check=False,
    )
    if result.returncode != 0:
        return None
    return json.loads(result.stdout)


def pull_request_oids(pull_request: dict[str, Any]) -> set[str]:
    commits = pull_request.get("commits") or []
    return {
        str(commit.get("oid", ""))
        for commit in commits
        if isinstance(commit, dict) and commit.get("oid")
    }


def inclusion_reasons(
    pull_request: dict[str, Any],
    range_oids: set[str],
    subject_pr_numbers: set[int],
) -> list[str]:
    reasons: list[str] = []
    number = int(pull_request.get("number", 0))
    merge_commit = pull_request.get("mergeCommit") or {}
    merge_oid = str(merge_commit.get("oid", "")) if isinstance(merge_commit, dict) else ""
    if merge_oid in range_oids:
        reasons.append("merge commit is in the build range")
    if pull_request_oids(pull_request) & range_oids:
        reasons.append("PR branch commit is in the build range")
    if number in subject_pr_numbers:
        reasons.append("a build-range commit names this PR")
    return reasons


def repository_name(explicit: str) -> str:
    if explicit:
        return explicit
    result = run(["gh", "repo", "view", "--json", "nameWithOwner"])
    value = json.loads(result.stdout).get("nameWithOwner", "")
    if not value:
        raise RuntimeError("cannot determine the GitHub repository; pass --repo OWNER/NAME")
    return str(value)


def main() -> int:
    parser = argparse.ArgumentParser(
        description=(
            "Export merged PR titles, bodies, branches, and commits between the Git refs "
            "that identify two builds. The JSON is review input, not publishable notes."
        )
    )
    parser.add_argument("--from-ref", required=True, help="Git ref for the previous build")
    parser.add_argument("--to-ref", required=True, help="Git ref for the new build")
    parser.add_argument("--build", required=True, type=int, help="New build number")
    parser.add_argument("--repo", default="", help="GitHub OWNER/NAME; auto-detected by gh")
    parser.add_argument("--output", type=Path, default=None)
    args = parser.parse_args()

    if args.build < 1:
        parser.error("build must be a positive integer")

    base_commit = git("rev-parse", "--verify", f"{args.from_ref}^{{commit}}")
    head_commit = git("rev-parse", "--verify", f"{args.to_ref}^{{commit}}")
    if run(["git", "merge-base", "--is-ancestor", base_commit, head_commit], check=False).returncode:
        parser.error("from-ref must be an ancestor of to-ref")

    commits = commit_rows(f"{base_commit}..{head_commit}")
    if not commits:
        parser.error("the selected build refs contain no new commits")
    range_oids = {row["oid"] for row in commits}
    subject_numbers = pr_numbers_from_subjects([row["subject"] for row in commits])
    pr_interval = infer_pr_interval(base_commit, head_commit)
    repository = repository_name(args.repo)

    pull_requests: list[dict[str, Any]] = []
    unavailable_numbers: list[int] = []
    if pr_interval is not None:
        first_pr, last_pr = pr_interval
        with ThreadPoolExecutor(max_workers=8) as executor:
            futures = {
                executor.submit(fetch_pull_request, repository, number): number
                for number in range(first_pr, last_pr + 1)
            }
            for future in as_completed(futures):
                number = futures[future]
                pull_request = future.result()
                if pull_request is None:
                    unavailable_numbers.append(number)
                    continue
                if not pull_request.get("mergedAt"):
                    continue
                reasons = inclusion_reasons(pull_request, range_oids, subject_numbers)
                if not reasons:
                    continue
                pull_request["selectionReasons"] = reasons
                pull_requests.append(pull_request)

    pull_requests.sort(key=lambda item: int(item["number"]))
    selected_numbers = {int(item["number"]) for item in pull_requests}
    missing_named = sorted(subject_numbers - selected_numbers)
    if missing_named:
        raise RuntimeError(
            "GitHub data did not confirm build-range PRs: "
            + ", ".join(f"#{number}" for number in missing_named)
        )

    output = args.output or (
        REPO_ROOT
        / "build"
        / "tmp"
        / "update"
        / "release-notes"
        / f"{args.build}-pr-review.json"
    )
    if not output.is_absolute():
        output = REPO_ROOT / output
    output.parent.mkdir(parents=True, exist_ok=True)
    payload = {
        "schema": 1,
        "purpose": "LLM-reviewed release-note source; do not publish this JSON directly",
        "build": args.build,
        "repository": repository,
        "fromRef": args.from_ref,
        "fromCommit": base_commit,
        "toRef": args.to_ref,
        "toCommit": head_commit,
        "candidatePrRange": (
            {"first": pr_interval[0], "last": pr_interval[1]}
            if pr_interval is not None
            else None
        ),
        "unavailableCandidateNumbers": sorted(unavailable_numbers),
        "commits": commits,
        "pullRequests": pull_requests,
        "reviewRequirements": [
            "Verify every selected PR belongs to this build range.",
            "Resolve duplicate, reverted, internal-only, and misleading commit messages.",
            "Describe user-visible outcomes in plain language; do not copy PR bodies blindly.",
            "Draft equivalent user-visible release notes in English and Simplified Chinese.",
            f"After approval, write docs/changelog/{args.build}.en.txt and "
            f"docs/changelog/{args.build}.zh-CN.txt together.",
            "Keep every line at 88 characters or fewer and use UTF-8 LF text.",
        ],
    }
    output.write_text(json.dumps(payload, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(output)
    if pr_interval is None:
        print("Selected 0 merged PRs; the build range contains direct commits only.")
    else:
        print(
            f"Selected {len(pull_requests)} merged PRs from "
            f"#{pr_interval[0]}..#{pr_interval[1]}."
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
