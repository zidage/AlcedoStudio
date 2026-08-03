# UI Fuzz Automation Platform Plan

Date: 2026-08-03

Primary roadmap owner: `alcedo_studio/src/ui` (test host executable) and `tools/ui_fuzz_platform` (web platform)

Affected areas: `alcedo_studio/src/ui/alcedo_main` (objectName coverage, automation-mode flag),
CMake target graph (new executable), `tools/` (new top-level directory), `alcedo_studio/tests`
(named GoogleTest coverage for the test host).

Status: Phase 0 complete on 2026-08-03; revised the same day to replace the embedded WebSocket
automation server with a thin in-process TestProbe behind QLocalSocket. Phase 1 is pending.

## Problem

Alcedo Studio has no automated end-to-end UI exercising. Deadlocks, crashes, and correctness
regressions in the QML shell are found manually. Existing tests are GoogleTest unit/integration
suites plus targeted Qt Quick Tests; nothing drives the real QML shell through realistic input
sequences. Two OS-modal flows block unattended runs: `WelcomeDialog.qml` (project open/create)
and the import `FileDialog` in `AppDialogs.qml`.

The goal is a "human simulator": a scripted, fuzzing-capable runner that injects real input into
the shipping QML UI, verifies an expected result after every operation, and captures a complete
reproduction bundle (seed, operation history, log tail, window grab) on failure. Primary targets:
deadlock detection, crash detection, and UI correctness.

## Confirmed Design Decisions

Decisions locked with the owner on 2026-08-03:

| Decision | Choice |
|---|---|
| QML observation channel | Thin `TestProbe` living inside the test host process. It receives the `QQmlApplicationEngine` and the root `QQuickWindow`, traverses the live `QQuickItem` tree (`rootObjects()` -> `contentItem()` -> `childItems()`), and reads properties through the Qt meta-object system. No hand-written serialization layer |
| Input path (hybrid) | Actions take the real user path: the probe resolves an element's scene position and injects synthesized mouse/key events through the normal `QQuickWindow` event dispatch (hover, focus, and pointer-handler semantics preserved). Observation is probe-side property reads and server-side waits. Windows UI Automation is a v2 option for OS-level input fidelity. Never call QML signals such as `clicked()` directly |
| Probe IPC | `QLocalServer` / `QLocalSocket` with JSON Lines. The Qt side never touches WebSocket; WebSocket/SSE exists only between the browser dashboard and the Next.js backend |
| Element identification | `objectName` as the stable test id (the `data-testid` analogy); optional `property string testId` convention for namespaced ids such as `project.import`. QML-local `id`s are never used as locators — they are invisible from C++ by design |
| Static QML scan role | Candidate action catalog only. Runtime truth is always the live `QQuickItem` tree: Loaders, delegates, conditional instantiation, popups, and state-dependent visibility make static structure unreliable as an execution source |
| Log capture | The runner spawns the test host and reads its stdout/stderr pipes directly; Qt logs never pass through the probe |
| Web stack | Next.js + React, single app: API routes manage the child process, browser-facing WebSocket streams live logs, dashboard, flow editor, DB access |
| Scenario script format | YAML DSL; human-writable, flow-editor round-trip, JSON Schema validation |
| v1 assertion surface | (a) QML element property assertions via the probe; (b) process liveness: heartbeat gap = deadlock, process exit = crash, per-expect timeout = failure. Log-pattern matching and DuckDB project-state assertions are deferred to v2 |
| Result store | SQLite, single file under the platform data directory, archived per run |
| DAG editor | React Flow with custom two-outlet operation nodes (next edges + expect edges), lossless YAML round-trip |
| Fuzz strategy | Seeded weighted random walk; every run records its seed; a failure seed replays the exact operation sequence deterministically |
| Test host shape | New plain executable target `alcedo_studio_test_host` (not a test target), reusing `AlbumBackendLib` and the `Alcedo.Main` QML module; WelcomeDialog never presented; CLI args feed project path, import dir, probe socket name |

## Architecture

Three cooperating pieces:

