//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "utils/diagnostics/app_logging.hpp"

#include <QCoreApplication>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QStringList>
#include <QThread>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <string_view>
#include <utility>

#include "utils/logging/log.hpp"

namespace alcedo::diag {

#define ALCEDO_DEFINE_QT_LOG_CATEGORY_(core_id, qt_category_function, name) \
  Q_LOGGING_CATEGORY(qt_category_function, name)
ALCEDO_LOG_CATEGORY_LIST(ALCEDO_DEFINE_QT_LOG_CATEGORY_)
#undef ALCEDO_DEFINE_QT_LOG_CATEGORY_

namespace {

// Flush buffered info/debug writes once this many bytes accumulate. Keeps the
// log durable in bounded batches instead of once per frame while preventing
// unbounded buffering across a long interactive session.
constexpr std::size_t kBufferedFlushThreshold = 1 << 16;  // 64 KiB

std::mutex             g_log_lock;
std::unique_ptr<QFile> g_log_file;
QtMessageHandler      g_previous_handler = nullptr;
QLoggingCategory::CategoryFilter g_previous_category_filter = nullptr;
QString               g_log_file_path;
// Written under g_log_lock at initialization, read without it after each message.
std::atomic<bool>                g_console_enabled{false};
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

  if (g_console_enabled.load(std::memory_order_relaxed)) {
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

/// The QLoggingCategory of each core category, in the order of alcedo::log::Category.
auto QtCategories() -> const std::array<const QLoggingCategory*, log::kCategoryCount>& {
  static const std::array<const QLoggingCategory*, log::kCategoryCount> categories = {
#define ALCEDO_QT_LOG_CATEGORY_ADDRESS_(core_id, qt_category_function, name) \
  &qt_category_function(),
      ALCEDO_LOG_CATEGORY_LIST(ALCEDO_QT_LOG_CATEGORY_ADDRESS_)
#undef ALCEDO_QT_LOG_CATEGORY_ADDRESS_
  };
  return categories;
}

/// Handler of the Qt-free interface: one message to the QLoggingCategory of the same name, with
/// the source location of the ALCEDO_LOG_* call. QMessageLogger checks the category rules again,
/// so a rule change between the two checks drops the message rather than writing it.
void ForwardCoreLogMessage(const log::Record& record) {
  const auto index = static_cast<std::size_t>(record.category);
  if (index >= log::kCategoryCount) {
    return;
  }
  const QLoggingCategory& category = *QtCategories()[index];
  const QMessageLogger    logger(record.file, record.line, record.function);
  const int               size = static_cast<int>(record.message.size());
  const char*             text = record.message.data();
  switch (record.level) {
    case log::Level::Debug:
      logger.debug(category, "%.*s", size, text);
      return;
    case log::Level::Info:
      logger.info(category, "%.*s", size, text);
      return;
    case log::Level::Warn:
      logger.warning(category, "%.*s", size, text);
      return;
    case log::Level::Error:
      logger.critical(category, "%.*s", size, text);
      return;
  }
}

/// Copies the enabled levels of @p category into the AlcedoLog flags when it is one of the
/// categories of ALCEDO_LOG_CATEGORY_LIST, so a disabled level costs one flag check in the core.
void ApplyQtRulesToCoreCategory(const QLoggingCategory& category) {
  const std::string_view name(category.categoryName());
  for (std::size_t index = 0; index < log::kCategoryCount; ++index) {
    const auto core = static_cast<log::Category>(index);
    if (log::CategoryName(core) != name) {
      continue;
    }
    log::SetEnabled(core, log::Level::Debug, category.isDebugEnabled());
    log::SetEnabled(core, log::Level::Info, category.isInfoEnabled());
    log::SetEnabled(core, log::Level::Warn, category.isWarningEnabled());
    log::SetEnabled(core, log::Level::Error, category.isCriticalEnabled());
    return;
  }
}

/// Qt calls the category filter for every category when it is installed and when the rules
/// change (QLoggingCategory::setFilterRules, QT_LOGGING_RULES). The previous filter applies the
/// rules first.
void CoreLogCategoryFilter(QLoggingCategory* category) {
  if (g_previous_category_filter != nullptr) {
    g_previous_category_filter(category);
  }
  ApplyQtRulesToCoreCategory(*category);
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

/// The files of one session in the log directory: one `<yyyyMMdd_HHmmss>_<pid>` stem.
struct LogSessionFiles {
  QFileInfoList files;
  qint64        bytes = 0;
};

/// Applies the retention rule of InitializeApplicationLogging to @p directory. Returns one line for
/// each file that could not be removed, for the log that opens after this call.
auto RemoveOldLogSessions(const QString& directory, qint64 current_pid) -> QStringList {
  static const QRegularExpression kSessionFileName(
      QStringLiteral("^alcedo_(\\d{8}_\\d{6})_(\\d+)(?:_[A-Za-z0-9]+)?\\.log$"));

  // Ordered by timestamp, then process ID: the oldest session first.
  std::map<std::pair<QString, qint64>, LogSessionFiles> sessions;
  const QFileInfoList                                   entries = QDir(directory).entryInfoList(
      QStringList{QStringLiteral("alcedo_*.log")}, QDir::Files | QDir::NoDotAndDotDot);
  for (const QFileInfo& entry : entries) {
    const QRegularExpressionMatch match = kSessionFileName.match(entry.fileName());
    if (!match.hasMatch()) {
      continue;
    }
    const qint64 pid = match.captured(2).toLongLong();
    if (pid == current_pid) {
      continue;
    }
    LogSessionFiles& session = sessions[{match.captured(1), pid}];
    session.files.append(entry);
    session.bytes += entry.size();
  }

  std::size_t remove_count = 0;
  if (sessions.size() > static_cast<std::size_t>(kRetainedLogSessionCount)) {
    remove_count = sessions.size() - static_cast<std::size_t>(kRetainedLogSessionCount);
  }
  qint64 kept_bytes = 0;
  auto   first_kept = std::next(sessions.begin(), static_cast<std::ptrdiff_t>(remove_count));
  for (auto it = first_kept; it != sessions.end(); ++it) {
    kept_bytes += it->second.bytes;
  }
  for (; first_kept != sessions.end() && kept_bytes > kRetainedLogSessionBytes; ++first_kept) {
    kept_bytes -= first_kept->second.bytes;
  }

  QStringList failures;
  for (auto it = sessions.begin(); it != first_kept; ++it) {
    for (const QFileInfo& entry : it->second.files) {
      QFile file(entry.absoluteFilePath());
      if (!file.remove()) {
        failures.append(QStringLiteral("logging.retention.remove_failed path=%1 error=%2")
                            .arg(entry.absoluteFilePath(), file.errorString()));
      }
    }
  }
  return failures;
}

}  // namespace

namespace {

/// Opens @p file_path and installs the message handler. With @p apply_retention, it first removes
/// old sessions from the directory of @p file_path (RemoveOldLogSessions). Returns the open path,
/// or an empty string when the file cannot be opened.
auto InitializeApplicationLoggingAt(const QString& file_path, bool apply_retention) -> QString {
  QString     initialized_path;
  QStringList retention_failures;
  {
    std::lock_guard lock(g_log_lock);
    if (g_log_file && g_log_file->isOpen()) {
      return g_log_file_path;
    }

    QDir().mkpath(QFileInfo(file_path).absolutePath());
    if (apply_retention) {
      retention_failures = RemoveOldLogSessions(QFileInfo(file_path).absolutePath(),
                                                QCoreApplication::applicationPid());
    }
    g_log_file_path = file_path;

    auto file = std::make_unique<QFile>(g_log_file_path);
    if (!file->open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
      g_log_file_path.clear();
      return {};
    }

    g_log_file        = std::move(file);
    g_console_enabled.store(qEnvironmentVariableIntValue("ALCEDO_LOG_CONSOLE") != 0,
                            std::memory_order_relaxed);
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

  // Each category already holds the result of the rules. installFilter calls the new filter
  // before it returns the previous one, so the states are copied here once more after it.
  g_previous_category_filter = QLoggingCategory::installFilter(CoreLogCategoryFilter);
  for (const QLoggingCategory* category : QtCategories()) {
    ApplyQtRulesToCoreCategory(*category);
  }
  log::SetHandler(ForwardCoreLogMessage);

  qCInfo(appLog).noquote()
      << QStringLiteral("logging.initialized path=%1").arg(initialized_path);
  for (const QString& failure : retention_failures) {
    qCWarning(appLog).noquote() << failure;
  }
  return initialized_path;
}

}  // namespace

auto InitializeApplicationLogging(const QString& preferred_directory) -> QString {
  const QString timestamp =
      QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss"));
  const qint64 pid = QCoreApplication::applicationPid();
  return InitializeApplicationLoggingAt(
      QDir(ResolveLogDirectory(preferred_directory))
          .filePath(QStringLiteral("alcedo_%1_%2.log").arg(timestamp).arg(pid)),
      true);
}

auto InitializeApplicationLoggingToFile(const QString& file_path) -> QString {
  return InitializeApplicationLoggingAt(QFileInfo(file_path).absoluteFilePath(), false);
}

void ShutdownApplicationLogging() {
  log::SetHandler(nullptr);
  if (g_previous_category_filter != nullptr) {
    QLoggingCategory::installFilter(g_previous_category_filter);
    g_previous_category_filter = nullptr;
  }
  log::ResetEnabledLevels();

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

void FlushApplicationLog() {
  std::lock_guard lock(g_log_lock);
  if (g_log_file && g_log_file->isOpen()) {
    g_log_file->flush();
    g_buffered_bytes = 0;
  }
}

auto CurrentLogFilePath() -> QString {
  std::lock_guard lock(g_log_lock);
  return g_log_file_path;
}

auto LogDirectory() -> QString {
  const QString log_file_path = CurrentLogFilePath();
  if (!log_file_path.isEmpty()) {
    return QFileInfo(log_file_path).absolutePath();
  }
  return QDir(ResolveLogDirectory({})).absolutePath();
}

auto CompanionLogFilePath(const QString& process_name) -> QString {
  std::lock_guard lock(g_log_lock);
  if (!g_log_file || !g_log_file->isOpen()) {
    return {};
  }
  const QFileInfo log_file(g_log_file_path);
  return log_file.dir().filePath(log_file.completeBaseName() + QLatin1Char('_') + process_name +
                                 QStringLiteral(".log"));
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