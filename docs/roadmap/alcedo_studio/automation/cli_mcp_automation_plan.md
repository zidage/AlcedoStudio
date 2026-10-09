# CLI, MCP, and Shared-Session Automation Plan

Date: 2026-10-08

Status: planned. No phase has implementation evidence.

Tracking issue: [#331](https://github.com/zidage/AlcedoStudio/issues/331). Progress is tracked in the issues, not in this document.

Primary owner: Alcedo Studio application shell (`alcedo_studio/src/ui/alcedo_main`) and the new
automation modules.

Affected areas:

- application startup (`alcedo_main` headless mode);
- `ApplicationModuleHost` and its controllers;
- editor session service and Mini-Git history storage;
- QML panels whose business logic moves to C++;
- CMake target graph, packaging, and GitHub CI;
- repository agent skills (`.agents/skills/`).

Related plans:

| Plan | Relation |
| --- | --- |
| [Editor Session Command Queue and Lock Simplification Plan](../ui/editor_session_command_queue_and_lock_simplification_plan.md) | Depends on it. Automation submits editor work through the same command queue. |
| [Background Tasks and Declarative UI State Plan](../ui/background_tasks_ui_state_plan.md) | Extends it. Agent control is one more background task with interaction locks. The existing task island and task dialog show it. |
| [UI Fuzz Automation Platform Plan](../ui/ui_fuzz_automation_platform_plan.md) | Independent. That plan drives the QML item tree with synthesized input. This plan drives owner APIs with named commands. The two do not share code, but this plan reuses its QLocalServer and JSON Lines precedent. |
| [Editor Single Live Pipeline + WAL + Checkpoint Simplification Plan](../ui/editor_single_live_pipeline_wal_checkpoint_plan.md) | Depends on it. Commit attribution extends the journal record and the commit materialization step. |
| [Phase 6C Mini-Git History and Pipeline Snapshot Plan](../ui/phase_6c_mini_git_history_and_pipeline_snapshot_plan.md) | Depends on it. Commit hashes stay unchanged. |

Source revision used for the audit: `f03ece5df` on `main`.

## 1. Decision record

### 1.1 Product goal

Alcedo Studio gets one named-command automation surface. Three clients use it:

1. **CI end-to-end tests.** CI starts a headless application process, drives the main user
   workflows through named commands, and verifies the result. The tests cover user-triggered
   operations and the operations that the application starts by itself (for example post-import
   processing, restore of the last edited image, journal recovery, and shutdown persistence).
   The main purpose is robustness under realistic user operation sequences.
2. **AI agents (for example Claude Code) through a shell.** The `alcedo-cli` program sends one
   command per invocation to a persistent session. The session keeps all state between
   invocations. An agent asks for preview images in a directory that it names.
3. **AI agents through MCP.** `alcedo-cli mcp` is a stdio MCP server. It exposes the same commands
   as MCP tools.

A session runs in one of two hosts:

- **Headless host:** `alcedo_main --headless`. No window and no QML. CI and unattended agent work
  use it.
- **GUI host:** the normal `alcedo_main` window with an automation server inside the process. The
  agent and the user share one application session. The user sees the agent's edits in the
  normal UI.

### 1.2 Approved decisions

| Topic | Decision |
| --- | --- |
| Agent and user in the GUI host | **Turn-taking control.** The agent must hold control to change state. While the agent holds control, the UI blocks user edits but keeps browsing and viewing available. The user can take control back at any time. After the user takes control back, the agent cannot take control again until the user returns it from the UI. The agent control state uses the existing background task notification path: the task island unfolds while the agent works, and a click shows the agent status. |
| Transport | JSON-RPC 2.0 over `QLocalSocket`, one JSON object per line. |
| QML business logic | Move it to C++. QML calls the same C++ operation. The CLI tests therefore exercise the same path that the UI uses. |
| CLI shape | A persistent session plus one-shot subcommands. Each `alcedo-cli` invocation is a short process. |
| Parameter units | The agent reads and writes editor parameters in UI units (the values that the panels show, for example saturation -100..100). |
| Commit attribution | Each history commit has an author. The default author is the user. Agent commits store `agent` and an optional note. The Versions and History panels show the author. |
| Headless host shape | `alcedo_main --headless`. It ships in the normal release package. `alcedo-cli` also ships in the release package. |
| CI assertions | Assert parameters, history, versions, database state, files, and session state. Do not compare rendered pixels with stored reference images. |
| Paid AI calls | Out of scope. Analysis commands are excluded (see 1.3). No cost confirmation is required. |
| Command scope | Project, import, library, editor, versions, render, export, and search. |

### 1.3 Explicit exclusions

- Image analysis (describe, score), semantic generation, and model download commands. They need
  downloaded models. Their current QML and C++ owners stay unchanged.
- LUT package management, update installation, and application settings commands.
- Reference-image pixel comparison in CI.
- The photo-editing taste skill. The user will design it later. This plan delivers only the
  operation skill that explains how to drive the CLI and MCP.
- Paste, Merge, mask authoring, node-graph editing, and comparison-view commands. The command
  registry design must allow them later without a protocol change.
- The detailed agent session logic in the GUI host: the content of the agent status view
  (command history, preview images, agent notes), messages between the agent and the user, and
  any richer control handoff. A separate plan designs it. AU14 delivers only the control
  behavior and the task presentation in 2.8.

### 1.4 Rejected options

- **gRPC as the automation transport.** Rejected. Each command would need a proto definition, and
  the MCP layer would need a second translation.
- **Free shared editing between agent and user without control.** Rejected in favor of
  turn-taking.
- **A separate logic copy in the CLI layer.** Rejected. QML and automation call one C++
  operation.
- **Reflection over every `Q_INVOKABLE`.** Rejected as the public command surface. The commands
  must have stable names, schemas, and completion behavior.

### 1.5 Unresolved decisions

These decisions need user confirmation before the named phase starts. The plan states a
proposed default.

| Decision | Proposed default | Needed by |
| --- | --- | --- |
| GUI automation server default | Enabled. The socket accepts only the current OS user. A setting "Allow agent control (CLI and MCP)" disables it. | Phase AU14 |
| `alcedo-cli` location in the macOS bundle and PATH exposure | `<Bundle>.app/Contents/Helpers/alcedo-cli`. The skill tells the user to add a symlink. The installer does not change PATH. | Phase AU15 |

## 2. Product design specification

### 2.1 Terms

- **Session:** one running `alcedo_main` process that has an automation server.
- **Session file:** a JSON file that describes a running session. Clients use it to find the
  socket.
- **Command:** one named JSON-RPC method with a parameter schema and a result schema.
- **Owner operation:** a C++ function on the existing data owner. A command calls owner
  operations only.
- **Agent control:** the state in which the agent can run state-changing commands in a GUI
  session.
- **Settled edit:** an adjustment write that creates one history commit.

### 2.2 Session files and discovery

- The session directory is `<AppLocalDataLocation>/automation/sessions/`. The
  `--session-dir <dir>` option overrides it in both programs. CI uses the override.
- A session file is named `<session-name>.json`. Its fields:

```json
{
  "protocol_version": 1,
  "session_name": "default",
  "socket_name": "alcedo-automation-12345",
  "pid": 12345,
  "host_mode": "headless",
  "project_path": "D:/Photos/trip.alcd",
  "started_at": "2026-10-08T09:00:00Z"
}
```

- The host writes the file after the server listens. The host removes the file during shutdown.
- `alcedo-cli` selects a session in this order: `--session <name>`, then the `ALCEDO_SESSION`
  environment variable, then the only live session. If several sessions are live and none is
  named, the CLI fails with exit code 2 and lists them.
- A session file is stale when its process does not exist or its socket does not answer `ping`.
  `alcedo-cli session list` reports stale files. `alcedo-cli session prune` removes them.

### 2.3 Protocol

- Framing: one UTF-8 JSON object per line. The line ends with `\n`.
- Requests and responses follow JSON-RPC 2.0. Notifications from the host carry no `id`.
- `QLocalServer` uses `QLocalServer::UserAccessOption`.
- The server accepts several connections. Each connection is independent. Agent control does not
  belong to a connection.
- `session.describe` returns `protocol_version`, `host_mode`, the application version, and the
  command list with each JSON Schema.
- A client with a different `protocol_version` major value fails with a clear message.

Error codes:

| Code | Name | Meaning |
| --- | --- | --- |
| -32700, -32600, -32601, -32602, -32603 | JSON-RPC standard | Parse error, invalid request, unknown method, invalid parameters, internal error. |
| -32001 | `not_ready` | The required state does not exist (for example no project is open). |
| -32002 | `rejected` | The owner rejected the operation. `data.reason` holds the owner message. |
| -32003 | `control_not_held` | A state-changing command arrived in a GUI session without agent control. |
| -32004 | `control_revoked_by_user` | The user took control back. The agent must wait until the user returns it. |
| -32005 | `busy` | An exclusive resource is in use (for example the image render port runs another job). |
| -32006 | `timeout` | The command did not reach its terminal state within `timeout_ms`. The operation can still finish later. |
| -32007 | `failed` | The operation started and failed. `data.reason` holds the owner message. |

### 2.4 Completion behavior

Every command states when it returns. A command returns only at a terminal state, or it returns
a `task_id` that `tasks.wait` accepts.

| Command group | Returns when |
| --- | --- |
| Reads (`*.get`, `*.list`, `*.query`, `state.get`) | The value is read from the owner. |
| `editor.set`, `editor.batch_set`, `editor.undo`, `editor.redo` | The history commit or head move is published, and the presented frame for that state is ready. |
| `editor.open` | The session is `Interactive` and the first frame is ready. |
| Version commands | The owner publishes the terminal history operation event. |
| `render.preview` | The PNG files are written. |
| `library.import`, `export.run` | The task starts. The result holds a `task_id`. |
| `tasks.wait` | The named task finishes, or all application work is idle (`--idle`). |

Every command accepts `timeout_ms`. The default is 120000.

### 2.5 Notifications

| Notification | Payload |
| --- | --- |
| `session.state_changed` | Editor session state and identity. |
| `history.changed` | Image identity, head commit, active Version. |
| `task.updated` | Task id, kind, progress, state. |
| `task.finished` | Task id, kind, final state, counts, error summary. |
| `control.changed` | `held`, `revoked_by_user`, `holder_name`. |

The CLI prints notifications only with `alcedo-cli watch`. The MCP server forwards them as MCP
logging notifications.

### 2.6 Command catalog (version 1)

UI units apply to every editor parameter value.

| Method | Parameters | Result |
| --- | --- | --- |
| `session.ping` | — | `{"pong": true}` |
| `session.describe` | — | protocol, host, command schemas |
| `session.shutdown` | `persist` (default true) | after `ApplicationModuleHost::Shutdown` |
| `state.get` | — | project, workspace, editor state, control state, running tasks |
| `control.acquire` | `holder_name` | control state |
| `control.release` | — | control state |
| `control.get` | — | control state |
| `project.create` | `folder`, `name` | project path |
| `project.open` | `path` | project summary |
| `project.save` | — | saved path |
| `project.close` | `persist` (default true) | — |
| `library.import` | `paths[]` or `folder`, `recursive` | `task_id` |
| `library.folders` | — | folder tree |
| `library.list` | `folder_id`, `offset`, `limit`, `sort` | items with `element_id`, `image_id`, file name, rating |
| `library.thumbnail` | `element_id`, `out`, `long_edge` | PNG path |
| `library.selection.get` | — | selected items (GUI selection) |
| `library.selection.set` | `element_ids[]` | selected items |
| `library.rate` | `element_ids[]`, `rating` 0..5 | `task_id` or applied count |
| `library.delete` | `element_ids[]`, `scope` (`project` or `album`) | deleted ids |
| `editor.open` | `element_id`, `image_id` | editor state, image identity |
| `editor.close` | `persist` (default true) | — |
| `editor.catalog` | — | parameter catalog in UI units |
| `editor.get` | `fields[]` (optional) | parameter values in UI units |
| `editor.set` | `field`, `value`, `note` | new head commit, values |
| `editor.batch_set` | `changes[]`, `note` | new head commits, values |
| `editor.undo` / `editor.redo` | — | head commit, values |
| `editor.history` | — | commits with author and note |
| `editor.actions` | — | action availability from `EditorActionPolicy` |
| `versions.list` | — | Versions and the active Version |
| `versions.create` | `name` | Version id |
| `versions.branch` | `name`, `from_commit` (default head) | Version id |
| `versions.checkout` | `version_id` | active Version |
| `versions.rename` | `version_id`, `name` | — |
| `versions.remove` | `version_id` | — |
| `render.preview` | `out_dir`, `long_edge`, `region`, `compare` (`none` or `root`), `file_stem` | PNG paths and pixel sizes |
| `export.options` | — | defaults, formats, bit depths, limits |
| `export.run` | `targets[]`, `out_dir`, options | `task_id` |
| `export.status` | — | queue rows and counts |
| `search.query` | `text`, `offset`, `limit`, `mode` (`auto`, `fuzzy`) | matches and paging values |
| `tasks.list` | — | running tasks |
| `tasks.wait` | `task_id` or `idle: true` | final task state |

`library.selection.*` is valid only in the GUI host. The headless host returns `not_ready`.

### 2.7 CLI behavior

- Form: `alcedo-cli [global options] <group> <verb> [arguments]`.
- Global options: `--session`, `--session-dir`, `--json`, `--timeout <ms>`.
- `--json` prints the JSON-RPC result or error object. Without it the CLI prints a short text
  summary.
- `alcedo-cli call <method> '<json>'` sends any command.
- `alcedo-cli schema` prints the command schemas.
- `alcedo-cli session start --headless --project <path> [--create <folder>,<name>]
  [--session <name>] [--editor-backend cuda|opencl|metal] [--viewport 1920x1080]
  [--settings-dir <dir>] [--log-file <path>]` starts a headless host and waits until the session
  file appears and `session.ping` succeeds.
- `alcedo-cli session stop` calls `session.shutdown` and waits for the process to exit.
- `alcedo-cli watch` prints notifications until interrupted.
- `alcedo-cli mcp` runs the MCP server on stdio.
- Exit codes: 0 success; 1 command error (JSON-RPC error); 2 session selection error; 3
  connection error; 4 usage error.

### 2.8 Agent control in the GUI host

Agent control uses the existing background task notification path. AU14 adds no banner, no new
window, and no new notification channel.

`control.acquire` registers one background task of the new kind `AgentControl` with
`BackgroundTaskController::RegisterTask`. The existing island (`BackgroundTaskBar.qml`) and the
existing task dialog (`BackgroundTasksDialog.qml`) show it:

```text
top toolbar, agent at work (island unfolded, lamp pulses, indeterminate progress hairline):
  ... [ (o) Agent · claude-code · editor.set saturation 20 ] [right sidebar toggle]
```

| Task field | Value |
| --- | --- |
| Kind label | "Agent" |
| Title | The holder name, for example `claude-code` |
| Detail | The summary of the running command, for example `editor.set saturation 20`. "Waiting for the next command" between commands. |
| Progress | Indeterminate (-1) |
| Cancelable | Yes. Cancel equals **Take control**. |
| State | Running while the agent holds control. Succeeded when the agent releases control. Canceled when the user takes control. |
| Locks | `UserStateChange` (global) |

Rules:

- The island unfolds while the agent works, by the existing rule for a running task.
- A click on the island shows the agent status through the existing task path
  (`BackgroundTasksDialog.qml` and its `taskDetailsRequested` signal). AU14 shows only the task
  fields above. The full agent status view belongs to the separate session design (1.3).
- The handler of each state-changing command calls `BackgroundTaskController::UpdateTask` with
  the command summary when the command starts. It sets the waiting text when the command ends.
- `control.acquire` succeeds when no other agent holds control, the user did not revoke control,
  and the editor has no unsealed slider input. If the editor has unsealed input, the acquire waits
  for the Release or Cancel boundary, with the command timeout.
- While the agent holds control, the UI blocks these user actions: adjustment panels, viewport
  edit tools, Undo, Redo, history head moves, Version mutation and checkout, image switching in
  the editor, import, delete, rating, export start, and project open, create, or close.
- While the agent holds control, the UI keeps these actions: library browsing, thumbnail
  scrolling, zoom and pan in the viewport, the History and Versions panels as read-only views,
  search, settings, and the task island and task dialog.
- **Take control** is the cancel action of the agent task. It releases agent control at once.
  The next agent command returns `control_revoked_by_user`.
- After **Take control**, `control.acquire` returns `control_revoked_by_user` until the user
  selects **Return control**. AU14 adds **Return control** as one row action on the canceled agent
  task in `BackgroundTasksDialog.qml`.
- New strings use the translation system. The `.ts` files are edited manually.
- In the headless host the agent always holds control. `control.*` commands report `held: true`.
  The headless host registers no agent task.

### 2.9 Parameter catalog

`editor.catalog` returns one entry for each field key. Example entries:

```json
{"field": "exposure", "kind": "scalar", "ui_min": -10, "ui_max": 10, "ui_default": 0,
 "ui_step": 0.01, "panel": "tone"}
{"field": "saturation", "kind": "scalar", "ui_min": -100, "ui_max": 100, "ui_default": 0,
 "ui_step": 1, "panel": "look"}
{"field": "odt", "kind": "object", "panel": "display", "schema": {"...": "..."}}
```

- Scalar fields accept a number. Structured fields accept an object in UI units, defined by the
  entry schema.
- The catalog validates the UI value before conversion. An out-of-range value returns
  `-32602` with the allowed range. The catalog does not clamp silently.
- The catalog converts UI values to model JSON and calls `ParseEditorParameterWrite`.
- The QML panels read ranges, defaults, steps, and option lists from the same catalog.

### 2.10 Commit attribution

- Storage: a new DuckDB table `EditCommitAttribution(commit_hash VARCHAR PRIMARY KEY,
  author VARCHAR NOT NULL, note VARCHAR)`.
- The application writes a row only for an agent commit. A commit without a row has the author
  `user`.
- The commit hash does not include attribution. Existing hashes and readers stay valid.
- The Mini-Git journal record carries attribution until materialization. Journal record format
  version 7 adds an optional `attribution` object. The reader accepts versions 6 and 7. A version
  6 record has the author `user`.
- Commit materialization writes the attribution rows in the same transaction as the commits.
- Mini-Git garbage collection removes attribution rows whose commit no longer exists.
- The History panel shows an agent badge and the note for agent commits.

## 3. Scope

### 3.1 Included work by module

| Module | Responsibility |
| --- | --- |
| `AutomationProtocol` library (proposed, `src/automation/`) | JSON-RPC message types, line framing, error codes, session file read and write. Qt Core and Qt Network only. |
| `AutomationHostLib` library (proposed, `src/ui/alcedo_main/automation/`) | Command registry, server, command handlers, headless frame sink, agent control owner. Links `AlbumBackendLib`. |
| `alcedo_main` | `--headless` startup branch, GUI automation server start. |
| `alcedo_cli` executable (proposed, output name `alcedo-cli`) | CLI client and MCP server. |
| Album backend controllers | New C++ operations that replace QML logic. |
| Editor session service and Mini-Git storage | Completion correlation for settled input, commit attribution. |
| QML | Calls the new C++ operations, reads the parameter catalog, shows the agent control task and attribution. |
| Tests and CI | Automation unit tests, headless end-to-end tests, CI label and preset changes. |
| Packaging | Install rules for `alcedo-cli` on Windows and macOS. |
| `.agents/skills/alcedo-automation/` (proposed) | Operation skill for agents. |

### 3.2 Exclusions and their owners

See 1.3. The excluded features keep their current QML and controller owners.

## 4. Current source audit

| Area | Current owner and path | Current behavior | Required change |
| --- | --- | --- | --- |
| Composition root | `ApplicationModuleHost` (`src/include/ui/alcedo_main/album_backend/application_module_host.hpp:52`) | Builds all controllers. Runs under `QCoreApplication` in tests (`tests/ui/ui_test_main.cpp:18`). `AttachQmlEngine` is optional. | Host the command registry in both host modes. |
| Startup | `src/ui/alcedo_main/main.cpp:194-389` | Always creates `QApplication` and loads `Main.qml`. Hand-written argument scan. No IPC. | Add a `--headless` branch with `QCommandLineParser`. |
| Backend before window | `src/ui/editor_rhi/editor_startup.cpp:111-231` | Sets the Qt graphics API and creates the backend context. `BindEditorGraphicsToWindow` only shares the device with Qt Quick. | Headless mode resolves the backend without a window. |
| Settings | About 70 `QSettings{}` sites in 21 files. `main.cpp:120` reads `NativeFormat` explicitly. | All default-constructed sites can be redirected with `setDefaultFormat` and `setPath`. | `--settings-dir`; change the `main.cpp:120` read to the default format. |
| Editor session API | `EditorSessionService` (`src/include/app/editor_session_service.hpp`) | Typed calls through one command queue. Results carry `operation_id` (`editor_session_types.hpp:55-66`). | Reuse. |
| Settled input correlation | `EditorSessionService::EnqueueAdjustmentInput` and `EnqueuePendingInputBoundary` (`src/app/editor_session_service.cpp:1231-1280`) | These calls bypass `SubmitCommand` and publish `operation_id = 0`. | Give the Release boundary a command id so its results correlate. |
| First frame | `editor_session_render_controller.cpp:266-333`, `editor_session_service.cpp:1233` | The session stays in `Loading` until a presentation sink id and size exist and the first frame is ready. Edits are rejected before `Interactive`. | Headless frame sink bound through `EditorSessionController`. |
| Sink resolution | `application_module_host.cpp:422-424`, `editor_session_render_scheduler_port.cpp:454-492` | The resolver returns `EditorSessionController::presentation_frame_sink()`. | Return the headless sink in headless mode. |
| Image render port | `IEditorImageRenderPort::ScheduleImages` (`src/include/app/editor_image_render_port.hpp:41-100`) | Renders one or two snapshots to CPU RGBA32F. One job at a time. Only `EditorComparisonService` uses it. `RenderRequest.view.visible_rect_in_edit_space` selects a region. | Use for `render.preview`. |
| Parameter write | `ParseEditorParameterWrite` (`src/app/editor_parameter_write_parse.cpp:845`), `kFieldKeys` (`src/app/editor_adjustment_pipeline.cpp:17-46`) | Model-unit JSON per field. No C++ table of UI ranges. | Add `EditorParameterCatalog`. |
| UI value logic | `editor_adjustment_models.cpp:37-46`, panel QML files | UI ranges, defaults, and conversions live in QML and in Qt models. About 900 QML JS lines. | Move to the catalog. |
| Commits | `EditCommit` (`src/include/edit/history/edit_commit.hpp:68-72`), DuckDB table `EditCommit` (`src/include/storage/store/database.hpp:65-72`) | Exact-key readers. The hash covers the payload. Journal record format 6. | Side table plus journal format 7. |
| Interaction locks | `BackgroundTaskController` (`background_task_controller.hpp:56-81`), `InteractionPolicyController` | 15 capabilities. Editor navigation locks also feed `EditorSessionService` admission (`editor_session_controller.cpp:251-262`). | Add one capability that only user-facing adapters read. |
| Export logic | `ExportInspectorPanel.qml:27-406`, `ExportQueueState.qml` | Defaults, validation, queue, and argument assembly live in QML. C++ has `StartExportWithRecipeOptionsForTargets` (`import_export.hpp:94`). No finish signal. | Move to C++. |
| Selection, rating, delete | `SelectionState.qml`, `ImageActionsController.qml:142-547` | Selection, target rules, and post-delete pruning live in QML. | Move to C++. |
| Search | `GlobalSearchDialog.qml:187-555` | Routing, request matching, and paging live in QML. | Move to C++. |
| Project launch and close | `ProjectLaunchController.qml:39-212`, `Main.qml:284-488` | Launch rollback and the close state machine live in QML. `ProjectModule` has no close invokable. | Move to C++. |
| Import | `AppDialogs.qml:40-45` | A QML extension list duplicates `kRawExtensions` (`src/include/type/supported_file_type.hpp:96`). `StartImportPaths` is C++ only. | Read the list from C++. |
| Idle condition | `ApplicationModuleHost::ShutdownModules` (`application_module_host.cpp:636-642`) | An inline loop tests running tasks, import, and export. | Extract one owner predicate. |
| Test host precedent | `src/ui/alcedo_studio_test_host/` | QLocalServer and JSON Lines input probe. Not in CI. | No change. |
| CI | `.github/workflows/cpp-ci.yml`, preset `macos_arm_metal_ci` (`CMakePresets.json:120-141, 214-222, 253-266`) | macOS 15 arm64 Metal. Labels `ci_core_flow|ci_raw_flow|ci_metal_runtime`. No test has `ci_metal_runtime`. | Add `ci_automation_flow`. |
| Packaging | Root `CMakeLists.txt:1061-1102, 935-946, 1646-1679` | Installs `alcedo_main` and `alcedo_update_installer`. macOS helper precedent in `Contents/Helpers`. | Install `alcedo-cli`. |
| Windows subsystem | `alcedo_main/CMakeLists.txt:414-419` | `alcedo_main` is a GUI-subsystem program. | The headless host writes logs to a file. `alcedo-cli` is a console program. |

## 5. Target architecture

### 5.1 Layers

```text
Claude Code (Bash) -> alcedo-cli <group> <verb>      Claude Code -> alcedo-cli mcp (stdio)
                         |  JSON-RPC 2.0, JSON Lines, QLocalSocket   |
                         +-------------------+---------------------+
                                             v
                                    AutomationServer
                                             v
                                AutomationCommandRegistry
           (method name, parameter schema, result schema, handler, control requirement)
                                             v
          Owner operations moved out of QML (QML calls the same operations)
          ProjectLaunchCoordinator, ApplicationCloseCoordinator, LibrarySelection,
          LibraryMutationOperations, EditorParameterCatalog, VersionNameRules,
          ExportQueue, ExportRecipeBuilder, SearchSession
                                             v
          ApplicationModuleHost -> controllers -> app services -> EditorSessionService queue

Host mode A: alcedo_main (GUI)  -> QApplication + Main.qml + AutomationServer
Host mode B: alcedo_main --headless -> QCoreApplication + HeadlessFrameSink + AutomationServer
```

### 5.2 Owners

| Owner | Input | Output | Can change | Reads only | Lifetime | Error surface | Publisher |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `AutomationServer` | Socket lines | Response and notification lines | Connection table | Registry | Session | JSON-RPC errors | GUI thread |
| `AutomationCommandRegistry` | Method, params | Result or error | Nothing directly | Handler table | Session | Error codes in 2.3 | Handler |
| Command handlers | Validated params | Result JSON | Through owner operations only | Owner read APIs | Per call | Owner error mapped to 2.3 | Handler completion on the GUI thread |
| `AgentControlOwner` | `control.*`, agent task cancel, Return control | Control state, agent task | Control state, its background task | Editor pending input state | Session | `control_*` errors | GUI thread |
| `HeadlessFrameSink` | Frames from the render scheduler | Latest host frame | Its own pixel buffer | — | Headless session | Frame failure through the existing render result | Render worker writes, GUI thread reads under its mutex |
| `EditorParameterCatalog` | Field key, UI value | Model JSON, UI value | Nothing | Catalog table | Static | Range or shape error | Caller |
| `EditCommitAttribution` storage | Attribution in journal records | Table rows | Its table | Commit table | Project | Storage error through the existing persistence result | Persistence worker |

### 5.3 Thread rules

- `AutomationServer` and every handler run on the GUI thread (`QCoreApplication` thread in
  headless mode).
- A handler never blocks the GUI thread. It starts the owner operation and waits for the owner's
  existing signal or result observer. Then it sends the response.
- Editor work goes through `EditorSessionService`. Its command queue owns the ordering.
- The plan adds no new consistency mechanism. The Release-boundary command id in AU10 uses the
  existing `operation_id` field. Its purpose is result correlation for a client, not a stale-result
  guard.

### 5.4 Primary success chain: agent sets saturation in a GUI session

```text
alcedo-cli editor set saturation 20 --note "lift color"
  -> AutomationServer (GUI thread) parses line
  -> registry: editor.set requires control -> AgentControlOwner.held == true
  -> EditorParameterCatalog.ToModelWrite("saturation", 20) -> model JSON {"saturation": 1.2}
  -> ParseEditorParameterWrite -> EditorAdjustmentPatch{settled = true, target resolved}
  -> EditorSessionService::EnqueueAdjustmentInput
  -> EditorSessionService::EnqueuePendingInputBoundary(Release, attribution{agent, note})
       -> command id N
  -> queue consume -> MiniGitWorkingHistory append (journal record v7 with attribution)
  -> result RenderRouted(operation_id N, render_request_id R)
  -> frame R ready -> result observer
  -> handler responds {"head_commit": "...", "values": {"saturation": 20}}
  -> QML viewport shows the frame. History panel shows the agent badge.
```

### 5.5 Primary failure chain: user takes control during an agent batch

```text
editor.batch_set (3 changes) in progress, change 1 committed
  -> user clicks Take control
  -> AgentControlOwner releases the lock task, sets revoked_by_user = true, emits control.changed
  -> handler checks control before change 2 -> responds -32004 control_revoked_by_user,
     data.applied = 1, data.head_commit = commit of change 1
  -> history keeps change 1 as a normal agent commit; the user can Undo it
```

## 6. File and API map

### 6.1 Current files that change

- `src/ui/alcedo_main/main.cpp` — headless branch, GUI server start.
- `src/ui/alcedo_main/CMakeLists.txt` — new libraries and the CLI executable.
- `src/ui/alcedo_main/album_backend/application_module_host.cpp` and header — idle predicate,
  headless sink resolver, automation owner members.
- `src/ui/alcedo_main/album_backend/editor_session_controller.cpp` and header — headless sink
  binding, user-input checks for agent control.
- `src/app/editor_session_service.cpp` and header — Release boundary command id, attribution
  parameter.
- `src/edit/history/mini_git_working_history.cpp`, journal record reader and writer, commit graph
  store, `src/include/storage/store/database.hpp` — attribution.
- `src/ui/alcedo_main/album_backend/background_task_controller.cpp` and header,
  `interaction_policy_controller.cpp` and header — new capability and task kind.
- `src/ui/alcedo_main/album_backend/import_export.cpp` and header, `image_controller.cpp`,
  `library_module.cpp`, `search_controller.cpp`, `project_module.cpp`, headers — new
  operations.
- QML: `Main.qml`, `ProjectLaunchController.qml`, `AppDialogs.qml`, `SelectionState.qml`,
  `ImageActionsController.qml`, `ExportInspectorPanel.qml`, `ExportQueueState.qml`,
  `ShellSignals.qml`, `GlobalSearchDialog.qml`, `EditorTonePanel.qml`, `EditorLookPanel.qml`,
  `EditorPostProcessPanel.qml`, `EditorRawDecodePanel.qml`, `EditorGeometryPanel.qml`,
  `EditorDisplayTransformPanel.qml`, `EditorWhiteBalanceSliders.qml`, `EditorVersionsPanel.qml`,
  `BackgroundTaskBar.qml`, `BackgroundTasksDialog.qml`, the History panel delegate.
- Root `CMakeLists.txt`, `scripts/verify_windows_install_tree.ps1`, `CMakePresets.json`,
  `.github/workflows/cpp-ci.yml`.
- `docs/roadmap/README.md` — link to this plan.

### 6.2 Proposed files

| Path | Content |
| --- | --- |
| `src/include/automation/automation_protocol.hpp`, `src/automation/automation_protocol.cpp` | `AutomationRequest`, `AutomationResponse`, `AutomationNotification`, `AutomationErrorCode`, `ParseAutomationLine`, `SerializeAutomationMessage`. |
| `src/include/automation/automation_session_file.hpp`, `src/automation/automation_session_file.cpp` | `AutomationSessionFile`, `WriteAutomationSessionFile`, `ReadAutomationSessionFiles`, `RemoveAutomationSessionFile`, `DefaultAutomationSessionDir`. |
| `src/include/ui/alcedo_main/automation/automation_command_registry.hpp` and `.cpp` | `AutomationCommandSpec`, `AutomationCommandRegistry`. |
| `src/include/ui/alcedo_main/automation/automation_server.hpp` and `.cpp` | `AutomationServer` (QObject). |
| `src/include/ui/alcedo_main/automation/headless_frame_sink.hpp` and `.cpp` | `HeadlessFrameSink : IFrameSink`. |
| `src/include/ui/alcedo_main/automation/agent_control_owner.hpp` and `.cpp` | `AgentControlOwner` (QObject, QML-visible). |
| `src/ui/alcedo_main/automation/commands/*.cpp` | One file per command group. |
| `src/ui/alcedo_main/headless_host.cpp` and header | `RunHeadlessHost(const HeadlessHostOptions&)`. |
| `src/ui/alcedo_cli/main.cpp`, `cli_commands.cpp`, `mcp_server.cpp` and headers | CLI and MCP. |
| `src/include/app/editor_parameter_catalog.hpp`, `src/app/editor_parameter_catalog.cpp` | `EditorParameterCatalog`. Qt-free. |
| `src/include/app/version_name_rules.hpp`, `src/app/version_name_rules.cpp` | `NormalizeVersionName`, `DefaultVersionName`. |
| `src/include/edit/history/edit_commit_attribution.hpp` | `EditCommitAttribution{author, note}`. |
| Album backend headers and sources: `project_launch_coordinator`, `application_close_coordinator`, `library_selection`, `library_mutation_operations`, `export_queue`, `export_recipe_builder`, `search_session` | Operations moved out of QML. |
| `tests/automation/` | Unit and end-to-end tests. |
| `.agents/skills/alcedo-automation/SKILL.md` | Operation skill. |

### 6.3 Proposed API details

```cpp
// Session-lifetime registry. GUI thread only.
struct AutomationCommandSpec {
  std::string    method;              // "editor.set"
  QJsonObject    params_schema;       // JSON Schema draft 2020-12
  QJsonObject    result_schema;
  bool           changes_state;       // true -> requires agent control in a GUI session
  std::function<void(const QJsonObject& params, AutomationReply reply)> handler;
};

class AutomationCommandRegistry {
 public:
  void Register(AutomationCommandSpec spec);           // duplicate method -> fatal at startup
  void Dispatch(const AutomationRequest& request, AutomationReply reply);
  auto Describe() const -> QJsonArray;
};
```

- `AutomationReply` is a move-only callable that sends exactly one response. A destroyed reply
  that has not sent a response sends `-32603` with the reason "handler dropped the request".
- `Dispatch` validates parameters against `params_schema` before it calls the handler.
- `EditorParameterCatalog::Entries()` returns a static table. `ToModelJson(field, ui_value,
  current_model_json)` returns model JSON or an error string. `ToUiValue(field, model_json)`
  returns the UI value.
- `EditorSessionService::EnqueuePendingInputBoundary(EditorPendingInputBoundaryKind kind,
  std::optional<EditCommitAttribution> attribution)` changes state. It validates the attribution
  author (`agent` only; empty means user).
- `ApplicationModuleHost::IsIdle() const -> bool` reads task, import, and export state.

## 7. Phase summary

| Phase | Result | Main modules | Dependency | Expected diff | Status | Issue |
| --- | --- | --- | --- | ---: | --- | --- |
| AU1 | Protocol, registry, server | `AutomationProtocol`, `AutomationHostLib` | — | 1100–1500 | planned | [#323](https://github.com/zidage/AlcedoStudio/issues/323) |
| AU2 | `alcedo-cli` client and session files | `alcedo_cli` | AU1 | 800–1200 | planned | [#323](https://github.com/zidage/AlcedoStudio/issues/323) |
| AU3 | Headless host and session lifecycle | `main.cpp`, headless host, frame sink | AU1, AU2 | 1000–1500 | planned | [#323](https://github.com/zidage/AlcedoStudio/issues/323) |
| AU4 | Project launch and close in C++, project commands | Project coordinators, QML | AU3 | 900–1400 | planned | [#324](https://github.com/zidage/AlcedoStudio/issues/324) |
| AU5 | Import, library reads, thumbnails, tasks, CI wiring | Import, library, CI | AU4 | 1000–1500 | planned | [#324](https://github.com/zidage/AlcedoStudio/issues/324) |
| AU6 | Selection, rating, delete in C++, commands | Library operations, QML | AU5 | 900–1300 | planned | [#324](https://github.com/zidage/AlcedoStudio/issues/324) |
| AU7 | Parameter catalog: scalar fields | Catalog, Tone, Look, PostProcess QML | AU1 | 1100–1600 | planned | [#325](https://github.com/zidage/AlcedoStudio/issues/325) |
| AU8 | Parameter catalog: RAW, input profile, lens, crop | Catalog, Raw and Geometry QML | AU7 | 1300–1700 | planned | [#325](https://github.com/zidage/AlcedoStudio/issues/325) |
| AU9 | Parameter catalog: display transform and color fields | Catalog, Display, Look, white balance QML | AU8 | 1200–1700 | planned | [#325](https://github.com/zidage/AlcedoStudio/issues/325) |
| AU10 | Editor commands and `render.preview` | Editor session, render port | AU3, AU7 | 1300–1800 | planned | [#326](https://github.com/zidage/AlcedoStudio/issues/326) |
| AU11 | Version commands and commit attribution | Session, journal, storage, QML | AU10 | 1300–1800 | planned | [#326](https://github.com/zidage/AlcedoStudio/issues/326) |
| AU12 | Export in C++, export commands | Export queue and recipe, QML | AU5 | 1100–1600 | planned | [#327](https://github.com/zidage/AlcedoStudio/issues/327) |
| AU13 | Search in C++, search commands | Search session, QML | AU5 | 900–1300 | planned | [#327](https://github.com/zidage/AlcedoStudio/issues/327) |
| AU14 | GUI automation server and agent control | Control owner, policy, background task island | AU6, AU10, AU11, AU12 | 1100–1600 | planned | [#328](https://github.com/zidage/AlcedoStudio/issues/328) |
| AU15 | MCP server, operation skill, packaging | `alcedo_cli`, skill, install rules | AU14 | 1100–1600 | planned | [#329](https://github.com/zidage/AlcedoStudio/issues/329) |
| AU16 | Operation-sequence robustness and recovery tests | Tests, CI | AU11, AU12, AU13 | 900–1400 | planned | [#330](https://github.com/zidage/AlcedoStudio/issues/330) |

AU7 to AU9 can run in parallel with AU4 to AU6. AU8 and AU9 are split because the structured
field builders are about 700 QML lines and their tests are large. One combined phase can pass
2000 lines.

## 8. Repository rules that affect this work

Read `AGENTS.md` and every applicable skill again before each phase. The rules can change.

- Update data through its owner. A command handler must not copy a whole model, edit it, and
  write it back.
- Do not add a fallback. A headless backend failure fails the start with the real error. Do not
  switch to another backend or a CPU path.
- Do not add a consistency mechanism without an executable interleaving.
- Include the defining header. Do not add forward declarations to save includes.
- Do not use the prohibited terms in code, tests, or text.
- Use LF line endings. Check files that are still CRLF (for example `import_export.cpp`) and
  convert them in a separate commit before the content change.
- Run MSVC builds through the PowerShell tool with `scripts\msvc_env.cmd`.
- Do not run the full `ctest` suite. Run the named targets.
- Do not run `lupdate`. Edit the `.ts` files by hand for new strings.
- Use the `alcedo-qml-ui` skill for QML changes and the `alcedo-msvc-cmake` skill for build
  changes.
- QML offscreen input tests are not reliable. Verify behavior through C++ owner tests and
  automation tests.

## 9. Detailed phases

### Phase AU1 — Protocol, command registry, and server

**Objective and deliverables**

- `AutomationProtocol` library with message types, framing, and error codes.
- `AutomationCommandRegistry` with schema validation and one-reply dispatch.
- `AutomationServer` on `QLocalServer` with several connections and notifications.
- Commands `session.ping` and `session.describe`.
- A reviewer can run `AutomationServerTest` and see a request, a response, and a notification
  pass through a real local socket.

**Inputs and prerequisites**

- Source at the audit revision or later.
- Qt 6 Core and Network.

**Modules, files, and APIs**

- Proposed: `src/include/automation/automation_protocol.hpp`, `src/automation/automation_protocol.cpp`,
  `src/automation/CMakeLists.txt` (library `AutomationProtocol`).
- Proposed: `automation_command_registry.{hpp,cpp}`, `automation_server.{hpp,cpp}` under
  `src/ui/alcedo_main/automation/` (library `AutomationHostLib`, links `AlbumBackendLib`,
  `AutomationProtocol`, `Qt6::Network`).
- Proposed: a JSON Schema subset validator inside the registry. It supports `type`,
  `properties`, `required`, `additionalProperties: false`, `enum`, `minimum`, `maximum`,
  `items`, and `minItems`. It rejects a schema keyword that it does not support at registration.

**Data rules and invariants**

- One request produces exactly one response.
- A line longer than 16 MiB closes the connection with a parse error.
- Method names are unique. A duplicate registration terminates startup with a log message.
- Unknown parameters fail validation (`additionalProperties: false` on every command).
- `protocol_version` is 1.

**Implementation steps**

1. Write the protocol types and `ParseAutomationLine`. Return a typed error for invalid JSON,
   a missing `jsonrpc: "2.0"`, a missing `method`, or a non-object `params`.
2. Write `SerializeAutomationMessage`. Output compact JSON plus `\n`.
3. Write the schema subset validator. Report the JSON pointer of the first failure.
4. Write the registry. `Dispatch` validates, then calls the handler with an `AutomationReply`.
5. Write the server. Use `QLocalServer::UserAccessOption`. Keep a buffer for each connection.
   Split on `\n`. Send notifications to all connections.
6. Register `session.ping` and `session.describe`.
7. Add CMake targets and the `automation` test directory.

**Primary success call chain**

```text
QLocalSocket readyRead -> buffer split -> ParseAutomationLine -> Registry::Dispatch
  -> schema check -> handler -> AutomationReply::Send -> socket write
```

**Primary failure and restore call chain**

```text
invalid params -> schema check fails -> -32602 with JSON pointer -> connection stays open
handler drops reply -> reply destructor -> -32603 "handler dropped the request"
```

**Tests and evidence**

| Test | Assertion |
| --- | --- |
| `AutomationProtocolTest.ParsesValidRequestLine` | Method, id, and params match the input. |
| `AutomationProtocolTest.RejectsLineWithoutJsonRpcVersion` | Error code -32600. |
| `AutomationCommandRegistryTest.RejectsUnknownParameterWithJsonPointer` | Error -32602 with `/extra`. |
| `AutomationCommandRegistryTest.DroppedReplySendsInternalError` | Exactly one -32603 response. |
| `AutomationCommandRegistryTest.DuplicateMethodRegistrationFails` | Registration reports failure. |
| `AutomationServerTest.PingRoundTripsOverLocalSocket` | `{"pong": true}` with the same id. |
| `AutomationServerTest.NotificationReachesAllConnections` | Two clients receive one notification each. |
| `AutomationServerTest.TwoRequestsOnOneLineBufferAreAnsweredInOrder` | Responses arrive in request order. |

**Build and run commands**

```powershell
Set-Location D:\Projects\pu-erh_lab
cmd /c scripts\msvc_env.cmd --build --preset win_debug --target AutomationProtocolTest AutomationServerTest --parallel 4
ctest --test-dir build/debug -R "Automation(Protocol|CommandRegistry|Server)Test" --output-on-failure
```

Put logs under `build/tmp/automation_au1/`.

**Exit criteria**

- [ ] All tests above are discovered and pass.
- [ ] `session.describe` returns both commands with schemas.
- [ ] No prohibited term in new files.

**Expected diff:** 1100–1500 lines.

**Completion record:** see the template in section 13.

### Phase AU2 — `alcedo-cli` client and session files

**Objective and deliverables**

- Session file read, write, removal, and stale detection.
- The `alcedo_cli` console executable (output name `alcedo-cli`) with `call`, `schema`,
  `session list`, `session prune`, and `watch`.
- The text and `--json` output modes and the exit codes in 2.7.

**Inputs and prerequisites**

- AU1.

**Modules, files, and APIs**

- Proposed: `src/include/automation/automation_session_file.hpp` and `.cpp` in
  `AutomationProtocol`.
- Proposed: `src/ui/alcedo_cli/main.cpp`, `cli_commands.{hpp,cpp}`. The target links only
  `AutomationProtocol`, `Qt6::Core`, and `Qt6::Network`. It must not link `AlbumBackendLib`.
- The CLI sets the organization and application names to the same values as `main.cpp:194-197`
  so `AppLocalDataLocation` resolves to the same directory.

**Data rules and invariants**

- The session file is written atomically (`QSaveFile`).
- A session file with an unknown `protocol_version` major value is listed but not used.
- The CLI never edits a session file except during `session prune`.

**Implementation steps**

1. Write the session file API and `DefaultAutomationSessionDir`.
2. Write session selection in the order given in 2.2.
3. Write a synchronous client: connect, send one request, read lines until the matching
   response, ignore notifications, and apply `--timeout`.
4. Write `call`, `schema`, `session list`, `session prune`, and `watch`.
5. Add a test server fixture that registers the AU1 commands in-process.

**Primary success call chain**

```text
alcedo-cli call session.ping '{}' -> select session -> connect -> send -> read response
  -> print -> exit 0
```

**Primary failure and restore call chain**

```text
two live sessions, no --session -> exit 2 with the session names
socket does not answer -> exit 3 with the socket name and the Qt error string
```

**Tests and evidence**

| Test | Assertion |
| --- | --- |
| `AutomationSessionFileTest.WrittenFileReadsBackWithSameFields` | All fields round-trip. |
| `AutomationSessionFileTest.DeadProcessFileIsReportedStale` | `stale == true`. |
| `AlcedoCliTest.CallPrintsResultAndExitsZero` | Exit 0 and the JSON result in `--json` mode. |
| `AlcedoCliTest.AmbiguousSessionExitsTwoAndListsNames` | Exit 2, both names in stderr. |
| `AlcedoCliTest.CommandErrorExitsOneWithCode` | Exit 1 and the error code in the output. |
| `AlcedoCliTest.WatchPrintsNotification` | One notification line appears. |

**Build and run commands**

```powershell
cmd /c scripts\msvc_env.cmd --build --preset win_debug --target alcedo_cli AlcedoCliTest AutomationSessionFileTest --parallel 4
ctest --test-dir build/debug -R "AlcedoCliTest|AutomationSessionFileTest" --output-on-failure
```

**Exit criteria**

- [ ] Tests pass.
- [ ] `alcedo_cli` links no album backend library (check the link line in the build log).

**Expected diff:** 800–1200 lines.

**Completion record:** see section 13.

### Phase AU3 — Headless host and session lifecycle

**Objective and deliverables**

- `alcedo_main --headless` runs `ApplicationModuleHost` under `QCoreApplication` without QML.
- `HeadlessFrameSink` lets the editor session reach `Interactive`.
- Options: `--project`, `--create <folder> <name>`, `--session`, `--session-dir`,
  `--editor-backend`, `--viewport WxH`, `--settings-dir`, `--log-file`.
- `ApplicationModuleHost::IsIdle()` replaces the inline loop condition.
- `session.shutdown`, `state.get`, and `alcedo-cli session start` and `session stop`.

**Inputs and prerequisites**

- AU1, AU2.

**Modules, files, and APIs**

- `src/ui/alcedo_main/main.cpp`: detect `--headless` before any application object exists and
  call `RunHeadlessHost`. The GUI path keeps its current argument handling.
- Proposed: `src/ui/alcedo_main/headless_host.{hpp,cpp}`.
- Proposed: `headless_frame_sink.{hpp,cpp}`. Model it on `HostPixelFrameSink`
  (`tests/ui/editor_session_render_scheduler_port_test.cpp:119-148`): RGBA32F host memory,
  `FrameMemoryDomain::HostVisible`, a mutex around the buffer.
- `EditorSessionController`: add `BindHeadlessPresentationSink(IFrameSink* sink, int width,
  int height)`. It sets the presentation sink, calls `SetPresentationSinkId` with the sink
  address, and calls `SetPresentationSize`. This mirrors the viewport binding at
  `editor_session_controller.cpp:1161-1222`.
- `application_module_host.{hpp,cpp}`: `IsIdle()`; `ShutdownModules` uses it.
- `main.cpp:120` `ReadConfiguredEditorBackend`: read with the default format when
  `--settings-dir` is set.

**Data rules and invariants**

- `--settings-dir` calls `QSettings::setDefaultFormat(QSettings::IniFormat)` and
  `QSettings::setPath(IniFormat, UserScope, dir)` before the first `QSettings` construction.
- The backend is resolved with `ResolveAcceleratorBackend` and the existing context creation.
  Metal uses `MetalContext::Instance()`. OpenCL uses `InitializeOpenClRuntime()` without GL
  sharing. CUDA uses the primary context. A failure stops startup with exit code 3 and the
  backend error. No other backend is tried.
- The headless host never calls `AttachQmlEngine`, `QFileDialog`, or `QDesktopServices`.
- The session file exists only while the server listens.

**Implementation steps**

1. Parse headless options with `QCommandLineParser`.
2. Apply settings redirection, then create `QCoreApplication` with the production names.
3. Resolve the backend. Fail on error.
4. Create `ApplicationModuleHost`. Bind `HeadlessFrameSink` through
   `BindHeadlessPresentationSink`. Point the scheduler sink resolver at it
   (`application_module_host.cpp:422-424`).
5. Open or create the project with `ProjectModule::LoadProject` or
   `CreateProjectInFolderNamed`. Wait for the load signal. Fail with exit code 4 on load failure.
6. Start `AutomationServer`, register `session.*` and `state.get`, write the session file.
7. Implement `session.shutdown`: respond after `ApplicationModuleHost::Shutdown()` returns,
   remove the session file, quit the event loop.
8. Implement `alcedo-cli session start`: find the host binary (same directory on Windows,
   `../MacOS/<bundle executable>` from `Contents/Helpers` on macOS, or `--host-binary`), start it
   detached, wait for the session file and `session.ping` (bounded by `--timeout`).
9. Implement `alcedo-cli session stop`.
10. Confirm that no QML timer behavior is needed: update checks, LUT feed checks, and
    accelerator preparation are QML-only and do not run in headless mode. Record this in the
    completion record.

**Primary success call chain**

```text
alcedo-cli session start --headless --project P
  -> spawn alcedo_main --headless ... -> QCoreApplication -> backend resolved
  -> ApplicationModuleHost -> headless sink bound -> project loaded
  -> server listens -> session file written -> CLI ping ok -> exit 0
```

**Primary failure and restore call chain**

```text
Metal device creation fails -> host logs the backend error, exits 3, writes no session file
  -> CLI sees process exit before the file -> prints the log tail -> exit 3
```

**Tests and evidence**

| Test | Assertion |
| --- | --- |
| `HeadlessFrameSinkTest.HostVisibleMappingMatchesRequestedSize` | Mapping width, height, and stride match. |
| `HeadlessHostTest.SettingsDirRedirectsDefaultQSettings` | A value written in the host appears in the INI file under the directory. |
| `HeadlessHostTest.ProjectLoadFailureExitsWithLoadCode` | Exit code 4 and no session file. |
| `ApplicationModuleHostTest.IsIdleIsFalseWhileImportRuns` | `IsIdle()` false during import, true after. |
| `AutomationHeadlessSessionTest.StartPingStopRemovesSessionFile` | Start, ping, stop; the file is gone and the process exited with 0. |
| `AutomationHeadlessSessionTest.EditorOpenReachesInteractiveWithHeadlessSink` | After opening one CI RAW file through the session service, the state is `Interactive`. |

The last two tests spawn the real `alcedo_main --headless`. They get its path through a compile
definition (`$<TARGET_FILE:alcedo_main>`) and depend on the target.

**Build and run commands**

```powershell
cmd /c scripts\msvc_env.cmd --build --preset win_debug --target alcedo_main alcedo_cli AutomationHeadlessSessionTest HeadlessHostTest --parallel 4
$env:PATH = "D:\Projects\pu-erh_lab\build\debug\vcpkg_installed\x64-windows\debug\bin;" + $env:PATH
ctest --test-dir build/debug -R "HeadlessFrameSinkTest|HeadlessHostTest|AutomationHeadlessSessionTest|ApplicationModuleHostTest.IsIdle" -j 1 --output-on-failure
```

macOS: run the same tests in `build/macos-debug` configured with `-DALCEDO_BUILD_TESTS=ON`.

**Exit criteria**

- [ ] The headless host reaches `Interactive` on CUDA (Windows) and Metal (macOS). Record
      both, or record the unavailable platform.
- [ ] Shutdown persists the project. A second start sees the same project state.
- [ ] The GUI start path behaves as before (manual start check recorded).

**Expected diff:** 1000–1500 lines.

**Completion record:** see section 13.

### Phase AU4 — Project launch and close in C++, project commands

**Objective and deliverables**

- `ProjectLaunchCoordinator` owns launch sequencing and rollback now in
  `ProjectLaunchController.qml:133-212`.
- `ApplicationCloseCoordinator` owns the close decision and the editor finalize state machine
  now in `Main.qml:284-436`.
- `ProjectModule::CloseProject(bool persist)` exists.
- Commands `project.create`, `project.open`, `project.save`, `project.close`.

**Inputs and prerequisites**

- AU3.

**Modules, files, and APIs**

- Proposed: `project_launch_coordinator.{hpp,cpp}`, `application_close_coordinator.{hpp,cpp}`
  in the album backend. Both are QObjects owned by `ApplicationModuleHost` and exposed as
  `Q_PROPERTY`.
- `ProjectLaunchController.qml`, `Main.qml`: call the coordinators. Keep only presentation
  (dialog visibility, timers that only schedule a repaint).
- `ProjectModule`: `CloseProject`.
- Proposed: `src/ui/alcedo_main/automation/commands/project_commands.cpp`.

**Data rules and invariants**

- The welcome-dialog visibility rule stays in QML. It reads the coordinator state.
- `editorCloseNeedsConfirm` becomes `ApplicationCloseCoordinator::CloseNeedsConfirmation()`.
  Automation mode skips the confirmation as it does now.
- The close state machine states are the states that `pollEditorCloseSave` handles today:
  waiting on `Saving`, `Switching`, or in-flight work; abort on recovery, `RetainedImageFailure`,
  or `Failed`; finish on `NoImage` or `ShuttingDown`. Replace polling with the session change
  notifier.
- `project.open` while a project is open closes the current project first with `persist: true`.

**Implementation steps**

1. Write `ApplicationCloseCoordinator` with the states above. Drive it from
   `EditorSessionService::SetChangeNotifier` through `EditorSessionController` signals.
2. Change `Main.qml` close handling to call it.
3. Write `ProjectLaunchCoordinator` with launch, success, and rollback.
4. Change `ProjectLaunchController.qml` to call it.
5. Add `ProjectModule::CloseProject`. Reuse the finalize, sync, and persist steps that
   `ShutdownModules` runs for a project, without shutting down modules.
6. Write the project commands.

**Primary success call chain**

```text
project.open P2 (P1 open) -> ApplicationCloseCoordinator.Close(persist) -> editor Finalize
  -> NoImage -> ProjectModule::CloseProject -> ProjectLaunchCoordinator.Launch(P2)
  -> load finished -> respond project summary
```

**Primary failure and restore call chain**

```text
editor save fails (RetainedImageFailure) -> close aborts -> respond -32007 with the save message
  -> P1 stays open, editor keeps the retained image
```

**Tests and evidence**

| Test | Assertion |
| --- | --- |
| `ApplicationCloseCoordinatorTest.CloseWaitsForSaveAndFinishesOnNoImage` | Finish emitted after `NoImage`. |
| `ApplicationCloseCoordinatorTest.SaveFailureAbortsClose` | Abort emitted with the message; project still open. |
| `ProjectLaunchCoordinatorTest.FailedLaunchRestoresPreviousState` | The previous project stays entered. |
| `ProjectModuleTest.CloseProjectPersistsAndReopenShowsSameItems` | Reopen lists the same element ids. |
| `AutomationProjectCommandsTest.OpenSecondProjectClosesFirstWithPersist` | Session state shows P2, P1 file has the last edit. |

**Build and run commands**

```powershell
cmd /c scripts\msvc_env.cmd --build --preset win_debug --target AlbumBackendLib AutomationProjectCommandsTest ApplicationCloseCoordinatorTest ProjectLaunchCoordinatorTest --parallel 4
ctest --test-dir build/debug -R "ApplicationCloseCoordinatorTest|ProjectLaunchCoordinatorTest|AutomationProjectCommandsTest|ProjectModuleTest.CloseProject" -j 1 --output-on-failure
```

**Exit criteria**

- [ ] Tests pass.
- [ ] Manual GUI check: quit with an open edited image saves and exits; quit during save waits.

**Expected diff:** 900–1400 lines.

**Completion record:** see section 13.

### Phase AU5 — Import, library reads, thumbnails, tasks, and CI wiring

**Objective and deliverables**

- Commands `library.import`, `library.folders`, `library.list`, `library.thumbnail`,
  `tasks.list`, `tasks.wait`.
- The QML import filter reads the extension list from C++.
- CI builds and runs automation end-to-end tests with label `ci_automation_flow`.

**Inputs and prerequisites**

- AU4.

**Modules, files, and APIs**

- `ImportExportHandler`: expose `SupportedImportNameFilters()` built from `kRawExtensions`
  and the category rules at `supported_file_type.hpp:115-116`. `AppDialogs.qml:40-45` reads it.
- `library.import` calls `StartImportPaths` (`import_export.hpp:121`) for paths and the folder
  scan model for folders.
- `tasks.*` reads `BackgroundTaskController` and `ImportExportHandler` state and uses
  `ApplicationModuleHost::IsIdle()`.
- `library.thumbnail` requests the thumbnail through `ThumbnailService` and writes PNG with
  `QImage::save`.
- Proposed: `tests/automation/CMakeLists.txt` registers end-to-end tests with
  `gtest_discover_tests(... PROPERTIES LABELS "ci_automation_flow")` and a new category
  `ci_automation` in `AlcedoTestRegistration.cmake`.
- `CMakePresets.json`: add `alcedo_tests_ci_automation` to the build targets and
  `ci_automation_flow` to the test label filter of `macos_arm_metal_ci`.
- `.github/workflows/cpp-ci.yml`: upload `build/macos-ci/automation-failures/` as an artifact on
  failure.

**Data rules and invariants**

- Import tasks report through the existing `ImportStateChanged`. `tasks.wait` waits on the
  signal, not on a sleep loop.
- An import of zero supported files finishes with counts, not an error.
- `library.list` returns stable ordering by the requested sort and then by `element_id`.

**Implementation steps**

1. Add the C++ filter list. Change `AppDialogs.qml`.
2. Write the library commands.
3. Write the task commands and map task states to `task.updated` and `task.finished`.
4. Add the CI category, label, preset entries, and the artifact upload.
5. Write the first CI end-to-end test.

**Primary success call chain**

```text
library.import ci_rawfiles -> StartImportPaths -> task_id
tasks.wait task_id -> ImportStateChanged ... ImportRunning false -> final counts
library.list -> items
```

**Primary failure and restore call chain**

```text
import path does not exist -> -32602 with the path -> no task starts
```

**Tests and evidence**

| Test | Assertion |
| --- | --- |
| `ImportExportHandlerTest.NameFiltersCoverEveryRawExtension` | Every `kRawExtensions` entry appears. |
| `AutomationLibraryE2ETest.ImportCiRawFilesListsEveryImportedFile` | `library.list` count equals imported count; failed count is 0. |
| `AutomationLibraryE2ETest.ThumbnailCommandWritesPngWithRequestedLongEdge` | The PNG exists and its long edge equals the request. |
| `AutomationLibraryE2ETest.IdleWaitReturnsAfterPostImportPersist` | After `tasks.wait --idle`, reopening the project lists the same items. |

**Build and run commands**

```powershell
cmd /c scripts\msvc_env.cmd --build --preset win_debug --target AutomationLibraryE2ETest --parallel 4
ctest --test-dir build/debug -R "AutomationLibraryE2ETest|ImportExportHandlerTest.NameFilters" -j 1 --output-on-failure
```

CI: the next PR run shows the label in the ctest output.

**Exit criteria**

- [ ] Local tests pass.
- [ ] The CI run on the phase PR discovers and passes the `ci_automation_flow` tests. Record the
      run URL.

**Expected diff:** 1000–1500 lines.

**Completion record:** see section 13.

### Phase AU6 — Selection, rating, and delete in C++

**Objective and deliverables**

- `LibrarySelection` (QObject) replaces `SelectionState.qml` logic.
- `LibraryMutationOperations` owns delete targets, rating rules, and post-delete pruning from
  `ImageActionsController.qml:142-547`.
- Commands `library.selection.get`, `library.selection.set`, `library.rate`, `library.delete`.

**Inputs and prerequisites**

- AU5.

**Modules, files, and APIs**

- Proposed: `library_selection.{hpp,cpp}`, owned by `LibraryModule`. Key: element id.
- Proposed: `library_mutation_operations.{hpp,cpp}`. Calls `ImageController::DeleteImages`,
  `SetImageRating`, `StartSetImageRatings`.
- `SelectionState.qml`, `ImageActionsController.qml`: call the C++ owners.
- The export queue pruning after delete calls the AU12 owner when it exists. Until AU12, keep the
  QML export queue prune call.

**Data rules and invariants**

- Rating is an integer 0..5. Values outside the range return `-32602`. The command does not
  clamp. The QML path keeps its clamp because a slider can produce only valid values.
- Delete scope `project` when the current folder is 0, `album` otherwise. The command takes an
  explicit scope.
- After delete: prune the selection, clear the focused image when it was deleted, and close the
  editor image when it was deleted (`clearLastEditedImage` + open empty editor).

**Implementation steps**

1. Write `LibrarySelection` and move the QML functions.
2. Write `LibraryMutationOperations`.
3. Change the QML files.
4. Write the commands.

**Primary success call chain**

```text
library.delete [e1] project -> LibraryMutationOperations.Delete -> ImageController::DeleteImages
  -> prune selection, editor image -> respond deleted ids
```

**Primary failure and restore call chain**

```text
element id not in project -> -32602 listing the unknown ids -> nothing deleted
```

**Tests and evidence**

| Test | Assertion |
| --- | --- |
| `LibrarySelectionTest.SetSelectedIsNoOpWhenStateMatches` | No change signal. |
| `LibraryMutationOperationsTest.DeleteOpenEditorImageClosesEditorImage` | Editor state `NoImage` after delete. |
| `LibraryMutationOperationsTest.BatchRatingUsesBatchPath` | `StartSetImageRatings` called once for two targets. |
| `AutomationLibraryE2ETest.RatingPersistsAfterReopen` | Reopened project returns the rating. |
| `AutomationLibraryE2ETest.RatingOutsideRangeIsRejected` | -32602 with range 0..5. |

**Build and run commands**

```powershell
cmd /c scripts\msvc_env.cmd --build --preset win_debug --target LibrarySelectionTest LibraryMutationOperationsTest AutomationLibraryE2ETest --parallel 4
ctest --test-dir build/debug -R "LibrarySelectionTest|LibraryMutationOperationsTest|AutomationLibraryE2ETest" -j 1 --output-on-failure
```

**Exit criteria**

- [ ] Tests pass.
- [ ] Manual GUI check: select all, delete with confirmation, rate one and several images.

**Expected diff:** 900–1300 lines.

**Completion record:** see section 13.

### Phase AU7 — Parameter catalog: scalar fields

**Objective and deliverables**

- `EditorParameterCatalog` (Qt-free) with scalar entries: exposure, contrast, highlights,
  shadows, white, black, saturation, vibrance, clarity, sharpen, diffusion, film_grain,
  halation.
- UI-to-model and model-to-UI conversions for these fields, moved from
  `editor_adjustment_models.cpp:37-46` and the panel loaders.
- `EditorTonePanel.qml`, `EditorLookPanel.qml`, `EditorPostProcessPanel.qml` read min, max,
  default, and step from the catalog.
- Command `editor.catalog` for the scalar entries.

**Inputs and prerequisites**

- AU1. Independent of AU4 to AU6.

**Modules, files, and APIs**

- Proposed: `src/include/app/editor_parameter_catalog.hpp`, `src/app/editor_parameter_catalog.cpp`.
- A QML-visible adapter: a `Q_INVOKABLE QVariantMap catalogEntry(QString field)` on
  `EditorSessionController` or a small QML singleton in the album backend.

**Data rules and invariants**

- The table is the only source of UI ranges for these fields.
- Conversions:
  - saturation: model = `1 + v / 100`; UI = `(m - 1) * 100`.
  - diffusion, film_grain, halation: model strength = `v / 100`; UI = strength × 100.
  - sharpen: model `offset = v`.
  - Other scalars: identity.
- `ToModelJson` rejects a value outside `[ui_min, ui_max]`.
- Round trip: `ToUiValue(ToModelJson(v)) == v` within 1e-6 for every valid step value.

**Implementation steps**

1. Write the table from the values in the source audit (Tone :129-191, Look :145-157,
   PostProcess :113-161).
2. Write the conversions. Delete the duplicates in `editor_adjustment_models.cpp`.
3. Change the QML value models to read the catalog.
4. Write `editor.catalog`.

**Primary success call chain**

```text
QML slider init -> catalogEntry("saturation") -> min -100, max 100, default 0
```

**Primary failure and restore call chain**

```text
ToModelJson("saturation", 150) -> error "saturation must be in [-100, 100]" -> nothing written
```

**Tests and evidence**

| Test | Assertion |
| --- | --- |
| `EditorParameterCatalogTest.EveryScalarFieldIsAKnownFieldKey` | Each entry resolves through `ResolveEditorAdjustmentField`. |
| `EditorParameterCatalogTest.SaturationUiValueRoundTrips` | -100, 0, 37, 100 round-trip. |
| `EditorParameterCatalogTest.OutOfRangeValueIsRejectedWithRange` | Error text contains the range. |
| `EditorParameterCatalogTest.ModelJsonParsesThroughParameterWriteParser` | `ParseEditorParameterWrite` accepts every converted default. |
| `EditorAdjustmentModelTest` (existing) | Still passes after the duplicate removal. |

**Build and run commands**

```powershell
cmd /c scripts\msvc_env.cmd --build --preset win_debug --target EditorParameterCatalogTest EditorAdjustmentModelTest --parallel 4
ctest --test-dir build/debug -R "EditorParameterCatalogTest|EditorAdjustmentModelTest" --output-on-failure
```

**Exit criteria**

- [ ] Tests pass.
- [ ] Manual GUI check: every changed slider shows the same range and default as before.

**Expected diff:** 1100–1600 lines.

**Completion record:** see section 13.

### Phase AU8 — Parameter catalog: RAW decode, input profile, lens, crop

**Objective and deliverables**

- Structured catalog entries and UI-unit schemas for `raw_decode`, `input_profile`,
  `lens_calib`, `crop_rotate`.
- The builders now in `EditorRawDecodePanel.qml:96-242, 311-387` and
  `EditorGeometryPanel.qml:77-245` move to C++: defaults (`method`, `user_wb: 7600`,
  `backend: "alcedo"`), lens catalog default merge, crop aspect and rotation constraints, and
  `buildCropParams`.
- The QML panels call the C++ builders.

**Inputs and prerequisites**

- AU7.

**Modules, files, and APIs**

- `editor_parameter_catalog.{hpp,cpp}`: add the entries and builders.
- The crop constraint math keeps using `EditorGeometryMath` and the clamp helper that QML calls
  today. Move the call sequence, not the math.
- `EditorRawDecodePanel.qml`, `EditorGeometryPanel.qml`.

**Data rules and invariants**

- A structured UI value is a full object or a partial object. A partial object merges into the
  current model value read from `pipeline_document()`. The merge happens in the catalog and
  returns one write. It does not copy the document.
- Crop: x and y in [-1, 1]; width and height in [0.0001, 1]; angle in [-180, 180]; aspect width
  and height in [0.0001, 100]. The orientation flip rule from `EditorGeometryPanel.qml:77-91`
  applies.
- Unknown keys are rejected, as `ParseEditorParameterWrite` does.

**Implementation steps**

1. Write the schemas and builders for each field.
2. Replace the QML builders with calls.
3. Add the entries to `editor.catalog`.

**Primary success call chain**

```text
editor.set crop_rotate {"angle": 2.5} -> catalog merges with current crop -> model JSON
  -> ParseEditorParameterWrite ok
```

**Primary failure and restore call chain**

```text
crop_rotate {"width": 0} -> -32602 "width must be in [0.0001, 1]"
```

**Tests and evidence**

| Test | Assertion |
| --- | --- |
| `EditorParameterCatalogTest.RawDecodeDefaultsMatchPreviousPanelDefaults` | Method, user_wb, backend values. |
| `EditorParameterCatalogTest.PartialCropMergesWithCurrentValue` | Only the angle changes. |
| `EditorParameterCatalogTest.PortraitAspectFlipFollowsOrientation` | Aspect swaps for a portrait source. |
| `EditorParameterCatalogTest.LensDefaultsMergeMakerAndModel` | Maker and model come from the catalog entry. |
| `EditorGeometryMathTest` (existing) | Still passes. |

**Build and run commands**

```powershell
cmd /c scripts\msvc_env.cmd --build --preset win_debug --target EditorParameterCatalogTest EditorGeometryMathTest --parallel 4
ctest --test-dir build/debug -R "EditorParameterCatalogTest|EditorGeometryMathTest" --output-on-failure
```

**Exit criteria**

- [ ] Tests pass.
- [ ] Manual GUI check: RAW panel and Geometry panel edits produce the same history entries as
      before.

**Expected diff:** 1300–1700 lines.

**Completion record:** see section 13.

### Phase AU9 — Parameter catalog: display transform and color fields

**Objective and deliverables**

- Catalog entries and builders for `odt`, `color_temp`, `hls`, `color_wheel`,
  `grade_white_balance`, `curve`, `lut`.
- The ODT option tables and EOTF validity rules from `EditorDisplayTransformPanel.qml:50-380`
  move to C++.
- Color temperature slider mapping (`color_temp.hpp:9-16`, pivot 6000 K), HLS scale
  (`hls.hpp:19-22`), and color wheel scale (`color_wheel.hpp:15-18`) are exposed through the
  catalog in UI units.

**Inputs and prerequisites**

- AU8.

**Modules, files, and APIs**

- `editor_parameter_catalog.{hpp,cpp}`.
- `EditorDisplayTransformPanel.qml`, `EditorLookPanel.qml`, `EditorWhiteBalanceSliders.qml`.
- The existing range constants in `editor_support/modules/*.hpp` stay the definition. The catalog
  reads them.

**Data rules and invariants**

- UI units for color_temp are Kelvin (2000..15000) and tint (-150..150), not the 0..4096 slider
  position.
- UI units for HLS are the panel values (-100..100, hue shift ±30°). UI units for the color wheel
  are the panel master values (-800..800) and disc positions.
- An EOTF that is not valid for the selected encoding space returns `-32602` with the valid
  list.
- `curve` and `lut` use their model JSON shape. Their UI shape equals the model shape, and the
  catalog documents that.

**Implementation steps**

1. Move the ODT tables and the EOTF rule.
2. Write the color field entries and conversions.
3. Change the QML panels.
4. Add the entries to `editor.catalog`.

**Primary success call chain**

```text
editor.set color_temp {"kelvin": 5200, "tint": 8} -> catalog -> model JSON -> parser ok
```

**Primary failure and restore call chain**

```text
editor.set odt {"encoding_space": "rec2020", "eotf": "<invalid>"} -> -32602 with valid EOTFs
```

**Tests and evidence**

| Test | Assertion |
| --- | --- |
| `EditorParameterCatalogTest.InvalidEotfForSpaceIsRejectedWithValidList` | Error lists the valid EOTFs. |
| `EditorParameterCatalogTest.ColorTempKelvinRoundTripsThroughSliderPivot` | 2000, 6000, 15000 round-trip. |
| `EditorParameterCatalogTest.HlsUiScaleMatchesModuleConstant` | Model value equals UI × 1000 scale rule. |
| `EditorParameterCatalogTest.EveryCatalogFieldCoversEveryFieldKey` | Each `kFieldKeys` field has an entry (aliases map to one entry). |

**Build and run commands**

```powershell
cmd /c scripts\msvc_env.cmd --build --preset win_debug --target EditorParameterCatalogTest --parallel 4
ctest --test-dir build/debug -R "EditorParameterCatalogTest" --output-on-failure
```

**Exit criteria**

- [ ] Tests pass.
- [ ] Manual GUI check: Display transform and Look panels behave as before.

**Expected diff:** 1200–1700 lines.

**Completion record:** see section 13.

### Phase AU10 — Editor commands and `render.preview`

**Objective and deliverables**

- Commands `editor.open`, `editor.close`, `editor.get`, `editor.set`, `editor.batch_set`,
  `editor.undo`, `editor.redo`, `editor.history`, `editor.actions`, `render.preview`.
- The Release boundary gets a command id so the consume results carry it.

**Inputs and prerequisites**

- AU3, AU7. AU8 and AU9 add more fields but are not required for this phase.

**Modules, files, and APIs**

- `EditorSessionService::EnqueuePendingInputBoundary`: route through `SubmitCommand` so the
  boundary gets a command id. The consume path emits results with that `operation_id`.
- Proposed: `editor_commands.cpp`, `render_commands.cpp`.
- `render.preview` builds Root and Current snapshots with the existing
  `BuildEditorComparisonInputs` (`editor_comparison_service.cpp:137-158`) and calls
  `IEditorImageRenderPort::ScheduleImages`. It converts `RenderedPipelineImage` pixels with the
  conversion that `comparison_presentation_image.cpp` uses, then writes PNG files.
- `region` sets `RenderRequest.view.visible_rect_in_edit_space`. `long_edge` sets
  `ResolutionRequest.max_edge` and must not pass `kQualityBaseMaxLongEdge` (4096).

**Data rules and invariants**

- `editor.set` sends the UI value through the catalog, then `EnqueueAdjustmentInput` with
  `settled = true`, then the Release boundary. The target resolution matches
  `EditorSessionController::submitWrite` (`editor_session_controller.cpp:1487-1510`). Extract that
  resolution into a function that both call.
- `editor.set` returns after a result with the boundary `operation_id` reports the commit and the
  presented frame for its `render_request_id` is ready.
- `editor.batch_set` applies changes in order. Each change is one commit. On the first failure
  it stops and reports `applied`.
- `render.preview` returns `busy` when the image render port runs another job (for example the
  comparison view). It does not wait for that job and does not cancel it.
- `render.preview` writes files only inside `out_dir`. The file names are
  `<file_stem>_current.png` and `<file_stem>_root.png`.

**Implementation steps**

1. Verify that the consume path for the Release boundary runs inside the command scope of the
   boundary. If it does not, stop and update this plan (section 12).
2. Route the boundary through `SubmitCommand`.
3. Extract the target resolution function.
4. Write the editor commands.
5. Write `render.preview`.

**Primary success call chain**

See 5.4.

**Primary failure and restore call chain**

```text
editor.set before Interactive -> owner rejects "Queued adjustment input requires an interactive
session" -> -32002 with the owner message -> history unchanged
```

**Tests and evidence**

| Test | Assertion |
| --- | --- |
| `EditorSessionServiceTest.ReleaseBoundaryResultsCarryBoundaryOperationId` | Every result after the boundary has its id. |
| `AutomationEditorE2ETest.SetExposureCreatesOneCommitAndReadsBackUiValue` | One new commit; `editor.get` returns the value. |
| `AutomationEditorE2ETest.UndoRestoresPreviousUiValue` | Value equals the value before set. |
| `AutomationEditorE2ETest.BatchStopsAtFirstInvalidValueAndReportsApplied` | `applied` equals 1 for an invalid second change. |
| `AutomationEditorE2ETest.PreviewWritesCurrentAndRootPngWithRequestedLongEdge` | Both files exist; long edge equals the request. |
| `AutomationEditorE2ETest.RegionPreviewHasRegionAspectRatio` | Width / height equals the region ratio within one pixel. |
| `AutomationEditorE2ETest.EditsPersistAfterSessionRestart` | After stop and start, `editor.get` returns the values. |

**Build and run commands**

```powershell
cmd /c scripts\msvc_env.cmd --build --preset win_debug --target AutomationEditorE2ETest EditorSessionServiceTest --parallel 4
ctest --test-dir build/debug -R "AutomationEditorE2ETest|EditorSessionServiceTest" -j 1 --output-on-failure
```

**Exit criteria**

- [ ] Tests pass on Windows CUDA locally and on macOS Metal in CI.
- [ ] The GUI slider path still creates the same commits (existing editor session tests pass).

**Expected diff:** 1300–1800 lines.

**Completion record:** see section 13.

### Phase AU11 — Version commands and commit attribution

**Objective and deliverables**

- Commands `versions.list`, `versions.create`, `versions.branch`, `versions.checkout`,
  `versions.rename`, `versions.remove`.
- `NormalizeVersionName` and `DefaultVersionName` move the rules from
  `EditorVersionsPanel.qml:281-340`.
- Commit attribution as specified in 2.10.

**Inputs and prerequisites**

- AU10.

**Modules, files, and APIs**

- Proposed: `version_name_rules.{hpp,cpp}`.
- Proposed: `edit_commit_attribution.hpp`.
- `MiniGitWorkingHistory::PrepareAppendEdit`: accept optional attribution and write it into the
  journal record.
- The journal record reader and writer: format version 7.
- `database.hpp`: `CREATE TABLE IF NOT EXISTS EditCommitAttribution`. It runs for new and
  existing projects at open.
- `CommitGraphStore::Materialize`: write attribution rows in its transaction.
- Mini-Git garbage collection: delete attribution rows of removed commits.
- `EditorHistoryCommit`, `CommitRowFromEdit`, `EditorHistoryModel`: author and note roles.
- The History panel delegate: agent badge and note.

**Data rules and invariants**

- Commit hashes do not change. `EditCommit::FromJSON` is unchanged.
- Version names: trim; an empty name is rejected; a rename to the same name is a no-op success.
- A journal record version 6 reads as author `user`.
- Attribution exists only for commits created through a Release boundary with attribution.

**Implementation steps**

1. Move the version name rules and call them from QML and the commands.
2. Write the version commands with the terminal history operation event as completion.
3. Add the attribution type and the boundary parameter.
4. Add journal format 7 with a reader for 6 and 7.
5. Add the table, materialization, and garbage collection.
6. Add the model roles and the QML badge.

**Primary success call chain**

```text
editor.set (agent) -> boundary attribution{agent, note} -> journal v7 record
  -> save checkpoint -> Materialize: EditCommit row + EditCommitAttribution row (one transaction)
  -> editor.history shows author agent and the note
```

**Primary failure and restore call chain**

```text
process killed after journal write, before materialize -> restart -> journal recovery reads v7
  -> Materialize writes both rows -> author preserved
```

**Tests and evidence**

| Test | Assertion |
| --- | --- |
| `VersionNameRulesTest.TrimmedEmptyNameIsRejected` | Rejected with the message. |
| `MiniGitJournalTest.Version6RecordReadsAsUserAuthor` | Author `user`. |
| `MiniGitJournalTest.Version7RecordRoundTripsAttribution` | Author and note match. |
| `CommitGraphStoreTest.MaterializeWritesAttributionInSameTransaction` | Failure injection after the commit insert leaves neither row. |
| `CommitGraphStoreTest.GarbageCollectionRemovesOrphanAttribution` | No row for a removed commit. |
| `EditCommitTest.HashIsUnchangedByAttribution` | Hash equals the hash without attribution. |
| `AutomationVersionsE2ETest.BranchCheckoutAndRenameAppearInVersionList` | List matches. |
| `AutomationVersionsE2ETest.AgentCommitShowsAuthorAfterReopen` | `editor.history` shows `agent` after restart. |
| Existing compatibility tests with `alcedo_studio/tests/resources/compat/*.alcd` | Old projects open and their commits read as `user`. |

**Build and run commands**

```powershell
cmd /c scripts\msvc_env.cmd --build --preset win_debug --target AutomationVersionsE2ETest VersionNameRulesTest MiniGitJournalTest CommitGraphStoreTest EditCommitTest --parallel 4
ctest --test-dir build/debug -R "AutomationVersionsE2ETest|VersionNameRulesTest|MiniGitJournalTest|CommitGraphStoreTest|EditCommitTest" -j 1 --output-on-failure
```

Confirm the exact existing test target names from `tests/*/CMakeLists.txt` before the run.

**Exit criteria**

- [ ] Tests pass.
- [ ] An old project opens, edits, and saves without a format error.
- [ ] Manual GUI check: an agent commit shows the badge and the note.

**Expected diff:** 1300–1800 lines.

**Completion record:** see section 13.

### Phase AU12 — Export in C++ and export commands

**Objective and deliverables**

- `ExportQueue` (QObject) replaces `ExportQueueState.qml` and the prune calls in
  `ShellSignals.qml:173-190`.
- `ExportRecipeBuilder` replaces the defaults, validation, bit-depth rules, output folder rule,
  and argument assembly in `ExportInspectorPanel.qml:27-406`.
- `ImportExportHandler` gets an `ExportFinished` signal.
- Commands `export.options`, `export.run`, `export.status`.

**Inputs and prerequisites**

- AU5.

**Modules, files, and APIs**

- Proposed: `export_queue.{hpp,cpp}`, `export_recipe_builder.{hpp,cpp}`.
- `import_export.{hpp,cpp}`: `ExportFinished`, emitted once when `exportInFlight` becomes false.
  Read the file bytes first. This file had mixed line endings in an earlier change.
- `ExportInspectorPanel.qml`, `ExportQueueState.qml`, `ShellSignals.qml`.

**Data rules and invariants**

- Defaults: JPEG, 8 bits, quality 95, PNG level 5, TIFF compression NONE, metadata on, ICC on,
  UltraHDR dither on, subfolder "Processed", resize original, long edge 2048, print 210×297 mm at
  300 dpi.
- Limits: long edge 256..16384; pixel bounds 1..100000; print size > 0 and ≤ 100000; dpi
  1..10000; quality 1..100; PNG level 0..9.
- Bit depths: JPEG 8; PNG 8, 16; TIFF 8, 16, 32; EXR 16, 32.
- The file name pattern uses the existing pattern validator (`ExportNamingEditor.qml:184`). Move
  it to C++ in this phase.
- The queue de-duplicates by element id and keeps the last entry.

**Implementation steps**

1. Write `ExportRecipeBuilder` with the rules above.
2. Move the name pattern validator.
3. Write `ExportQueue`.
4. Add `ExportFinished`.
5. Change the QML files.
6. Write the commands.

**Primary success call chain**

```text
export.run [e1,e2] out_dir JPEG -> ExportRecipeBuilder.Validate -> StartExportWithRecipeOptionsForTargets
  -> task_id -> tasks.wait -> ExportFinished -> counts
```

**Primary failure and restore call chain**

```text
long_edge 100 -> -32602 "long_edge must be in [256, 16384]" -> no export starts
one target fails to write -> ExportFinished with failed = 1 -> export.status lists the error
```

**Tests and evidence**

| Test | Assertion |
| --- | --- |
| `ExportRecipeBuilderTest.DefaultsMatchPreviousPanelDefaults` | All default values. |
| `ExportRecipeBuilderTest.BitDepthNotAllowedForFormatIsRejected` | JPEG 16 rejected. |
| `ExportQueueTest.AddTargetsKeepsLastEntryPerElement` | One row per element. |
| `ExportNamePatternTest.InvalidPatternIsRejected` | Validator result. |
| `AutomationExportE2ETest.ExportJpegWritesFilesWithRequestedLongEdge` | Files exist; long edge matches. |
| `AutomationExportE2ETest.Export16BitTiffWritesSixteenBitFile` | File bit depth 16 (read with OIIO). |

**Build and run commands**

```powershell
cmd /c scripts\msvc_env.cmd --build --preset win_debug --target ExportRecipeBuilderTest ExportQueueTest ExportNamePatternTest AutomationExportE2ETest --parallel 4
ctest --test-dir build/debug -R "ExportRecipeBuilderTest|ExportQueueTest|ExportNamePatternTest|AutomationExportE2ETest" -j 1 --output-on-failure
```

**Exit criteria**

- [ ] Tests pass.
- [ ] Manual GUI check: export panel defaults, validation messages, and queue behave as before.

**Expected diff:** 1100–1600 lines.

**Completion record:** see section 13.

### Phase AU13 — Search in C++ and search commands

**Objective and deliverables**

- `SearchSession` (QObject) replaces routing, request matching, and paging in
  `GlobalSearchDialog.qml:187-555`.
- Command `search.query`.

**Inputs and prerequisites**

- AU5.

**Modules, files, and APIs**

- Proposed: `search_session.{hpp,cpp}`, owned by `SearchController`.
- `GlobalSearchDialog.qml`: keep presentation and debounce timers. Call `SearchSession` for
  routing and paging.

**Data rules and invariants**

- Page size 24. Window capacity 48.
- Routing: empty query → recommendations; `ClassifyQuery` result `semantic` with natural
  language search on → semantic path; otherwise fuzzy.
- A response is applied only when its request id matches the active request. This uses the
  existing request id from `RequestSearch` and replaces the QML generation counter. It adds no new
  mechanism.
- `search.query` with `mode: auto` returns `semantic_unavailable: true` when the semantic path
  is not available. It does not switch to fuzzy unless the client sends `mode: fuzzy`.

**Implementation steps**

1. Write `SearchSession` with routing, response matching, and the sliding window.
2. Change the QML file.
3. Write `search.query`.

**Primary success call chain**

```text
search.query "beach" fuzzy -> SearchSession.Submit -> RequestSubmitSearch -> SearchResponseReady
  (matching id) -> respond matches, total, has_more
```

**Primary failure and restore call chain**

```text
query longer than the limit -> response tooLong -> respond {"too_long": true, "matches": []}
```

**Tests and evidence**

| Test | Assertion |
| --- | --- |
| `SearchSessionTest.ResponseWithOldRequestIdIsIgnored` | Window unchanged. |
| `SearchSessionTest.AppendBeyondCapacityDropsLeadingRows` | Offset and row count match. |
| `AutomationSearchE2ETest.FuzzySearchFindsImportedFileName` | The imported file is in matches. |

**Build and run commands**

```powershell
cmd /c scripts\msvc_env.cmd --build --preset win_debug --target SearchSessionTest AutomationSearchE2ETest --parallel 4
ctest --test-dir build/debug -R "SearchSessionTest|AutomationSearchE2ETest" -j 1 --output-on-failure
```

**Exit criteria**

- [ ] Tests pass.
- [ ] Manual GUI check: global search typing, paging, and semantic routing behave as before.

**Expected diff:** 900–1300 lines.

**Completion record:** see section 13.

### Phase AU14 — GUI automation server and agent control

**Objective and deliverables**

- The GUI `alcedo_main` starts `AutomationServer` and writes a session file with
  `host_mode: "gui"`.
- `AgentControlOwner` and the commands `control.acquire`, `control.release`, `control.get`.
- A new interaction capability that blocks user-facing state changes while the agent holds
  control.
- The agent control task in the existing background task island and task dialog, as specified
  in 2.8. No banner and no new notification channel.
- **Return control** as a row action on the canceled agent task.
- Notifications `control.changed`.

The detailed agent session logic (status view content, messages, richer handoff) is not part
of this phase. A separate plan designs it (1.3). Keep the agent task fields in 2.8 stable so that
design can read them.

**Inputs and prerequisites**

- AU6, AU10, AU11, AU12. The user confirms the server default (1.5).

**Modules, files, and APIs**

- Proposed: `agent_control_owner.{hpp,cpp}`, owned by `ApplicationModuleHost`, exposed as
  `Q_PROPERTY agentControl`. It receives `BackgroundTaskController*` through its constructor.
- `background_task_controller.hpp`: `InteractionCapability::UserStateChange` and
  `BackgroundTaskKind::AgentControl`, plus `"agentControl"` in `BackgroundTaskController::KindToString`.
- `interaction_policy_controller.{hpp,cpp}`: `canChangeStateAsUser`, `changeStateAsUserReason`,
  `EvaluateChangeStateAsUser()`.
- `EditorSessionController`: its QML-facing write, undo, redo, version, and navigation
  invokables reject while `canChangeStateAsUser` is false. `SyncBackgroundActionRestrictions`
  does **not** map this capability into `EditorBackgroundActionRestrictions`. Otherwise the
  session service would also reject the agent's own commands.
- Other QML-facing invokables that start user state changes (import, delete, rating, export,
  project open, create, close) check the same capability in their controller adapter. The shared
  C++ operations from AU4 to AU12 do not check it.
- `AutomationCommandRegistry`: for `changes_state` commands in the GUI host, call
  `AgentControlOwner::BeginCommand(summary)` before the handler and `EndCommand()` after the
  reply. These update the agent task detail.
- `main.cpp`: start the server after the host exists. Respect the setting
  `automation/serverEnabled`.
- QML (load the `alcedo-qml-ui` skill first):
  - `BackgroundTaskBar.qml`: `kindLabel("agentControl")` returns "Agent".
  - `BackgroundTasksDialog.qml`: the **Return control** row action on a canceled agent task,
    visible while `agentControl.revokedByUser` is true.
  - `enabled` bindings on the editor panels, viewport edit tools, Versions actions, import,
    delete, rating, and export controls.
- Translations: add the new strings to the `.ts` files by hand.

**Data rules and invariants**

- Control state: `held`, `holder_name`, `revoked_by_user`. Only `AgentControlOwner` changes it.
- One agent task exists while control is held. `AgentControlOwner` keeps its task id and is the
  only caller of `UpdateTask` and `FinishTask` for it.
- `control.acquire` waits for the pending input Release or Cancel when the editor has unsealed
  input. It uses the existing pending-input state. It does not add a new signal source.
- The task cancel callback (**Take control**) finishes the agent task as Canceled, sets
  `revoked_by_user`, and emits `control.changed` once.
- `control.release` finishes the agent task as Succeeded and emits `control.changed` once.
- **Return control** clears `revoked_by_user` and emits `control.changed`. It does not acquire
  control for the agent.
- A state-changing command checks control when it starts and between batch steps. A command
  already submitted to an owner runs to completion.
- Shutdown cancels the agent task like other tasks.

**Implementation steps**

1. Add the capability, task kind, and policy properties.
2. Write `AgentControlOwner` with task registration, detail updates, cancel, release, and
   Return control.
3. Add the control check and the `BeginCommand` and `EndCommand` calls to the registry for
   `changes_state` commands in GUI host mode.
4. Add the user-facing checks to the controller adapters.
5. Start the server in GUI mode and write the session file.
6. Add the kind label, the Return control row action, and the `enabled` bindings.
7. Add the translations.

**Primary success call chain**

```text
control.acquire claude-code -> no unsealed input
  -> BackgroundTaskController::RegisterTask(AgentControl, lock UserStateChange, cancel cb)
  -> PolicyChanged -> QML panels disabled; island unfolds after the grace delay -> respond held
editor.set saturation 20 -> BeginCommand -> UpdateTask(detail "editor.set saturation 20")
  -> ... -> reply -> EndCommand -> UpdateTask(detail waiting text)
```

**Primary failure and restore call chain**

See 5.5. The user cancels the agent task in the island or the task dialog. The task finishes as
Canceled, the lock is released, and the user edits at once.

**Tests and evidence**

| Test | Assertion |
| --- | --- |
| `AgentControlOwnerTest.AcquireWaitsForPendingInputRelease` | Held only after the Release boundary. |
| `AgentControlOwnerTest.AcquireRegistersRunningAgentTaskWithUserStateChangeLock` | One running task of kind `AgentControl` with the global lock. |
| `AgentControlOwnerTest.CommandSummaryUpdatesAgentTaskDetail` | Task detail equals the summary during the command and the waiting text after it. |
| `AgentControlOwnerTest.CancelingAgentTaskRevokesControl` | Task state Canceled, `revoked_by_user` true, next state-changing command returns -32004. |
| `AgentControlOwnerTest.ReacquireIsRefusedUntilReturnControl` | Acquire returns -32004, then succeeds after Return control. |
| `AgentControlOwnerTest.ReleaseFinishesAgentTaskAsSucceeded` | Task state Succeeded, lock released. |
| `InteractionPolicyControllerTest.UserStateChangeLockDoesNotRestrictSessionAdmission` | `EditorBackgroundActionRestrictions` stays all false. |
| `EditorSessionControllerTest.UserWriteIsRejectedWhileAgentHoldsControl` | `submitPatch` returns false; no commit. |
| `AutomationGuiSessionTest.AgentEditAppearsInControllerProjection` | After `editor.set` through the GUI host socket, the controller panel projection shows the value. This test constructs the host and server without loading QML. |
| `AutomationGuiSessionTest.StateChangeWithoutControlIsRejected` | -32003. |

**Build and run commands**

```powershell
cmd /c scripts\msvc_env.cmd --build --preset win_debug --target alcedo_main AgentControlOwnerTest InteractionPolicyControllerTest EditorSessionControllerTest AutomationGuiSessionTest --parallel 4
ctest --test-dir build/debug -R "AgentControlOwnerTest|InteractionPolicyControllerTest|EditorSessionControllerTest|AutomationGuiSessionTest" -j 1 --output-on-failure
```

**Exit criteria**

- [ ] Tests pass.
- [ ] Manual GUI check, recorded by the user: the island unfolds while the agent edits; a click
      shows the agent task with the running command; cancel takes control and blocks the agent;
      Return control lets the agent acquire control again.

**Expected diff:** 1100–1600 lines.

**Completion record:** see section 13.

### Phase AU15 — MCP server, operation skill, and packaging

**Objective and deliverables**

- `alcedo-cli mcp`: a stdio MCP server. The tool list comes from `session.describe`.
- `render.preview` and `library.thumbnail` results include MCP image content (PNG, long edge at
  most 1024) and the file paths.
- `.agents/skills/alcedo-automation/SKILL.md`: the operation skill.
- `alcedo-cli` install rules on Windows and macOS.

**Inputs and prerequisites**

- AU14. The user confirms the macOS location (1.5).

**Modules, files, and APIs**

- Proposed: `src/ui/alcedo_cli/mcp_server.{hpp,cpp}`. MCP methods: `initialize`,
  `tools/list`, `tools/call`, `notifications/message` for forwarded host notifications.
- Tool names replace `.` with `_` (`editor_set`). Tool input schemas equal the command schemas.
- MCP tools also include `session_start` and `session_stop`, which run the AU3 CLI logic.
- Root `CMakeLists.txt`: install `alcedo_cli` to `bin` on Windows and to
  `<Bundle>.app/Contents/Helpers` on macOS with `INSTALL_RPATH @loader_path/../Frameworks`.
  The bundle signing step covers it.
- `scripts/verify_windows_install_tree.ps1`: require `alcedo-cli.exe`.

**Data rules and invariants**

- The MCP server holds one connection to the selected session. It reconnects once per tool call
  when the connection is closed.
- A JSON-RPC error becomes an MCP tool result with `isError: true` and the code, name, and
  reason in the text.
- The operation skill documents: session start and selection, control rules, completion
  behavior, UI units and `editor.catalog`, the preview workflow (`render.preview` full image,
  then region, then `compare: root`), error codes, and history attribution. It does not contain
  taste rules.

**Implementation steps**

1. Write the MCP server on top of the AU2 client.
2. Write the image content conversion.
3. Write the skill.
4. Add install rules and the verify script entry.

**Primary success call chain**

```text
MCP tools/call editor_set {"field":"exposure","value":0.3} -> JSON-RPC editor.set
  -> result -> MCP content text
```

**Primary failure and restore call chain**

```text
session not running -> tools/call returns isError with exit-code-2 text and the session list
```

**Tests and evidence**

| Test | Assertion |
| --- | --- |
| `AlcedoMcpServerTest.ToolsListMatchesSessionDescribe` | Same names and schemas. |
| `AlcedoMcpServerTest.JsonRpcErrorBecomesIsErrorResult` | `isError: true` with the code. |
| `AlcedoMcpServerTest.PreviewResultContainsImageContent` | One image item with `image/png`. |
| Windows install tree check | `verify_windows_install_tree.ps1` passes on the release build. |
| Manual check | Claude Code with the MCP server edits one image in a GUI session. Record by the user. |

**Build and run commands**

```powershell
cmd /c scripts\msvc_env.cmd --build --preset win_debug --target alcedo_cli AlcedoMcpServerTest --parallel 4
ctest --test-dir build/debug -R "AlcedoMcpServerTest" --output-on-failure
cmd /c scripts\msvc_env.cmd --build --preset win_release --parallel 4
powershell -ExecutionPolicy Bypass -File scripts/package_windows.ps1 -BuildDir build/release -Preset win_release
```

**Exit criteria**

- [ ] Tests pass.
- [ ] The release install tree contains `alcedo-cli` on Windows. The macOS bundle check is
      recorded or reported as unavailable.

**Expected diff:** 1100–1600 lines.

**Completion record:** see section 13.

### Phase AU16 — Operation-sequence robustness and recovery tests

**Objective and deliverables**

- A seeded random sequence test drives the headless host with user-like command sequences and
  checks invariants after each step.
- Crash recovery tests kill the host process during editing and verify journal recovery.
- CI runs short sequences on every PR and longer sequences on the scheduled runs.

**Inputs and prerequisites**

- AU11, AU12, AU13.

**Modules, files, and APIs**

- Proposed: `tests/automation/automation_operation_sequence_test.cpp`,
  `tests/automation/automation_crash_recovery_test.cpp`, and a client-side expected state
  model (`tests/automation/expected_session_state.{hpp,cpp}`).
- Environment variables: `ALCEDO_AUTOMATION_SEQUENCE_STEPS` (default 150),
  `ALCEDO_AUTOMATION_SEQUENCE_RNG_SEEDS` (default three fixed values; scheduled CI uses random
  values and prints them).
- `.github/workflows/cpp-ci.yml`: set longer values on `schedule` events.

**Data rules and invariants**

The operation set: open image, switch image, set scalar field, set structured field, batch set,
undo, redo, create Version, branch, checkout, rename, render preview, export one image, rate,
search, close editor, save project, restart session.

After every step:

1. `session.ping` answers within 5 s (no deadlock).
2. The host process is alive (no crash).
3. `state.get` reports a stable editor state (`Interactive` or `NoImage`) within the timeout.
   `Saving` or `Switching` past the timeout is a failure.
4. `editor.get` equals the expected model for the active Version head.
5. `versions.list` equals the expected Version list.

After a restart: the same checks with the persisted expected model.

After a hard kill during editing: restart, then `editor.get` equals the last settled state that
the journal had accepted.

On failure, the test writes the RNG seed, the full command log, the last responses, and the host
log tail to `automation-failures/<test>/<seed>/`.

**Implementation steps**

1. Write the expected state model (values per Version head, undo stack per Version).
2. Write the weighted sequence generator with a fixed RNG seed per run.
3. Write the invariant checks and the failure bundle writer.
4. Write the crash recovery tests with `QProcess::kill`.
5. Add the CI environment values.

**Primary success call chain**

```text
for step in 1..N: pick operation (RNG) -> send command -> update expected model -> check
invariants
```

**Primary failure and restore call chain**

```text
invariant 4 fails at step 87 -> write failure bundle with RNG seed -> test fails with the bundle
path -> replay with ALCEDO_AUTOMATION_SEQUENCE_RNG_SEEDS=<seed> reproduces the sequence
```

**Tests and evidence**

| Test | Assertion |
| --- | --- |
| `AutomationOperationSequenceTest.RandomUserSequenceKeepsSessionConsistent` | All invariants hold for each seed. |
| `AutomationOperationSequenceTest.SameRngSeedReplaysSameCommandLog` | Two runs with one seed produce the same command log. |
| `AutomationCrashRecoveryTest.KillDuringEditRestoresLastAcceptedEdit` | Values equal the last accepted settled state. |
| `AutomationCrashRecoveryTest.KillDuringSaveKeepsProjectOpenable` | The project opens; values equal either the old or the new state. |
| `AutomationCrashRecoveryTest.AgentAuthorSurvivesKillBeforeMaterialize` | `editor.history` shows `agent`. |

**Build and run commands**

```powershell
cmd /c scripts\msvc_env.cmd --build --preset win_debug --target AutomationOperationSequenceTest AutomationCrashRecoveryTest --parallel 4
ctest --test-dir build/debug -R "AutomationOperationSequenceTest|AutomationCrashRecoveryTest" -j 1 --output-on-failure
```

**Exit criteria**

- [ ] Tests pass locally and in CI.
- [ ] One scheduled CI run with random RNG seeds passes. Record the run URL and the seeds.
- [ ] Every defect found is fixed in its owner with a named regression test, or recorded as an
      open defect with its failure bundle.

**Expected diff:** 900–1400 lines.

**Completion record:** see section 13.

## 10. Cross-phase acceptance matrix

| Behavior | Expected result | Phase |
| --- | --- | --- |
| Headless start and stop | Session file appears and disappears; project persists. | AU3 |
| Unknown command or parameter | JSON-RPC error; connection stays open. | AU1 |
| Out-of-range UI value | -32602 with the range; history unchanged. | AU7–AU9 |
| Edit before `Interactive` | -32002 with the owner message. | AU10 |
| Set, undo, redo | Values follow the history head. | AU10 |
| Version branch and checkout | Values follow the active Version head. | AU11 |
| Reopen after edits | Values and authors are unchanged. | AU10, AU11 |
| Old project file | Opens; commits read as `user`. | AU11 |
| Hard kill during edit | Last accepted settled edit restored. | AU16 |
| Export | Files match format, bit depth, and long edge. | AU12 |
| Search | Imported file name found. | AU13 |
| Agent without control in GUI host | -32003. | AU14 |
| User takes control | Agent receives -32004; user edits work. | AU14 |
| Image render port busy | -32005; the other job continues. | AU10 |
| MCP error | `isError: true` with the code. | AU15 |
| Random user sequences | No deadlock, crash, or state mismatch. | AU16 |

## 11. Build and evidence rules

- Presets: `win_debug`, `win_release`, `macos_debug` (with `-DALCEDO_BUILD_TESTS=ON`),
  `macos_release`, and the existing CI preset `macos_arm_metal_ci`.
- Target groups: `alcedo_tests_app`, `alcedo_tests_ui`, and the new `alcedo_tests_ci_automation`.
- Backends: Windows CUDA locally; macOS Metal in CI. Record OpenCL as not run unless a run
  exists.
- Evidence location: `build/tmp/automation_<phase>/`. Remove it when the phase is complete.
- Record each test as passed, failed, skipped, or not run. A skipped test is not a pass. An
  unavailable platform is reported as unavailable.
- Manual GUI checks supplement automated tests. The user records them.

## 12. Risks and stop conditions

| Risk | Detection signal | Required response |
| --- | --- | --- |
| A host-side code path needs `QGuiApplication` | Crash or Qt warning about a missing GUI application in AU3 | Stop. Name the path. Move it out of the headless path or change the plan. Do not create a hidden window. |
| The Release boundary consume does not run in the boundary command scope | AU10 step 1 finds the consume on another command scope | Stop. Update AU10 with the real correlation point. Do not add a new id type without an interleaving. |
| The journal reader cannot accept two record versions cleanly | AU11 reader change breaks existing journal tests | Stop. Write a migration decision into AU11. |
| The `CREATE TABLE IF NOT EXISTS` step does not run for existing projects | The AU11 old-project test fails with a missing table | Stop. Add the table creation to the project open path and update the plan. |
| Metal rendering without a window fails in the CI runner | AU3 test fails on macOS CI | Stop. Record the error. Do not use a CPU path. |
| A GUI user state change is not covered by the new capability | AU14 manual check finds an enabled control | Add the check to that adapter and a test. |
| A phase passes 2000 lines | Diff count during implementation | Stop and split the phase in this plan. |
| A fallback seems necessary | Any phase | Stop and ask the user. |

## 13. Completion record template

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
