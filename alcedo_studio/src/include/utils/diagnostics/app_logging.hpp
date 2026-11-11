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
auto InitializeApplicationLogging(const QString& preferred_directory = {}) -> QString;
/// Writes the application log to @p file_path (appends). Creates the parent directory. Returns
/// the absolute path, or an empty string when the file cannot be opened.
auto InitializeApplicationLoggingToFile(const QString& file_path) -> QString;
void ShutdownApplicationLogging();
auto CurrentLogFilePath() -> QString;
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