1. **`alcedo_studio_test_host`** (C++/Qt, this repo): boots the real application shell
   (`ApplicationModuleHost`, `AlbumBackendLib`, `Alcedo.Main` QML module) without any OS dialog,
   auto-opens/creates a project at a given path, recursively imports a given directory, and
   hosts the `TestProbe` behind a `QLocalServer`.
2. **Scenario DSL** (YAML files, platform-side): describes the operation DAG. Each operation node
   has two outlet kinds: weighted `next` edges to successor operations, and `expect` assertions
   describing the side effect that must hold after the operation.
3. **Web platform** (`tools/ui_fuzz_platform`, Next.js): spawns and supervises test-host
   processes, walks the DAG with a seeded RNG, evaluates expects through the probe, streams Qt
   logs live from the child pipes, persists runs to SQLite, renders the dashboard and the React
   Flow editor, and scans the QML sources to generate the candidate action catalog.

```
browser dashboard
      | HTTP / WebSocket (browser <-> backend only)
      v
tools/ui_fuzz_platform (Next.js)
      | child_process spawn; stdout/stderr pipes carry Qt logs
      | QLocalSocket (JSON Lines, one client per host)
      v
alcedo_studio_test_host (Qt)
      TestProbe -> QQmlApplicationEngine::rootObjects()
                -> QQuickWindow::contentItem() -> live QQuickItem tree
                -> direct in-process access, no serialization layer
                -> synthesized QMouseEvent / QKeyEvent into QQuickWindow
```

Inside the test host the wiring is deliberately thin:

```cpp
QQmlApplicationEngine engine;
engine.loadFromModule("Alcedo.Main", "Main");
auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().value(0));
TestProbe probe(&engine, window);   // serves QLocalServer on the requested socket name
```

## TestProbe Surface

`TestProbe` (in `alcedo_studio/src/ui/alcedo_studio_test_host/test_probe.{hpp,cpp}`) owns exactly
six operations:

- `snapshotWindow()` — recursive walk of `QQuickItem::childItems()` from the window's
  `contentItem()`. Per element: objectName (or `testId` property), meta-object type name,
  visible, enabled, scene rect, plus the common inspectable properties when present
  (`text`, `checked`, `currentIndex`, `value`, `activeFocus`) read generically through
  `QQmlProperty` / `QMetaObject::indexOfProperty` — no per-page serialization code.
- `findElement(testId)` — existence + resolved path. Resolution happens per call, so targets
  pointing at destroyed items (Loader-hosted pages, popups) fail loudly instead of dereferencing
  stale pointers.
- `readProperty(testId, property)` — current value as JSON.
- `clickElement(testId)` — resolve scene center, deliver press/release as real
  `QMouseEvent`s through the window's event dispatch; fails when the element is invisible,
  disabled, or covered by a modal overlay. Double-click, right-click, key input, text input, and
  position-based drag (sliders, filmstrip) follow the same path.
- `waitForProperty(testId, property, matcher, timeoutMs)` — server-side wait: subscribes to
  the property's notify signal when one exists, otherwise polls on a GUI-thread timer. Replies
  with the actual value on success, or a timeout error carrying the last observed value. The GUI
  thread is never blocked.
- `captureScreenshot()` — `QQuickWindow::grabWindow()` to PNG. Taken at scenario-marked key
  nodes and on failure only; not part of routine stepping.

## Probe IPC Protocol (JSON Lines over QLocalSocket)

One line per message. Requests carry `id`; replies echo it. The runner is the only client.

```
->  {"id":17,"method":"snapshot"}
<-  {"id":17,"result":{"window":"Alcedo Studio","elements":[...]}}
->  {"id":18,"method":"click","target":"project.import"}
<-  {"id":18,"result":"ok"}
->  {"id":19,"method":"wait","target":"photoGrid","property":"count","gte":1}
<-  {"id":19,"result":"ok","actual":127}
->  {"id":20,"method":"read","target":"importStatus","property":"text"}
<-  {"id":20,"result":{"value":"Finished"}}
```

