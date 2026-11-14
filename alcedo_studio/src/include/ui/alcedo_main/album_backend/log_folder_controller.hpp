//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QObject>
#include <QString>
#include <QUrl>
#include <functional>

namespace alcedo::ui {

/**
 * @brief The Open Log Folder action of Settings > About (issue 824).
 *
 * Opens the directory of the application log (alcedo::diag::LogDirectory) in the system file
 * manager: Explorer on Windows, Finder on macOS. The user cannot change the log directory or the
 * log levels; they come from the logging code.
 *
 * Threading: GUI thread only. Side effects: creates the log directory when it does not exist,
 * and starts the file manager through the open-URL function.
 */
class LogFolderController final : public QObject {
  Q_OBJECT

 public:
  /// Opens a local URL in the file manager. Returns false when no application opened it.
  using OpenUrlFunction = std::function<bool(const QUrl&)>;

  /// @p open_url defaults to QDesktopServices::openUrl. Tests pass a stand-in.
  explicit LogFolderController(QObject* parent = nullptr, OpenUrlFunction open_url = {});

  /// The directory that openLogFolder opens.
  Q_INVOKABLE QString logDirectory() const;

  /// Creates the log directory when it does not exist and opens it in the file manager.
  /// Returns false when the directory cannot be created or the file manager does not open it;
  /// the failure is written to `alcedo.app` with the directory.
  Q_INVOKABLE bool    openLogFolder();

  /// Replaces the open-URL function (tests).
  void                SetOpenUrlForTesting(OpenUrlFunction open_url);

 private:
  OpenUrlFunction open_url_;
};

}  // namespace alcedo::ui
