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
  /// The file content, or nothing when the file cannot be read.
  std::optional<AutomationSessionFile> file;
  QString                              read_error;
  /// True when the file cannot be read or its process does not run. A client also treats a
  /// session whose socket does not answer `session.ping` as stale.
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

/// Reads one session file. Returns nothing and writes @p error for an unreadable file.
auto               ReadAutomationSessionFile(const QString& path, QString* error = nullptr)
    -> std::optional<AutomationSessionFile>;

/// Reads every `*.json` file in @p session_dir, sorted by file name, and marks the stale ones.
auto ReadAutomationSessionFiles(const QString& session_dir)
    -> std::vector<AutomationSessionFileEntry>;

/// Removes `<session_dir>/<session_name>.json`. Returns true when the file does not exist after
/// the call.
auto RemoveAutomationSessionFile(const QString& session_dir, const QString& session_name) -> bool;

/// True when a process with @p pid exists.
auto IsAutomationProcessRunning(qint64 pid) -> bool;

}  // namespace alcedo::automation