Method set (v1): `snapshot`, `find`, `read`, `click`, `doubleClick`, `rightClick`, `key`,
`typeText`, `drag`, `wait`, `screenshot`, `ping`. Wait matchers: `eq`, `ne`, `contains`, `gt`,
`gte`, `lt`, `lte`, `truthy`.

Unsolicited events from the probe:

- `{"event":"ready"}` — shell loaded, project open, import settled.
- `{"event":"heartbeat","counter":n,"guiTimeMs":t}` every 250 ms, emitted by a `QTimer` on the
  GUI thread. A GUI-thread deadlock stops heartbeats; that is the deadlock signal. A heartbeat
  from any other thread would be meaningless here.
- `{"event":"fatal","reason":...}` — best-effort notice before abnormal shutdown.

Liveness verdicts (runner-side): heartbeat gap (or unanswered `ping`) >
`livenessThresholdMs` (default 5000) -> deadlock; child process exit before `run.stop` ->
crash; `wait` timeout -> correctness failure.

## Test Host Executable

Location: `alcedo_studio/src/ui/alcedo_studio_test_host/`, wired as a sibling of `alcedo_main`
(precedent: `EditorRhiHarness` is a standalone `add_executable`). Registered as a normal build
target in the `win_debug` / `win_release` presets, not under `tests/`.

- Own `main.cpp` using `QCommandLineParser` (the production `main.cpp` scans argv ad hoc; new
  code should not copy that). It replicates the existing bootstrap: `ApplicationModuleHost`,
  `RegisterApplicationModuleTypes()`, `appTheme`, `languageManager` context properties,
  `engine.loadFromModule("Alcedo.Main", "Main")`, then hands engine + root window to
  `TestProbe`.
- Welcome suppression: the host sets an `automationMode` context property before loading the
  root QML. `Main.qml` / `AppDialogs.qml` skip presenting `WelcomeDialog` when the flag is set.
  The QML module source list stays shared; the dialog simply never opens. This keeps one QML
  source of truth instead of forking `AppDialogs.qml`.
- Arguments:
  - `--project-path <dir>`: project storage directory; created via
    `ProjectModule::CreateProjectInFolderNamed` semantics when missing, otherwise
    `ProjectModule::LoadProject`. No `QFileDialog` is ever shown.
  - `--import-dir <dir>`: recursively expanded with `std::filesystem::recursive_directory_iterator`
    filtered to supported image extensions (same approach as `CollectRawTestImages` in
    `album_backend_test_fixture.hpp`), then fed to
    `ImportExportHandler::StartImportPaths(...)`. Default for manual runs:
    `alcedo_studio/tests/resources/sample_images/raw/camera`.
  - `--probe-socket <name>`: local socket name; defaults to `alcedo_test_host_<pid>` and is
    printed to stdout as `PROBE_SOCKET=<name>` for the parent process to parse. Per-pid default
    avoids stale-name collisions between runs.
  - `--reuse-project`: skip re-import when the project already contains the import set
    (fast iteration for long fuzz sessions).
- Startup ordering: parse args -> open project (or schedule open) -> start import -> load root
  QML -> start `TestProbe` server -> emit `ready` only when the project is open and the import
  job has settled. The platform must not send operations before `ready`.

## Scenario YAML Schema

```yaml
name: library_to_editor_exposure
start: workspace_ready
defaults:
  expectTimeoutMs: 8000
nodes:
  workspace_ready:
    op: { action: wait, target: workspaceHost, property: visible, eq: true }
    next:
      - { to: open_first_image, weight: 1 }
  open_first_image:
    op: { action: click, target: thumbnailGridView_firstCard }
    expect:
      - { target: editorWorkspace, property: visible, eq: true }
      - { target: editorSessionStatus, property: text, contains: "Ready", timeoutMs: 15000 }
    next:
      - { to: drag_exposure_slider, weight: 2 }
      - { to: back_to_library, weight: 1 }
```

- `op.action` catalog (v1): `click`, `rightClick`, `doubleClick`, `key`, `typeText`, `drag`,
  `wait`, `waitMs`, `screenshot` (marks a key node for the artifact record).
