//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/log_folder_controller.hpp"

#include <QDesktopServices>
#include <QDir>
#include <utility>

#include "utils/diagnostics/app_logging.hpp"

namespace alcedo::ui {

LogFolderController::LogFolderController(QObject* parent, OpenUrlFunction open_url)
    : QObject(parent), open_url_(std::move(open_url)) {
  if (!open_url_) {
    open_url_ = [](const QUrl& url) { return QDesktopServices::openUrl(url); };
  }
}

QString LogFolderController::logDirectory() const { return alcedo::diag::LogDirectory(); }

bool LogFolderController::openLogFolder() {
  const QString directory = logDirectory();
  if (!QDir().mkpath(directory)) {
    qCWarning(alcedo::diag::appLog).noquote()
        << "Open Log Folder: cannot create the log directory" << directory;
    return false;
  }
  if (!open_url_(QUrl::fromLocalFile(directory))) {
    qCWarning(alcedo::diag::appLog).noquote()
        << "Open Log Folder: the file manager did not open" << directory;
    return false;
  }
  return true;
}

void LogFolderController::SetOpenUrlForTesting(OpenUrlFunction open_url) {
  open_url_ = std::move(open_url);
}

}  // namespace alcedo::ui
