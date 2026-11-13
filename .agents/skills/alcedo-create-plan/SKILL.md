---
name: alcedo-create-plan
description: >
  Create or revise detailed Alcedo Studio roadmap and phase plans. Use when the
  user asks for an implementation plan, design specification, roadmap phase,
  phase split, or a plan file under docs/roadmap. Do not use to execute an
  approved phase.
---

# Alcedo Create Plan

Create an implementation-ready plan for Alcedo Studio. Write the plan in
English. Do not implement the planned product changes unless the user also asks
for implementation.

## Required language

Load `Simplified Technical English (ASD-STE100)` before you write the plan.
Apply its rules to all new plan text.

- Use active voice.
- Use short sentences.
- Give one instruction per sentence.
- Use one term for one concept.
- Define required project terms once.
- Keep technical names when they add precision.
- Do not add an STE before-and-after table.
- Do not keep a record of wording changes.

## Entry checks

Complete these checks before you edit a plan:

1. Read the repository `AGENTS.md` file.
2. Read each nested `AGENTS.md` file that applies to the target path.
3. Read the user request and all corrections in the current conversation.
4. Read the target master plan and the target phase plan, if they exist.
5. Read two to four relevant plans from the same roadmap area.
6. Read the current source, tests, build files, and design rules for the scope.
7. Load each project skill that applies to the planned implementation.
8. Check the worktree before you edit files.

Do not use an old plan as proof of current source behavior. Confirm important
facts in the current source tree.

## Preserve the user decision

Rewrite the user request as a polished product specification. Preserve every
approved requirement and correction. Resolve informal wording, but do not add a
different product direction.

Record these items:

- the product goal;
- the approved interaction and visual design;
- required success behavior;
- required failure behavior;
- explicit exclusions;
- unresolved decisions, if any.

Do not retain superseded ideas as active work. Keep a short decision note only
when it prevents an implementer from reopening rejected work.

## Source audit

Describe the current implementation before you describe the target design.
Name the current owners, modules, APIs, files, and tests. State the observed
defect or limitation. Separate a verified source fact from an inference.

Do not add a generation, epoch, token, cancel protocol, or similar mechanism
without the evidence that `AGENTS.md` requires.

Do not add a duplicate data model for convenience. Identify the current data
owner. Use an owner operation, a scoped read, or a minimal change description.
If the feature needs an independent point-in-time value, explain its purpose,
fields, owner, lifetime, and consistency rule.

## Plan structure

Read [references/plan-format.md](references/plan-format.md) before you write or
revise a plan. Use its sections when they apply. You can change the heading
order when the existing roadmap uses a better local structure.

The plan must include:

- a complete design specification;
- an exact scope by module;
- the related plans and source dependencies;
- target ownership and data flow;
- primary success and failure call chains;
- ordered implementation phases;
- detailed work for each phase;
- test and evidence requirements;
- build and verification commands;
- completion record requirements.

## Phase size limit

Estimate the changed lines for each implementation phase. Count production
code, tests, build files, resources, and documentation that the phase changes.
Do not count generated files or temporary evidence.

Split a phase before implementation when its expected diff is more than 2000
lines. Split it when the estimate has a material risk of passing 2000 lines.
Keep each phase independently reviewable and testable. Do not split one
indivisible state change across phases.

Show the estimate and the split reason in the phase summary table. An estimate
is a planning value. It is not completion evidence.

## Phase detail standard

Write each phase like a computer science course project assignment. An
implementer must know what to build, where to build it, and how to prove it.

For each phase, include:

1. Objective and deliverables.
2. Inputs and prerequisites.
3. Modules, files, types, and owner APIs.
4. Data rules and invariants.
5. Ordered implementation steps.
6. Primary success call chain.
7. Primary failure and restore call chain.
8. Named tests and observable assertions.
9. Build and run commands.
10. Exit criteria.
11. Expected diff size.
12. A completion record placeholder.

Do not use vague instructions such as "wire it up," "handle errors," or "add
tests." Name the operation, the owner, the failure, and the required result.

## Repository rules

Apply all current rules from `AGENTS.md`. Do not copy the full file into the
plan. List the rules that directly affect the planned work.

Before implementation, the plan must tell the executor to read `AGENTS.md` and
all applicable skills again. The repository rules can change after plan
creation.

Check new roadmap text against the prohibited terminology in `AGENTS.md`.
Check the complete `docs/roadmap/` tree for prohibited filenames before you
finish. Update links when you rename a file.

## Plan updates

When the phase belongs to a master plan:

- replace an empty placeholder with the detailed phase plan;
- update the master phase status;
- update the master phase result and summary;
- preserve truthful evidence for earlier phases;
- distinguish user-confirmed manual verification from automated evidence;
- do not mark a phase complete during plan creation.

## Final checks

Before you report completion:

1. Confirm that all new plan text is English.
2. Confirm that STE wording is clear and direct.
3. Confirm that no phase estimate is more than 2000 lines.
4. Confirm that every phase has detailed implementation and test steps.
5. Confirm that source links and paths exist or are clearly marked as proposed.
6. Confirm that the master plan and phase plan agree.
7. Run the repository terminology checks for the touched roadmap files.
8. Review the final diff for unrelated changes.

Report the plan files that changed. Report that the product code did not change
when the request only asked for planning.
