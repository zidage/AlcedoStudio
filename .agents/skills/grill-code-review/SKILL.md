---
name: grill-code-review
description: Review uncommitted changes, patches, pull requests, or implementation phases with correctness judged from test evidence rather than implementation inspection, while separately auditing naming, responsibilities, performance, file size, fixtures, call chains, and Doxygen maintainability. Use for demanding code reviews, test-coverage audits, phase acceptance reviews, or requests to grill an implementation and its tests.
---

# Grill Code Review

Produce an evidence-led review that distinguishes demonstrated failures, missing test evidence, and
maintainability findings. Be adversarial toward gaps while staying precise about what the evidence
proves.

## Establish Scope and Intent

1. Read repository instructions and the referenced design, plan, issue, or prior task.
2. Enumerate every changed and untracked file. Include build wiring and documentation.
3. Record the acceptance criteria and user-visible behaviors in a test matrix before judging coverage.
4. Check complete file LOC, not only diff size. Flag changed files above 1000 lines and assess a
   responsibility-based split. Also flag test files that concentrate too many unrelated behaviors.

## Preserve the Correctness Evidence Boundary

- Judge behavioral correctness only from tests, test execution, runtime diagnostics, sanitizers,
  benchmarks with assertions, or other observed execution evidence.
- Read production code only for style, structure, performance characteristics, call-chain mapping,
  documentation, and to understand what public surface the tests exercise.
- Do not report an implementation-derived hypothetical as a correctness defect. Convert it into a
  missing-test finding with the exact adversarial scenario required to prove or disprove it.
- Label evidence precisely:
  - `Observed failure`: an executed test or runtime check fails.
  - `Coverage gap`: required behavior has no convincing test evidence.
  - `Style/maintainability`: a source-inspection finding that does not claim behavioral failure.
- Passing tests prove only their asserted behavior. Do not treat compilation, construction, or a
  no-throw assertion as proof of an end-to-end outcome.

## Grill the Tests

Build a traceable matrix from each acceptance criterion and user action to concrete test names and
assertions. Require evidence across these layers when applicable:

1. Basic unit behavior: smallest meaningful success cases and stable invariants.
2. Boundary and safety behavior: empty, null, maximum, malformed, stale, duplicated, reordered, and
   invalid-state inputs. Boundary checks must not be the entire suite.
3. State-machine and failure behavior: every transition, retry, cancellation, partial failure,
   rollback, interruption window, idempotence, and recovery path.
4. Integration behavior: real collaborators, persistence reopen, thread/task boundaries, and build
   registration. Avoid mocks that merely restate the implementation.
5. End-to-end user behavior: exercise the actual entry surface and assert externally visible state,
   ordering, locks, messages, persistence, and subsequent recovery.
6. Non-functional behavior when relevant: concurrency, race resistance, bounded resource use,
   latency/throughput expectations, and large realistic inputs.

For each test, inspect whether it can pass while the promised behavior is broken. Demand assertions
on final state and important intermediate ordering. Prefer deterministic clocks, failure injection,
controllable async executors, temporary storage, and reopen verification over timing sleeps.

Audit fixture quality:

- Extract repeated environment construction into focused fixtures/builders.
- Keep fixtures explicit about defaults and cheap to customize for negative cases.
- Split a test file when it mixes unrelated components, contains too many helper functions, or
  becomes difficult to navigate. Preserve behavior-oriented test names; never use vague run-only
  names.

Run the narrowest relevant tests first, then the integration/e2e set, and finally broader regression
tests in proportion to risk. Record exact commands, pass/fail counts, skipped tests, and environmental
limitations. Verify that newly added test sources are registered and executed, not merely compiled.

## Audit Code Style and Maintainability

Inspect production code independently of correctness conclusions:

- Naming: prefer established software-engineering/domain terms. Reject metaphors, vague containers,
  invented synonyms, misleading boolean polarity, unexplained abbreviations, and names that hide
  units or ownership.
- Responsibilities: require one coherent reason to change per function/class/file. Flag orchestration
  mixed with persistence, UI policy, translation, or low-level mechanics when boundaries are unclear.
- Design: favor small explicit interfaces, immutable inputs at async boundaries, visible side effects,
  and state transitions that an AI agent or human can locate and modify safely.
