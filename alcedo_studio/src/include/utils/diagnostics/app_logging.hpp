//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstddef>

#include <QLoggingCategory>
#include <QString>

#include <chrono>

namespace alcedo::diag {

Q_DECLARE_LOGGING_CATEGORY(appLog)
Q_DECLARE_LOGGING_CATEGORY(semanticLog)
Q_DECLARE_LOGGING_CATEGORY(semanticRpcLog)
Q_DECLARE_LOGGING_CATEGORY(semanticDbLog)
Q_DECLARE_LOGGING_CATEGORY(editorLog)
/// Per-frame editor presentation diagnostics. Disabled at info level by default
/// (see InitializeApplicationLogging); per-frame detail lives at debug level and
/// requires an explicit runtime rule to enable. Keeps presentation traffic off
/// the frame budget: no per-info-line disk flush and no default output.
Q_DECLARE_LOGGING_CATEGORY(editorPresentLog)

auto InitializeApplicationLogging(const QString& preferred_directory = {}) -> QString;
void ShutdownApplicationLogging();
auto CurrentLogFilePath() -> QString;

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