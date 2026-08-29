---
name: execute-phase-plan
description: >
  Implement a user-specified phase (or sub-phase) from a multi-phase markdown roadmap/plan,
  building to grill-code-review standards during implementation, then writing completion status
  and primary call chains back into that phase section of the plan. Use when the user asks to
  complete a Phase from a plan doc, execute a roadmap phase, implement phase N of a plan,
  or runs /execute-phase-plan. Also use when a phase plan path is given with a scoped
  implementation request.
---

# Execute Phase Plan

Implement one scoped slice of a multi-phase plan document end-to-end: read the plan, implement
only the requested phase, build as if `grill-code-review` will audit the result, optionally run
that review, then append a completion record (status, call chains, proof) under that phase in the
plan.

This skill is the **execution** workflow. `grill-code-review` is the **acceptance/audit** standard
and may also be invoked after implementation for a formal review.

## When to Use

- User points at a plan path (for example under `docs/roadmap/.../*_plan.md`) and names a phase
  (for example `Phase 4`, `4A`, `6C-5`, `Phase 5B`).
- User says complete / implement / execute a phase from a plan.
- User runs `/execute-phase-plan`.

If there is **no** phase plan document, still implement the requested scope, still build to
`grill-code-review` standards, and report call chains in the chat response. Skip plan write-back
unless the user provides a doc to update.

## Inputs to Resolve First

1. **Plan path** — absolute or repo-relative markdown plan. Prefer the path the user gave.
2. **Implementation scope** — exact phase / sub-phase IDs the user named. Do not expand into later
   phases unless the user explicitly asks.
3. **Out of scope** — anything the plan marks as later, blocked, or dependent on unfinished prior
   work. State those blockers as engineering facts; do not soft-defer the requested capability with
   roadmap language.

Read repository `AGENTS.md` / `CLAUDE.md` / project rules before coding. Honor project bans
(terminology, test naming, temp paths, build wrappers).

## Workflow

### 1. Establish plan authority and scope

1. Open the plan document. Treat it as the source of truth for that phase's checklist, files,
   acceptance criteria, required tests, and exit conditions.
2. Locate the exact heading for the requested phase (and sub-phases if the user named a parent).
3. Extract into a working checklist:
   - target files / modules;
   - behavioral acceptance criteria;
   - required tests and assertion goals;
   - explicit non-goals and dependencies on prior phases;
   - any “phase is complete when …” exit condition.
4. If prior phases are incomplete and the requested phase depends on them, stop and report the
   concrete blocker. If the plan says the prior work is already implemented, verify lightly and
   proceed.
5. Do **not** redesign the phase unless the plan is inconsistent with the codebase. Prefer the
   closest viable implementation that satisfies the written criteria.

### 2. Build to grill-code-review standards (during implementation)

Load and follow `grill-code-review` **as construction standards**, not only as a post-hoc review.
Goal: avoid rework when that skill audits the change later.

While implementing, enforce:

| Area | Build-time requirement |
| --- | --- |
| Correctness evidence | Every acceptance criterion gets a named test with assertions on final state (and critical intermediate ordering when relevant). Compilation or “did not throw” is not enough. |
| Test layers | Unit + boundary/failure + state-machine/async where applicable + integration/reopen when persistence is in scope. Prefer real collaborators over mocks that restate the code. |
| Test naming | Behavior-oriented names only. Never `smoke` / run-only names. |
| Fixtures | Focused builders per module; no god fixture that builds the full app for unit tests. |
| Responsibilities | One coherent reason to change per type/file. Facades route; they do not re-absorb business rules. |
| Decomposition | Real module boundaries with owned state — not method-file splits, friend access, or giant mutable Context bags. |
| Naming | Established domain/SE terms; no vague metaphors or hidden ownership. |
| File size | Track total LOC of changed files. Above ~1000 LOC, plan a responsibility-based split before merging more logic. |
| Call chains | Keep user/entry → service → domain → persistence/async → observable effect explicit and short. |
| Documentation | Concise Doxygen-compatible comments on new/changed public APIs: purpose, preconditions, ownership, side effects, thread/async, failure. |
| Async boundaries | Prefer immutable snapshots / typed messages over shared mutable state across threads. |