- `expect` entries compile to probe `wait` calls and accept the same matchers plus an optional
  `timeoutMs` override.
- `next` edges carry `weight`; the runner picks proportionally with the run seed. A node with
  no `next` ends the walk (run success when `maxSteps` or `maxDurationMs` is also reached).
- The schema is validated against a JSON Schema document shipped in the platform repo; the flow
  editor and the runner share the same validator.
- Scenario targets reference runtime elements discovered through `snapshot`. The static QML
  scan only seeds the editor palette; it is never consulted at execution time.

## Runner Semantics

1. Spawn the test host with the run's `--project-path`/`--import-dir`; parse `PROBE_SOCKET`
   from stdout; connect the JSON Lines channel; await `ready` (bounded by `startupTimeoutMs`).
   stdout/stderr keep streaming to the log view in parallel — they never transit the probe.
2. Walk: resolve current node -> dispatch `op` -> evaluate each `expect` via probe `wait`
   -> pick weighted `next` edge with the seeded RNG -> record the step (node id, op, resolved
   target, expect results with actual values, timestamps) to SQLite.
3. Failure capture: on any verdict (deadlock, crash, correctness), persist the seed, the full
   ordered operation history, the last N KiB of the Qt log from the child pipes, the last
   `snapshot` result, and a `screenshot` when the window is still responsive.
4. Replay: `runs.replay(seed)` re-executes the identical edge choices with weights bypassed;
   the run header links the replay to the original failure.

## Web Platform

Location: `tools/ui_fuzz_platform/` (new top-level directory; the repo root currently has no
`tools/`). Next.js + React + TypeScript, `better-sqlite3` for the DB, `ws` on a small custom
server for the browser-facing log stream, React Flow for the editor, `js-yaml` + `ajv` for the
DSL, Node `net` client for the QLocalSocket channel.

Pages:

- **Dashboard** (`/runs/active`): start/stop controls, run config (seed, max steps, max
  duration, per-node weight overrides, liveness threshold), live Qt log view (follows child
  stdout, level-colored), current operation + step counter + elapsed time, heartbeat indicator.
- **Results** (`/runs`, `/runs/[id]`): run table with verdicts; detail page with ordered step
  list, failing expect with last observed value, captured artifacts, and a "Replay seed" button.
- **Editor** (`/workflows/[name]`): React Flow canvas. Operation nodes have two outlet kinds:
  `next` (weighted, to another operation) and `expect` (to assertion nodes). Saving writes the
  YAML file losslessly; loading renders any valid YAML file, including hand-written ones.
- **Element catalog** (`/catalog`): generated by the QML scanner — every interactive element
  found in `alcedo_main/qml` (objectName, source file, inferred op kinds). The catalog is a
  candidate list for authoring; the runner always resolves targets against the live tree via
  `snapshot`, and the catalog page marks entries missing at runtime as stale.

SQLite schema (v1): `runs(id, seed, scenario, started_at, ended_at, verdict, config_json)`,
`steps(id, run_id, seq, node_id, op_json, expect_results_json, started_at, ended_at)`,
`failures(run_id, kind, detail_json, op_history_json, log_tail, tree_snapshot, screenshot_path)`.

## Phases and Acceptance Criteria

### Phase 0 — Test host skeleton and TestProbe link (C++)

- New `alcedo_studio_test_host` target building under `win_debug`/`win_release`;
  `automationMode` flag suppresses WelcomeDialog; `--project-path`/`--import-dir`/
  `--probe-socket` work; recursive camera-tree import runs through `StartImportPaths`.
- `TestProbe` serves `snapshot`, `find`, `read`, `ping` over JSON Lines on QLocalSocket; emits
  `ready` and GUI-thread heartbeats.
- Acceptance: launching the host against a scratch project imports every RAW under
  `tests/resources/sample_images/raw/camera` with no dialog shown; a manual socket client
  receives `ready`, steady heartbeats, and `read workspaceHost.visible` returns true.
