//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QString>
#include <QStringList>
#include <optional>

#include "edit/pipeline/pipeline_accelerator.hpp"
#include "ui/editor_rhi/editor_backend.hpp"

namespace alcedo::automation {

/// Exit codes of `alcedo_main --headless`.
enum class HeadlessHostExitCode : int {
  Success          = 0,
  UsageError       = 2,
  /// The editor backend cannot start. No other backend is tried.
  BackendError     = 3,
  /// The project cannot be opened or created.
  ProjectLoadError = 4,
  /// The automation server or the session file cannot start.
  SessionError     = 5,
};

/// Options of `alcedo_main --headless`.
struct HeadlessHostOptions {
  /// `--project <path>`: an existing packed project.
  QString                                  project_path;
  /// `--create <folder> <name>`: a new project in a folder.
  QString                                  create_folder;
  QString                                  create_name;
  /// `--session <name>`.
  QString                                  session_name = QStringLiteral("default");
  /// `--session-dir <dir>`. Empty selects `DefaultAutomationSessionDir()`.
  QString                                  session_dir;
  /// `--editor-backend cuda|opencl|metal`. Empty selects the saved setting, then the platform
  /// default.
  std::optional<editor_rhi::EditorBackend> editor_backend;
  /// `--viewport WxH`: the presentation size of the editor session.
  int                                      viewport_width  = 1920;
  int                                      viewport_height = 1080;
  /// `--settings-dir <dir>`: INI settings in this directory instead of the user settings.
  QString                                  settings_dir;
  /// `--log-file <path>`: the application log file.
  QString                                  log_file;
};

/// The pipeline accelerator preference that matches @p backend.
auto HeadlessAcceleratorPreference(editor_rhi::EditorBackend backend)
    -> AcceleratorBackendPreference;

/// Resolves and starts @p backend without a window and makes it the active editor backend.
/// Metal also needs a Metal device. Returns the error text on failure and an empty string on
/// success. No other backend is tried.
auto StartHeadlessEditorBackend(editor_rhi::EditorBackend backend) -> QString;

/// True when the command line has `--headless`.
auto IsHeadlessHostRequested(int argc, char** argv) -> bool;

/// Parses the arguments after the program name. `--headless` is accepted and ignored. Exactly
/// one of `--project` and `--create` is required.
auto ParseHeadlessHostOptions(const QStringList& arguments, HeadlessHostOptions* options,
                              QString* error) -> bool;

/// Sends every default-constructed `QSettings` to INI files in @p settings_dir. Call before the
/// first `QSettings` construction.
void ApplyHeadlessSettingsDirectory(const QString& settings_dir);

/// Runs the headless host: `QCoreApplication`, the editor backend, `ApplicationModuleHost`
/// with a headless presentation sink, the project, the automation server, and the session file.
/// Returns when `session.shutdown` ends the session. Call before any application object exists.
auto RunHeadlessHost(int argc, char** argv) -> int;

}  // namespace alcedo::automation