- Performance: identify avoidable copies, repeated parsing/traversal, blocking I/O on responsive
  threads, unbounded collections/tasks, redundant executor construction, lock scope, and algorithmic
  scaling. State whether the concern is measured, structurally evident, or needs a benchmark.
- Files: for every changed file report total LOC and diff LOC. A changed file over 1000 LOC requires a
  concrete split analysis by responsibility, not an automatic split based on size alone.
- Tests: apply the same naming, responsibility, LOC, and duplication standards to test code.

## Reject Fake Decomposition and God Objects

Do not accept lower per-file LOC as proof of modularity. Moving `GodClass::method` definitions into
another implementation file is only physical file splitting; the god class still owns the state,
dependencies, locking, and reasons to change. Keep the decomposition finding open until real module
boundaries exist.

A decomposition counts only when each resulting unit has:

- a responsibility named with an established domain or software-engineering term;
- its own type or explicit module API;
- sole ownership of the mutable state needed for that responsibility;
- a clear dependency direction and typed requests, results, or callbacks;
- focused unit tests that can construct the unit without constructing the former god object.

Require a responsibility-and-state inventory for every in-scope oversized or multi-purpose class.
For each responsibility, name the current methods and fields, the target type, its public API, its
dependencies, and its test target. Every mutable field must have exactly one owning module. Move the
field with the behavior that maintains its invariants; do not leave component-owned state in a thinly
renamed parent.

Reject these shortcuts as incomplete decomposition:

- spreading one class's method definitions across several `.cpp` files;
- giving an extracted component `friend` access or a parent/god-object pointer so it can keep
  mutating the parent's internals;
- passing every component one giant mutable `Context`, `State`, `Manager`, or service locator;
- coordinating nominally independent components through the former parent's shared mutex;
- retaining business rules or component-owned state in an orchestrator/facade;
- creating a new module that still mixes lifecycle, persistence, rendering, editing, UI policy, or
  transport concerns;
- creating QML/UI wrappers that merely mirror a monolithic controller without owning a distinct UI
  behavior;
- replacing repeated setup with one god fixture that constructs unrelated subsystems for every unit
  test.

A facade is acceptable when it owns or receives focused collaborators and only routes typed
commands, results, immutable snapshots, and completion/failure notifications. It must not become a
back door to shared mutable state. At asynchronous boundaries, prefer immutable snapshots or explicit
messages over references into another component's state.

Organize permanent production and test modules by domain responsibility or user behavior, not by a
roadmap phase, fix number, or temporary migration label. Keep unit fixtures focused on one module;
reserve a full application graph for integration and end-to-end fixtures.

When decomposition needs stages, require each stage to name exact source and test files, target
types, methods and fields moved, dependency direction, compatibility work, deletion of temporary
files, and executable acceptance tests. Treat roughly 500 changed LOC as a reviewability target, not
a hard cap: split near that size only at a coherent boundary. If a stage merely relocates methods,
label it as a temporary relocation prerequisite and do not claim the god-class finding is fixed.

## Document the Call Chain

Map each user action or external entrypoint through controllers/services, domain logic, persistence or
async boundaries, and the final observable effect. Include failure and completion callbacks. Prefer a
compact arrow chain or Mermaid diagram only when branching or concurrency makes prose unclear.

For every changed or introduced function, check for a concise Doxygen-compatible comment. Require it
to describe purpose, important preconditions, parameters, return value, ownership/lifetime, side
effects, async/thread affinity, and failure behavior where applicable. Do not accept comments that
repeat the function name or narrate implementation line by line. For local callbacks or QML/JavaScript
functions, use the nearest documentation convention that preserves the same information.

## Report Findings

Lead with actionable findings ordered by severity and include file and line references. Each finding
must state:

- evidence category;
- affected acceptance criterion or user behavior;
- what is demonstrated or unproven;
- the exact test or refactor needed;
- why existing coverage or structure is insufficient.

After findings, provide:

1. test-evidence verdict and executed commands;
2. acceptance-criterion coverage matrix;
3. changed-file LOC table and split recommendations;
4. call-chain map;
5. Doxygen/documentation gaps;
6. residual risks and environmental limitations.

If no observed failure exists, say so explicitly. Never convert absence of evidence into evidence of
correctness.
