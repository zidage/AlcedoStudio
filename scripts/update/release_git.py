#!/usr/bin/env python3
"""Git identity helpers for signed update manifests."""

from __future__ import annotations

from pathlib import Path
import re
import subprocess


FULL_COMMIT = re.compile(r"^[0-9a-f]{40}$")


def run_git(repo: Path, *arguments: str, check: bool = True) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        ["git", *arguments],
        cwd=repo,
        check=check,
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
    )


def validate_commit(value: str) -> str:
    commit = value.strip().lower()
    if not FULL_COMMIT.fullmatch(commit):
        raise SystemExit(f"git commit must be a 40-character lowercase SHA: {value}")
    return commit


def read_head_commit(repo: Path) -> str:
    result = run_git(repo, "rev-parse", "HEAD")
    return validate_commit(result.stdout.strip())


def require_clean_worktree(repo: Path) -> None:
    result = run_git(repo, "status", "--porcelain")
    if result.stdout.strip():
        raise SystemExit(
            "refusing to write a release commit into the manifest from a dirty worktree"
        )


def origin_main_ref(repo: Path) -> str:
    for candidate in ("origin/main", "refs/remotes/origin/main"):
        result = run_git(repo, "rev-parse", "--verify", candidate, check=False)
        if result.returncode == 0:
            return candidate
    raise SystemExit("origin/main is not available; fetch origin before publishing a stable update")


def commit_is_on_origin_main(repo: Path, commit: str) -> bool:
    validate_commit(commit)
    result = run_git(
        repo,
        "merge-base",
        "--is-ancestor",
        commit,
        origin_main_ref(repo),
        check=False,
    )
    return result.returncode == 0


def require_commit_on_origin_main(repo: Path, commit: str) -> None:
    if commit_is_on_origin_main(repo, commit):
        return
    raise SystemExit(
        f"stable updates must be built from a commit already on origin/main ({commit} is not). "
        "Merge first, then package and upload from that merge commit."
    )
