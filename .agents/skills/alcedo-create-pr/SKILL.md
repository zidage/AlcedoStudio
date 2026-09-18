---
name: alcedo-create-pr
description: >
  Write and open Alcedo Studio GitHub pull requests in English with a fixed
  Why / Changes / Verification body. Use when the user asks to create a PR,
  open a PR, submit a PR, write a pull-request description, or runs
  /alcedo-create-pr.
---

# Alcedo Create Pull Request

Create the GitHub pull request for the current branch. Write the title and the
body in English. Load `simplified-technical-english-asd-ste100` and apply those
rules to the title and the body. Do not put the STE rewrite table in the pull
request.

## Title

```text
purpose(scope): Summary of changes
```

`purpose` is one of: `feat`, `refact`, `docs`, `misc`, `fix`.

`scope` names one product module. Use a grain finer than `editor` / `ui` /
`pipeline`. Do not use a file path.

Do not put a phase id in the title (`NM8.1`, `Phase 4`, `6C-5`, and the like).

Use the same title form for the primary commit message.

## Body order

Use these headings in this order. Use no other headings.

### Plan

Include this heading only when the pull request implements a roadmap phase.

- Link the plan document.
- Name the phase id here. Do not put the phase id in the title.
- State that this pull request implements that phase.

### Why

Write Why as a Markdown bullet list. One requirement per bullet. Do not write
a paragraph of stacked sentences.

- **Plan phase:** rewrite that phase's requirements as bullets. Include extra
  requirements the user added in the same conversation.
- **No plan:** paraphrase the user prompt as bullets. Do not copy the prompt
  as a quote.

### Changes

Group work by module. Grain: finer than `editor` / `ui` / `pipeline`, coarser
than a file path. Examples of the right grain: pending-input queue, session
consume, static plan cache, plan executor, grade executor, local-tone
executor, CUDA develop encode, texture pool, preview-performance diagnostics.

For each module write:

1. The design.
2. The implementation.

If a plan exists, restate the plan requirements this pull request implements.
Keep that restatement short.

Do not list file paths. GitHub lists files.

### Verification

If the plan requires measured numbers, write those numbers.

Then list every executed test. After each test name, write one sentence that
states the behavior the test checks.

## Forbidden text

Do not write any of the following:

- `git diff --check` or a claim that a diff check passed
- A list of changed files
- An `Out of Scope` heading or an out-of-scope paragraph

Work that Why and Changes do not name is not in this pull request. Do not say
that.

## GitHub steps

Run from the repository root.

1. Inspect `git status`, `git diff`, and `git log` against the base branch
   (`main` unless the user names another base).
2. Draft the title and the body with the rules above.
3. Commit remaining work. The primary commit title uses the same form as the
   pull-request title.
4. Push the branch to `origin`.
5. Create the pull request:

```text
gh pr create --base main --title "<title>" --body "<body>"
```

Point `--head` at the current branch when `gh` does not infer it.

6. Return the pull-request URL.
