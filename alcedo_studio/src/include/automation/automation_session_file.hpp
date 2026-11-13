//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QDateTime>
#include <QString>
#include <optional>
#include <vector>

namespace alcedo::automation {

/// A JSON file that describes one running automation session. Clients use it to find the
/// socket. The host writes it after the server listens and removes it during shutdown.
struct AutomationSessionFile {
  int       protocol_version = 0;
  QString   session_name;
  QString   socket_name;
  qint64    pid = 0;
  /// `headless` or `gui`.
  QString   host_mode;
  QString   project_path;
  QDateTime started_at;
};

/// One file found in a session directory.
struct AutomationSessionFileEntry {
  /// Absolute path of the file.
  QString                              path;
  /// The session record, or nothing when the file cannot be read or is not an Alcedo session
  /// record.
  std::optional<AutomationSessionFile> file;
  /// Why `file` is empty.
  QString                              read_error;
  /// True only for a session record whose process does not run. A client also treats a session
  /// whose socket does not answer `session.ping` as stale. A file without a session record is
  /// never stale, so that no caller removes a file that Alcedo did not write.
  bool                                 stale = false;
};

/// `<AppLocalDataLocation>/automation/sessions`. The organization and application names must
/// be set before the call.
auto DefaultAutomationSessionDir() -> QString;

/// Session names use letters, digits, `.`, `_`, and `-` only, so that a name is a safe file
/// name.
auto IsValidAutomationSessionName(const QString& name) -> bool;

auto AutomationSessionFilePath(const QString& session_dir, const QString& session_name) -> QString;

/// Writes `<session_dir>/<session_name>.json` atomically. Creates the directory when needed.
[[nodiscard]] auto WriteAutomationSessionFile(const QString&               session_dir,
                                              const AutomationSessionFile& file,
                                              QString*                     error = nullptr) -> bool;

/// Reads one session file. Returns nothing and writes @p error when the file cannot be read or is
/// not an Alcedo session record: a JSON object with every field that WriteAutomationSessionFile
/// writes, valid values, and a file name of `<session_name>.json`.
auto               ReadAutomationSessionFile(const QString& path, QString* error = nullptr)
    -> std::optional<AutomationSessionFile>;

/// Reads every `*.json` file in @p session_dir, sorted by file name, and marks the stale session
/// records.
auto ReadAutomationSessionFiles(const QString& session_dir)
    -> std::vector<AutomationSessionFileEntry>;

/// Removes `<session_dir>/<session_name>.json`. Returns true when the file does not exist after
/// the call.
auto RemoveAutomationSessionFile(const QString& session_dir, const QString& session_name) -> bool;

/// True when a process with @p pid exists.
auto IsAutomationProcessRunning(qint64 pid) -> bool;

}  // namespace alcedo::automation
