//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QLoggingCategory>
#include <QString>

#include <chrono>

namespace alcedo::diag {

Q_DECLARE_LOGGING_CATEGORY(appLog)
Q_DECLARE_LOGGING_CATEGORY(semanticLog)
Q_DECLARE_LOGGING_CATEGORY(semanticRpcLog)
Q_DECLARE_LOGGING_CATEGORY(semanticDbLog)
Q_DECLARE_LOGGING_CATEGORY(editorLog)

auto InitializeApplicationLogging(const QString& preferred_directory = {}) -> QString;
void ShutdownApplicationLogging();
auto CurrentLogFilePath() -> QString;

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
