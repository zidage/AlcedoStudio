//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QLoggingCategory>
#include <QString>
#include <chrono>
#include <cstddef>

#include "utils/logging/log_categories.hpp"

namespace alcedo::diag {

/// One QLoggingCategory function for each entry of ALCEDO_LOG_CATEGORY_LIST
/// (utils/logging/log_categories.hpp), for example `appLog` (alcedo.app) and `gpuLog` (alcedo.gpu).
/// Every production log call names one of them.
///
/// `editorPresentLog` (alcedo.editor.present) holds per-frame editor presentation diagnostics. It
/// is disabled at info level by default (see InitializeApplicationLogging); per-frame detail lives
/// at debug level and requires an explicit runtime rule to enable. Keeps presentation traffic off
/// the frame budget: no per-info-line disk flush and no default output.
#define ALCEDO_DECLARE_QT_LOG_CATEGORY_(core_id, qt_category_function, name) \
  Q_DECLARE_LOGGING_CATEGORY(qt_category_function)
ALCEDO_LOG_CATEGORY_LIST(ALCEDO_DECLARE_QT_LOG_CATEGORY_)
#undef ALCEDO_DECLARE_QT_LOG_CATEGORY_

/// Opens the log file and installs the Qt message handler. Also installs the AlcedoLog handler
/// (utils/logging/log.hpp): a message of the Qt-free interface goes to the QLoggingCategory of the
/// same name, so the Qt rules filter it and this log file receives it.
///
/// The file is `alcedo_<yyyyMMdd_HHmmss>_<pid>.log` in @p preferred_directory, `ALCEDO_LOG_DIR`,
/// or `AppLocalData/logs`. Before it opens the file, it removes old sessions from that directory.
/// A session is the files `alcedo_<yyyyMMdd_HHmmss>_<pid>[_<process>].log` of one stem. It keeps
/// the newest kRetainedLogSessionCount sessions, and then removes the oldest of them until their
/// files hold at most kRetainedLogSessionBytes. Files of the current process and other file
/// names stay. A file that cannot be removed stays and gives one warning line in `alcedo.app`.
auto InitializeApplicationLogging(const QString& preferred_directory = {}) -> QString;
/// Writes the application log to @p file_path (appends). Creates the parent directory. Returns
/// the absolute path, or an empty string when the file cannot be opened. Removes no files.
auto InitializeApplicationLoggingToFile(const QString& file_path) -> QString;
void ShutdownApplicationLogging();
auto CurrentLogFilePath() -> QString;
/// The directory of the application log: the directory of the open log file, or, before a log
/// file is open, the directory InitializeApplicationLogging uses without a preferred directory
/// (`ALCEDO_LOG_DIR`, else `AppLocalData/logs`). Settings > About opens it in the file manager.
auto LogDirectory() -> QString;
/// The log file of another process of this session, such as the AI sidecar: the name of the open
/// log file with `_<process_name>` before `.log`, in the same directory. For
/// `alcedo_20261111_101500_1234.log` and `mind` it is `alcedo_20261111_101500_1234_mind.log`, so
/// the retention rule keeps or removes it with the application log. Empty when no log file is open.
auto                    CompanionLogFilePath(const QString& process_name) -> QString;

/// Retention of InitializeApplicationLogging: the number of old sessions it keeps.
inline constexpr int    kRetainedLogSessionCount = 20;
/// Retention of InitializeApplicationLogging: the largest total size of the old sessions it keeps.
inline constexpr qint64 kRetainedLogSessionBytes = qint64{100} * 1024 * 1024;
/// Write buffered info/debug lines to the log file now. For a line that must survive a crash
/// in the next call, such as one written before a driver call that may take the process down.
void FlushApplicationLog();

/// Test-only snapshot of internal write/flush counters. Production code never
/// reads these; they exist so logging tests can assert the buffered-flush policy
/// (info/debug writes do not flush once per frame, warnings flush immediately)
/// and the disabled-by-default presentation category.
struct DiagnosticCounters {
  std::size_t present_messages_written   = 0;  ///< Messages in alcedo.editor.present.
  std::size_t info_debug_messages_written = 0;  ///< Buffered-severity messages seen by the handler.
  std::size_t immediate_flushes           = 0;  ///< Flushes triggered by warning/critical/fatal.
  std::size_t batch_flushes                = 0;  ///< Flushes triggered by the buffered-write threshold.
};
auto DiagnosticCountersSnapshot() -> DiagnosticCounters;
void ResetDiagnosticCountersForTesting();

class TraceScope final {
 public:
  TraceScope(const QLoggingCategory& category, QString event, QString details = {});
  ~TraceScope();

  TraceScope(const TraceScope&)            = delete;
  TraceScope& operator=(const TraceScope&) = delete;

 private:
  const QLoggingCategory&                         category_;
  QString                                         event_;
  QString                                         details_;
  std::chrono::steady_clock::time_point           started_at_;
  bool                                            enabled_ = false;
};

}  // namespace alcedo::diag