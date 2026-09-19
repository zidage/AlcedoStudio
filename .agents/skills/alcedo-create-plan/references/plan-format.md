# Alcedo Plan Format

Use this reference for a new detailed phase plan. Adapt it to the local roadmap.
Do not add an empty section only to match this list.

## 1. Header

Include:

- the phase or plan title;
- the creation or revision date;
- the status;
- the parent plan;
- direct prerequisite plans;
- the source revision used for the audit, when it matters.

Use `planned` for an approved design that has no implementation evidence. Use
`in progress` only after implementation starts. Use `complete` only when the
required evidence exists.

## 2. Decision record

Write the approved product decisions first. This section must let a new
implementer understand the user request without the original conversation.

Include:

- the user goal in polished language;
- later user corrections;
- selected design options;
- rejected or superseded options that must not return;
- the evidence status of prerequisite work.

Do not copy informal prompt text. Preserve its meaning.

## 3. Product design specification

Describe the complete user-visible result.

For a UI feature, include:

- an ASCII layout when spatial relationships matter;
- visible controls and labels;
- focus and selection rules;
- enabled and disabled states;
- keyboard and accessibility behavior;
- empty, loading, and error states;
- theme, spacing, typography, and button rules;
- narrow-window behavior;
- copy, paste, save, cancel, and retry behavior when applicable.

For a data feature, include:

- identities and stable keys;
- field ownership;
- ordering rules;
- default values;
- serialization rules;
- validation rules;
- failure behavior;
- compatibility behavior.

For an asynchronous feature, include:

- the owner thread or executor;
- queue boundaries;
- publication order;
- cancellation or the reason that cancellation is not required;
- the exact executable interleaving for each new consistency mechanism.

## 4. Scope

List included work by module. Give each module one responsibility.

List explicit exclusions. Explain which existing owner keeps excluded work.
Do not use exclusions to delay a capability that the user requested.

List related plans. State whether this plan extends, replaces, or only depends
on each related plan.

## 5. Current source audit

Use a table with these columns when it improves clarity:

| Area | Current owner and path | Current behavior | Required change |
| --- | --- | --- | --- |

Include current tests and build registration. Record the actual source behavior.
Do not describe a planned file as if it already exists.

## 6. Target architecture

Define owners before you define data structures.

For each owner, specify:

- its input;
- its output;
- the data that it can change;
- the data that it can only read;
- its lifetime;
- its error surface;
- the caller that publishes the result.

Show important data flow with a short call chain. Add a failure chain for any
operation that changes persistent or user-visible state.

For a new value type, explain why an existing type or owner operation cannot
meet the requirement. Keep the new type minimal.

## 7. File and API map

List current files that will change. List proposed files separately.

For each proposed API, give:

- a descriptive name;
- input types;
- output types;
- ownership and lifetime;
- validation responsibility;
- error result;
- whether the operation changes state.

Do not prescribe a forward declaration to reduce includes. The implementation
must include the defining header unless `AGENTS.md` permits an exception.

## 8. Phase summary

Use a table:

| Phase | Result | Main modules | Dependency | Expected diff | Status |
| --- | --- | --- | --- | ---: | --- |

Keep every expected diff at or below 2000 lines. Split a phase when the estimate
can pass that limit.

## 9. Detailed phases

Use this structure for each phase.

### Phase identifier and name

**Objective and deliverables**

State the result that the phase adds. State what a reviewer can observe.

**Inputs and prerequisites**

List the source state and prior phase outputs that this phase requires.

**Modules, files, and APIs**

Name the current files that change. Mark each new file as proposed. State the
owner responsibility for each file.

**Data rules and invariants**

List all identity, order, lifetime, validation, persistence, and publication
rules that apply.

**Implementation steps**

Use an ordered list. Each step must name an action and its result. State how the
code reports failure. State what it must not change.

**Primary success call chain**

Use a compact text diagram. Show the owner transitions and publication order.

**Primary failure and restore call chain**

Show the failure source, the state that remains valid, and the error that the
caller receives. Do not add an unauthorized substitute path.

**Tests and evidence**

Name tests by behavior. For each test, state the observable assertion. Include
failure injection when the operation changes persistent state.

**Build and run commands**

Use repository wrappers and presets. Confirm test discovery before you claim a
test ran. Put temporary logs under the required build directory.

**Exit criteria**

Use checkboxes. Each item must map to an observable result or evidence record.

**Expected diff**

Give a range. Include production code, tests, build files, resources, and docs.
Split the phase if the upper bound is more than 2000 lines.

**Completion record**

Add an empty record template. Do not fill it during plan creation.

## 10. Cross-phase acceptance matrix

Use named behaviors. Cover:

- normal operation;
- boundary values;
- invalid input;
- owner failure;
- persistence and reopen;
- Undo and Redo when applicable;
- multiple versions or targets when applicable;
- QML behavior and accessibility when applicable;
- real product integration.

State the expected result. Do not use a test name that only says that code ran.

## 11. Build and evidence rules

State:

- supported presets and wrappers;
- target groups;
- configuration flags;
- required platforms and backends;
- temporary evidence location;
- how to record pass, fail, skip, and unavailable results;
- which manual checks supplement automated tests.

Do not report an unavailable result as zero. Do not report a skipped test as a
pass.

## 12. Risks and stop conditions

Name each real risk. Give its detection signal and required response.

Stop implementation when:

- a required owner API does not exist;
- the source audit no longer matches the branch;
- a phase can pass the 2000-line limit;
- a new persistence format lacks a rejection or migration decision;
- an operation needs a fallback that the user did not authorize;
- a proposed consistency mechanism lacks a real production interleaving.

Update the plan before implementation continues.

## 13. Completion record template

Use a compact record that includes:

```text
Phase / date / status:
Source revision and branch:
Actual changed modules:
Implemented behavior:
Explicitly unimplemented items:
Primary success call chain:
Primary failure and restore call chain:
Build and test commands with exit codes:
Discovered / passed / failed / skipped counts:
Manual verification:
Performance or resource evidence, if required:
Evidence path:
Remaining defects or unavailable platforms:
```

Keep historical failures and gaps. Do not remove them to make completion look
cleaner.