- GoogleTest additions (purpose-named): `TestHostSkipsWelcomeDialogAndImportsCameraTreeRecursively`,
  `TestProbeAnswersSnapshotAfterProjectReady`,
  `TestProbeReportsStaleTargetAfterDialogDestroyed`.

##### Phase 0 completion record (2026-08-03)

**Status:** complete — the debug and release host targets build, the production QML module remains
shared, and the required probe/import behavior is covered by executed tests.

**Primary success call chain:**

```text
alcedo_studio_test_host main
  -> QCommandLineParser (--project-path / --import-dir / --probe-socket)
  -> RegisterApplicationModuleTypes()
  -> QQmlApplicationEngine::loadFromModule("Alcedo.Main", "Main")
  -> automationMode=true
  -> ProjectLaunchController/AppDialogs keep WelcomeDialog closed
  -> ProjectModule start/create
  -> ImportExportHandler::StartImportPaths(recursive camera paths)
  -> ImportStateChanged (ImportFailed == 0 && ImportCompleted == ImportTotal)
  -> TestProbe::MarkReady()
  -> QLocalSocket JSON Lines ready/heartbeat
  -> snapshot/find/read/ping
```

**Primary failure call chain:**

```text
invalid command line or project/import path -> qCritical + exit(1), no ready event
project initialization/import failure -> qCritical + exit(1), no ready event
invalid JSON or unsupported probe method -> JSON error response
unknown or destroyed QML target -> target_not_found JSON error response
```

**What was proven (executed tests):**

| Required name / criterion | Target / binary | Result |
|---|---|---|
| `TestHostSkipsWelcomeDialogAndImportsCameraTreeRecursively` | `UiFuzzAutomationTest.exe` | PASS — 107/107 RAW files imported; WelcomeDialog remained invisible and unopened; ready, heartbeat, and `read workspaceHost.visible` succeeded |
| `TestProbeAnswersSnapshotAfterProjectReady` | `UiFuzzAutomationTest.exe` | PASS — snapshot found `workspaceHost`; ping reported the GUI thread and heartbeat; property read returned true |
| `TestProbeReportsStaleTargetAfterDialogDestroyed` | `UiFuzzAutomationTest.exe` | PASS — a destroyed target returned `target_not_found` without retaining a stale item pointer |
| `alcedo_studio_test_host` build coverage | `win_debug` and `win_release` | PASS — both preset builds linked the executable and the QML module plugin |
| Production shell compatibility | `alcedo_main` under `win_debug` | PASS — production main and the shared QML registration source compiled and linked |

Commands executed:

```text
cmd /c scripts\msvc_env.cmd --preset win_debug
cmd /c scripts\msvc_env.cmd --build --preset win_debug --target alcedo_studio_test_host UiFuzzAutomationTest --parallel 4
$env:QT_QPA_PLATFORM='offscreen'; .\UiFuzzAutomationTest.exe
ctest --test-dir build/debug/alcedo_studio/tests/ui --output-on-failure -R UiFuzzAutomationTest
cmd /c scripts\msvc_env.cmd --build --preset win_debug --target alcedo_main --parallel 4
cmd /c scripts\msvc_env.cmd --preset win_release
cmd /c scripts\msvc_env.cmd --build --preset win_release --target alcedo_studio_test_host --parallel 4
```

Suite totals: 3 tests from 2 suites ran; 3 passed.

**Checklist / exit condition:** complete — the host target, automation-mode startup, recursive
camera import, probe methods/events, stale-target behavior, and debug/release build requirements
are all evidenced above.

**LOC note:** the change is intentionally limited to the host executable, shared QML type
registration, automation-mode wiring, probe/test target wiring, and the three Phase 0 tests.

**Residual gaps:** input injection, wait matchers, screenshot capture, and the web runner remain
Phase 1+ work. The current probe deliberately exposes only `snapshot`, `find`, `read`, and `ping`.
The repository-level CTest entry point is currently blocked during unrelated
`SharedToneCurveTest` discovery by a missing runtime DLL; the scoped UI CTest directory passes
all three Phase 0 tests.

### Phase 1 — Real-path input injection and objectName coverage