Write tests in the same change as production code. Register new test targets in CMake (or the
project's build system) so they actually run.

### 3. Implement only the scoped phase

1. Change production and test code needed for the checklist items.
2. Prefer smallest coherent diffs that fully satisfy the phase exit condition.
3. Match existing architecture layers and project patterns (services, ports, controllers, UI
   boundaries).
4. Use project build wrappers (for example `scripts/msvc_env.cmd` on Windows Alcedo builds). Put
   temporary logs and harness output under `build/tmp/<task>/` when the repo requires it.
5. Run the narrowest relevant tests first, then broader suites proportional to risk. Record exact
   commands and pass/fail counts.

### 4. Optional formal review pass

When the user asks for review, or when the phase is large/high-risk:

1. Invoke `grill-code-review` on the uncommitted (or phase-scoped) changes.
2. Fix **observed failures** and critical **coverage gaps** before claiming the phase complete.
3. Style/maintainability findings: fix what is cheap and in-scope; list residual items honestly in
   the completion record rather than claiming perfection.

Do not treat a clean compile as phase completion.

### 5. Write back to the plan (required when a plan exists)

After implementation and verification, edit the **same plan file**, under the completed phase
heading (or immediately after its checklist / exit condition), append a completion record.

Also update the document-level **Status** line if the plan maintains one, so readers see which
phases are done without scrolling.

#### Completion record template

Use today's date. Keep chains compact. Prefer `text` fenced arrow chains; use Mermaid only if
branching/concurrency is hard to read in prose.

```markdown
##### Phase <ID> completion record (YYYY-MM-DD)

**Status:** complete | partial — <one-line scope summary>

**Primary success call chain:**

```text
<entry / user action>
  -> <controller/service>
  -> <domain / port>
  -> <persistence or async boundary>
  -> <observable effect / completion callback>
```

**Primary failure call chain:**

```text
<failure mode>
  -> <guards / rollback / retained state>
  -> <user-visible or API-visible outcome>
```

**What was proven (executed tests):**

| Required name / criterion | Target / binary | Result |
| --- | --- | --- |
| `TestName` | `BinaryOrSuite` | PASS |

Commands: `<exact build/test commands>`
Suite totals: `<n/n>` …

**Checklist / exit condition:** <all boxes checked | list remaining>

**LOC note (grill-code-review):** <changed-file LOC summary; split notes if any>

**Residual gaps:** <explicitly deferred to later phases, or none>
```

Rules for write-back:

1. Append under the phase that was implemented; do not invent a new top-level doc.
2. Mark checklist items `[x]` only when tests or inspection truly justify it.
3. If partial, set **Status** to partial, leave unfinished checklist boxes unchecked, and list
   what remains.
4. Report **main call chains** (success + important failure paths) — this is mandatory.
5. Do not delete prior completion records; add a new dated record or clearly amend if redoing the
   same phase.
6. Keep residual risks honest: absence of e2e/reopen proof is a residual gap, not silent success.

### 6. Final response to the user

In the chat reply, include:

1. Phase scope completed (and any partial remainder).
2. Main success and failure call chains (same as plan write-back).
3. Tests run and results.
4. Path to the updated plan section.
5. Residual risks / follow-on phases only as concrete facts.

## Hard Constraints

- **Scope discipline:** implement only the user-named phase range. Do not start the next phase
  “while you're here” unless asked.
- **Plan fidelity:** checklists and required test names in the plan are acceptance criteria. Rename
  only when production API naming must differ; note the mapping in the completion record.
- **Evidence over narrative:** do not claim behavior is correct without executed tests (or an
  explicit environmental limitation preventing them).
- **No fake modularity:** splitting methods across files without owned state does not satisfy
  decomposition criteria from `grill-code-review`.
- **No banned project terms** in first-party names/docs when the repo forbids them.
- **No root-level temp dumps;** use the project's designated temp location (often `build/tmp/`).

## Relationship to Other Skills

| Skill | Role |
| --- | --- |
| `execute-phase-plan` (this) | Drive phase-scoped implementation + plan write-back |
| `grill-code-review` | Construction standards during work; formal evidence-led audit after |

Always prefer building correctly under `grill-code-review` rules during implementation over fixing
the same issues after a failed review.
