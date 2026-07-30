//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "utils/diagnostics/app_logging.hpp"

#include <QCoreApplication>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QStandardPaths>
#include <QThread>

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <utility>

namespace alcedo::diag {

Q_LOGGING_CATEGORY(appLog, "alcedo.app")
Q_LOGGING_CATEGORY(semanticLog, "alcedo.semantic")
Q_LOGGING_CATEGORY(semanticRpcLog, "alcedo.semantic.rpc")
Q_LOGGING_CATEGORY(semanticDbLog, "alcedo.semantic.db")
Q_LOGGING_CATEGORY(editorLog, "alcedo.editor")
Q_LOGGING_CATEGORY(editorPresentLog, "alcedo.editor.present")

namespace {

// Flush buffered info/debug writes once this many bytes accumulate. Keeps the
// log durable in bounded batches instead of once per frame while preventing
// unbounded buffering across a long interactive session.
constexpr std::size_t kBufferedFlushThreshold = 1 << 16;  // 64 KiB

std::mutex             g_log_lock;
std::unique_ptr<QFile> g_log_file;
QtMessageHandler      g_previous_handler = nullptr;
QString               g_log_file_path;
bool                  g_console_enabled = false;
std::size_t           g_buffered_bytes   = 0;

// Test-only counters. Guarded by g_log_lock. They count messages that actually
// reach the handler (Qt suppresses disabled category/severity before dispatch).
std::size_t g_present_messages   = 0;
std::size_t g_info_debug_messages = 0;
std::size_t g_immediate_flushes   = 0;
std::size_t g_batch_flushes       = 0;

auto IsPresentCategory(const char* category) -> bool {
  return category != nullptr && std::string_view(category) == "alcedo.editor.present";
}

auto IsImmediateFlushType(QtMsgType type) -> bool {
  return type == QtWarningMsg || type == QtCriticalMsg || type == QtFatalMsg;
}

auto MessageTypeName(QtMsgType type) -> const char* {
  switch (type) {
    case QtDebugMsg:
      return "DEBUG";
    case QtInfoMsg:
      return "INFO";
    case QtWarningMsg:
      return "WARN";
    case QtCriticalMsg:
      return "ERROR";
    case QtFatalMsg:
      return "FATAL";
  }
  return "LOG";
}

auto ThreadIdString() -> QString {
  return QString::number(reinterpret_cast<quintptr>(QThread::currentThreadId()), 16);
}

void ApplicationMessageHandler(QtMsgType type, const QMessageLogContext& context,
                               const QString& message) {
  const QString timestamp = QDateTime::currentDateTime().toString(Qt::ISODateWithMs);
  const QString category =
      context.category && context.category[0] != '\0' ? QString::fromUtf8(context.category)
                                                      : QStringLiteral("default");
  QString line = QStringLiteral("%1 [%2] [tid=%3] [%4] %5")
                     .arg(timestamp, QString::fromLatin1(MessageTypeName(type)), ThreadIdString(),
                          category, message);
  if (context.file && context.line > 0) {
    line += QStringLiteral(" (%1:%2)").arg(QString::fromUtf8(context.file)).arg(context.line);
  }
  line += QLatin1Char('\n');

  const QByteArray bytes = line.toUtf8();
  const bool       immediate = IsImmediateFlushType(type);
  {
    std::lock_guard lock(g_log_lock);
    if (IsPresentCategory(context.category)) {
      ++g_present_messages;
    }
    if (!immediate) {
      ++g_info_debug_messages;
    }
    if (g_log_file && g_log_file->isOpen()) {
      g_log_file->write(bytes);
      if (immediate) {
        g_log_file->flush();
        ++g_immediate_flushes;
        g_buffered_bytes = 0;
      } else {
        g_buffered_bytes += static_cast<std::size_t>(bytes.size());
        if (g_buffered_bytes >= kBufferedFlushThreshold) {
          g_log_file->flush();
          ++g_batch_flushes;
          g_buffered_bytes = 0;
        }
      }
    }
  }

  if (g_console_enabled) {
    std::fwrite(bytes.constData(), 1, static_cast<size_t>(bytes.size()), stderr);
    // Console output is opt-in (ALCEDO_LOG_CONSOLE); keep stderr responsive but
    // do not fflush per info/debug line — the OS stream buffers it.
    if (immediate) {
      std::fflush(stderr);
    }
  }

  if (type == QtFatalMsg) {
    std::abort();
  }
}

auto ResolveLogDirectory(const QString& preferred_directory) -> QString {
  if (!preferred_directory.isEmpty()) {
    return preferred_directory;
  }
  const QString env_dir = qEnvironmentVariable("ALCEDO_LOG_DIR");
  if (!env_dir.isEmpty()) {
    return env_dir;
  }
  QString base = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
  if (base.isEmpty()) {
    base = QDir::tempPath() + QStringLiteral("/Alcedo");
  }
  return QDir(base).filePath(QStringLiteral("logs"));
}

}  // namespace

auto InitializeApplicationLogging(const QString& preferred_directory) -> QString {
  QString initialized_path;
  {
    std::lock_guard lock(g_log_lock);
    if (g_log_file && g_log_file->isOpen()) {
      return g_log_file_path;
    }

    const QString log_dir_path = ResolveLogDirectory(preferred_directory);
    QDir().mkpath(log_dir_path);

    const QString timestamp =
        QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss"));
    const qint64 pid = QCoreApplication::applicationPid();
    g_log_file_path =
        QDir(log_dir_path).filePath(QStringLiteral("alcedo_%1_%2.log").arg(timestamp).arg(pid));

    auto file = std::make_unique<QFile>(g_log_file_path);
    if (!file->open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
      g_log_file_path.clear();
      return {};
    }

    g_log_file        = std::move(file);
    g_console_enabled = qEnvironmentVariableIntValue("ALCEDO_LOG_CONSOLE") != 0;
    g_buffered_bytes  = 0;

    if (qEnvironmentVariableIsEmpty("QT_LOGGING_RULES")) {
      QLoggingCategory::setFilterRules(QStringLiteral(
          "*.debug=false\n"
          "qt.*.info=false\n"
          "alcedo.*.info=true\n"
          "alcedo.editor.present.info=false\n"
          "alcedo.*.warning=true\n"));
    }

    g_previous_handler = qInstallMessageHandler(ApplicationMessageHandler);
    initialized_path   = g_log_file_path;
  }

  qCInfo(appLog).noquote()
      << QStringLiteral("logging.initialized path=%1").arg(initialized_path);
  return initialized_path;
}

void ShutdownApplicationLogging() {
  std::lock_guard lock(g_log_lock);
  qInstallMessageHandler(g_previous_handler);
  g_previous_handler = nullptr;
  if (g_log_file) {
    g_log_file->flush();
    g_log_file->close();
    g_log_file.reset();
  }
  g_buffered_bytes = 0;
}

auto CurrentLogFilePath() -> QString {
  std::lock_guard lock(g_log_lock);
  return g_log_file_path;
}

auto DiagnosticCountersSnapshot() -> DiagnosticCounters {
  std::lock_guard lock(g_log_lock);
  return DiagnosticCounters{
      .present_messages_written   = g_present_messages,
      .info_debug_messages_written = g_info_debug_messages,
      .immediate_flushes           = g_immediate_flushes,
      .batch_flushes               = g_batch_flushes,
  };
}

void ResetDiagnosticCountersForTesting() {
  std::lock_guard lock(g_log_lock);
  g_present_messages   = 0;
  g_info_debug_messages = 0;
  g_immediate_flushes   = 0;
  g_batch_flushes       = 0;
  g_buffered_bytes      = 0;
}

TraceScope::TraceScope(const QLoggingCategory& category, QString event, QString details)
    : category_(category),
      event_(std::move(event)),
      details_(std::move(details)),
      started_at_(std::chrono::steady_clock::now()),
      enabled_(category_.isInfoEnabled()) {
  if (!enabled_) {
    return;
  }
  if (details_.isEmpty()) {
    QMessageLogger(QT_MESSAGELOG_FILE, QT_MESSAGELOG_LINE, QT_MESSAGELOG_FUNC)
        .info(category_)
        .noquote()
        << event_ + QStringLiteral(".begin");
  } else {
    QMessageLogger(QT_MESSAGELOG_FILE, QT_MESSAGELOG_LINE, QT_MESSAGELOG_FUNC)
        .info(category_)
        .noquote()
        << event_ + QStringLiteral(".begin ") + details_;
  }
}

TraceScope::~TraceScope() {
  if (!enabled_) {
    return;
  }
  const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::steady_clock::now() - started_at_)
                              .count();
  QString message =
      event_ + QStringLiteral(".end elapsed_ms=") + QString::number(elapsed_ms);
  if (!details_.isEmpty()) {
    message += QLatin1Char(' ');
    message += details_;
  }
  QMessageLogger(QT_MESSAGELOG_FILE, QT_MESSAGELOG_LINE, QT_MESSAGELOG_FUNC)
      .info(category_)
      .noquote()
      << message;
}

}  // namespace alcedo::diag