- Position-based mouse/key injection through `QQuickWindow` dispatch; covered/invisible/
  disabled targets rejected with an explicit error; `wait` with notify-signal subscription.
- Audit `alcedo_main/qml` for interactive elements missing `objectName` (navigation, toolbar,
  thumbnail delegates, editor panels, menus); add them. `SlidingIconNav.qml` already anticipates
  objectName-based discovery.
- Acceptance: a scripted sequence clicks Library -> first thumbnail -> editor workspace, and
  probe reads confirm each transition; an intentionally wrong `wait` times out and reports the
  last observed value.
- Tests: `InputClickActivatesNavButtonAndSwitchesWorkspace`,
  `InputClickRejectsElementCoveredByModalOverlay`,
  `TestProbeWaitForPropertyTimeoutReportsLastObservedValue`.

### Phase 2 — YAML DSL and runner core (platform, headless)

- YAML schema + JSON Schema validation, loader, QLocalSocket JSON Lines client, sequential walk
  (weights parsed but unused), expect engine compiling to probe `wait`, failure bundle capture,
  CLI entry `pnpm fuzz run <file>`.
- Acceptance: a hand-written scenario "open project -> import -> open first image -> drag
  exposure slider" passes end-to-end; an intentionally wrong expect fails and the failure bundle
  contains the operation history, log tail, and screenshot.

### Phase 3 — Dashboard MVP

- Next.js scaffold, process manager (spawn/kill child tree, parse `PROBE_SOCKET`), live log
  streaming from child pipes, run controls (seed, max steps, max duration), current-op/step/
  elapsed display, heartbeat indicator.
- Acceptance: dashboard starts and stops the host; logs stream live; killing the run tears down
  the child process tree with no orphan.

### Phase 4 — Persistence, results browser, replay

- SQLite schema, results pages, replay-by-seed.
- Acceptance: a failed run row holds seed + full operation list + log tail; replay reproduces
  the identical step sequence (verified by comparing `steps` rows).

### Phase 5 — Flow editor and QML scanner

- Scanner generates the candidate element catalog from `alcedo_main/qml`; React Flow editor
  with two-outlet operation nodes; lossless YAML round-trip; runtime staleness markers on the
  catalog page via `snapshot` diffing.
- Acceptance: an editor-assembled workflow saves to schema-valid YAML identical in semantics to
  a hand-authored file; the runner executes it without edits.

### Phase 6 — Seeded fuzzing and hardening

- Weighted random walk with recorded seed; deadlock verdict via heartbeat gap / unanswered
  `ping`; crash verdict via exit code; failure artifacts (log tail, tree snapshot, screenshot);
  per-run node/edge coverage counters (input for a future coverage-guided mode).
- Acceptance: a 10k-step fuzz session either completes or fails with a complete repro bundle;
  an artificially injected event-loop stall triggers the deadlock verdict within
  `livenessThresholdMs`.

## Risks and Deferred Scope

- `QLocalServer`/`QLocalSocket` live in Qt Network, which the application already links; no new
  Qt module dependency. On Windows the socket is a named pipe; the runner must tolerate a short
  connect-retry window while the host initializes.
- One runner per host is assumed. Multi-client probe access is explicitly out of scope.
- `waitForProperty` must never block the GUI thread; notify-signal subscription with a timer
  fallback keeps the event loop responsive, which is also what keeps heartbeats honest.
- Static catalog drift: scanner output is advisory and can go stale as QML evolves; the runner
  resolves targets against live `snapshot` output, and the catalog page surfaces staleness
  instead of failing silently.
- Recursive import of the full camera tree is slow on a cold project; `--reuse-project` plus
  gating `ready` on import-settled keeps iteration acceptable.
- Heartbeat proves GUI-thread liveness only. A deadlock on a worker thread that does not block
  the GUI thread will surface as a hung expect (correctness failure), which is acceptable for
  v1; thread-scoped watchdogs are a v2 candidate.
- Deferred to v2: Windows UI Automation input path, Qt log-pattern expects, DuckDB
  project-state expects, coverage-guided edge selection, multi-instance parallel fuzzing,
  CI gating.
