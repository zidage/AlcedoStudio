//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "cli_commands.hpp"

#include <QByteArray>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QString>
#include <QtGlobal>
#include <cstdio>
#include <functional>
#include <optional>
#include <utility>
#include <vector>

#include "automation/automation_client.hpp"
#include "automation/automation_protocol.hpp"
#include "automation/automation_session_file.hpp"

namespace alcedo::cli {
namespace {

using automation::AutomationClient;
using automation::AutomationResponse;
using automation::AutomationSessionFile;
using automation::AutomationSessionFileEntry;

/// Extra wait after the command time limit, so that the session can send its timeout error.
constexpr int  kResponseWaitMarginMs = 5000;
constexpr int  kConnectTimeoutMs     = 5000;
constexpr int  kPingWaitMs           = 2000;

constexpr auto kUsage                = R"(Usage: alcedo-cli [global options] <command> [arguments]

Global options:
  --session <name>      Use this session. Also: the ALCEDO_SESSION environment variable.
  --session-dir <dir>   Directory of the session files.
  --json                Print the JSON-RPC result or error object.
  --timeout <ms>        Time limit for the command (timeout_ms).

Commands:
  call <method> [<params-json>]   Send one command.
  schema                          Print the command schemas.
  session list                    List the session files and their state.
  session prune                   Remove the stale session files.
  watch                           Print notifications until interrupted.
  help                            Print this text.

Exit codes: 0 success, 1 command error, 2 session selection error, 3 connection error,
4 usage error.
)";

struct CliOptions {
  QString            session_name;
  QString            session_dir;
  bool               json = false;
  std::optional<int> timeout_ms;
  QStringList        positional;
};

void WriteStream(std::FILE* stream, const QString& text) {
  const QByteArray bytes = text.toUtf8();
  std::fwrite(bytes.constData(), 1, static_cast<std::size_t>(bytes.size()), stream);
  std::fflush(stream);
}

void WriteOut(const QString& text) { WriteStream(stdout, text); }
void WriteErr(const QString& text) { WriteStream(stderr, text); }

auto Exit(CliExitCode code) -> int { return static_cast<int>(code); }

auto UsageFailure(const QString& message) -> int {
  WriteErr(QStringLiteral("alcedo-cli: %1\n\n").arg(message) + QString::fromUtf8(kUsage));
  return Exit(CliExitCode::UsageError);
}

auto ToJsonText(const QJsonValue& value, QJsonDocument::JsonFormat format) -> QString {
  if (value.isObject()) {
    return QString::fromUtf8(QJsonDocument(value.toObject()).toJson(format)).trimmed();
  }
  if (value.isArray()) {
    return QString::fromUtf8(QJsonDocument(value.toArray()).toJson(format)).trimmed();
  }
  // Wrap a scalar in an array to use the Qt serializer, then remove the brackets.
  const QString wrapped =
      QString::fromUtf8(QJsonDocument(QJsonArray{value}).toJson(QJsonDocument::Compact));
  return wrapped.mid(1, wrapped.size() - 2);
}

/// Splits global options from the command words. Global options are accepted at any position.
auto ParseOptions(const QStringList& arguments, CliOptions* options, QString* error) -> bool {
  for (qsizetype index = 0; index < arguments.size(); ++index) {
    const QString& argument   = arguments.at(index);
    auto           take_value = [&](const QString& name, QString* value) -> bool {
      if (argument.startsWith(name + QLatin1Char('='))) {
        *value = argument.mid(name.size() + 1);
        return true;
      }
      if (index + 1 >= arguments.size()) {
        *error = QStringLiteral("option %1 needs a value").arg(name);
        return false;
      }
      *value = arguments.at(++index);
      return true;
    };
    auto is_option = [&](const QString& name) {
      return argument == name || argument.startsWith(name + QLatin1Char('='));
    };

    if (is_option(QStringLiteral("--session"))) {
      if (!take_value(QStringLiteral("--session"), &options->session_name)) return false;
    } else if (is_option(QStringLiteral("--session-dir"))) {
      if (!take_value(QStringLiteral("--session-dir"), &options->session_dir)) return false;
    } else if (is_option(QStringLiteral("--timeout"))) {
      QString value;
      if (!take_value(QStringLiteral("--timeout"), &value)) return false;
      bool      ok      = false;
      const int timeout = value.toInt(&ok);
      if (!ok || timeout <= 0) {
        *error = QStringLiteral("--timeout needs a positive number of milliseconds");
        return false;
      }
      options->timeout_ms = timeout;
    } else if (argument == QStringLiteral("--json")) {
      options->json = true;
    } else {
      options->positional.push_back(argument);
    }
  }
  if (options->session_dir.isEmpty()) {
    options->session_dir = automation::DefaultAutomationSessionDir();
  }
  if (options->session_name.isEmpty()) {
    options->session_name = qEnvironmentVariable("ALCEDO_SESSION");
  }
  return true;
}

/// True when the socket of @p file answers `session.ping`.
auto SessionAnswersPing(const AutomationSessionFile& file) -> bool {
  AutomationClient client;
  if (!client.Connect(file.socket_name, kPingWaitMs)) {
    return false;
  }
  const auto response = client.Call(QStringLiteral("session.ping"), {}, kPingWaitMs);
  return response.has_value() && !response->error.has_value();
}

/// Reads the session directory and adds the ping check to the process check.
auto ReadSessionStates(const QString& session_dir) -> std::vector<AutomationSessionFileEntry> {
  std::vector<AutomationSessionFileEntry> entries =
      automation::ReadAutomationSessionFiles(session_dir);
  for (AutomationSessionFileEntry& entry : entries) {
    if (!entry.stale && entry.file->protocol_version == automation::kAutomationProtocolVersion) {
      entry.stale = !SessionAnswersPing(*entry.file);
    }
  }
  return entries;
}

auto IsUsable(const AutomationSessionFileEntry& entry) -> bool {
  return !entry.stale && entry.file->protocol_version == automation::kAutomationProtocolVersion;
}

auto ProtocolMismatchMessage(const AutomationSessionFile& file) -> QString {
  return QStringLiteral(
             "session '%1' uses protocol version %2; this alcedo-cli supports "
             "version %3")
      .arg(file.session_name)
      .arg(file.protocol_version)
      .arg(automation::kAutomationProtocolVersion);
}

/// Selects the session in the order --session, ALCEDO_SESSION, the only live session.
auto SelectSession(const CliOptions& options, QString* error)
    -> std::optional<AutomationSessionFile> {
  if (!options.session_name.isEmpty()) {
    if (!automation::IsValidAutomationSessionName(options.session_name)) {
      *error = QStringLiteral("invalid session name '%1'").arg(options.session_name);
      return std::nullopt;
    }
    QString    read_error;
    const auto file = automation::ReadAutomationSessionFile(
        automation::AutomationSessionFilePath(options.session_dir, options.session_name),
        &read_error);
    if (!file.has_value()) {
      *error = QStringLiteral("no session named '%1' in '%2' (%3)")
                   .arg(options.session_name, options.session_dir, read_error);
      return std::nullopt;
    }
    if (file->protocol_version != automation::kAutomationProtocolVersion) {
      *error = ProtocolMismatchMessage(*file);
      return std::nullopt;
    }
    if (!automation::IsAutomationProcessRunning(file->pid)) {
      *error = QStringLiteral("session '%1' is stale: process %2 does not run")
                   .arg(file->session_name)
                   .arg(file->pid);
      return std::nullopt;
    }
    return file;
  }

  std::vector<AutomationSessionFile> live;
  for (const AutomationSessionFileEntry& entry : ReadSessionStates(options.session_dir)) {
    if (IsUsable(entry)) {
      live.push_back(*entry.file);
    }
  }
  if (live.empty()) {
    *error = QStringLiteral("no live session in '%1'").arg(options.session_dir);
    return std::nullopt;
  }
  if (live.size() > 1) {
    QStringList names;
    for (const AutomationSessionFile& file : live) {
      names.push_back(file.session_name);
    }
    *error = QStringLiteral("several live sessions; select one with --session: %1")
                 .arg(names.join(QStringLiteral(", ")));
    return std::nullopt;
  }
  return live.front();
}

auto PrintResponse(const CliOptions& options, const AutomationResponse& response) -> int {
  if (response.error.has_value()) {
    const auto& error = *response.error;
    if (options.json) {
      QJsonObject object{{"code", static_cast<int>(error.code)}, {"message", error.message}};
      if (!error.data.isUndefined()) {
        object.insert("data", error.data);
      }
      WriteOut(ToJsonText(QJsonObject{{"error", object}}, QJsonDocument::Compact) + '\n');
    } else {
      QString text = QStringLiteral("error %1 %2: %3\n")
                         .arg(static_cast<int>(error.code))
                         .arg(automation::AutomationErrorName(error.code), error.message);
      if (!error.data.isUndefined()) {
        text += ToJsonText(error.data, QJsonDocument::Indented) + '\n';
      }
      WriteErr(text);
    }
    return Exit(CliExitCode::CommandError);
  }

  WriteOut(
      ToJsonText(response.result, options.json ? QJsonDocument::Compact : QJsonDocument::Indented) +
      '\n');
  return Exit(CliExitCode::Success);
}

/// Selects the session, connects, sends one command, and prints the response.
auto CallSession(const CliOptions& options, const QString& method, QJsonObject params,
                 const std::function<int(const AutomationResponse&)>& print) -> int {
  QString    error;
  const auto session = SelectSession(options, &error);
  if (!session.has_value()) {
    WriteErr(QStringLiteral("alcedo-cli: %1\n").arg(error));
    return Exit(CliExitCode::SessionSelectionError);
  }

  if (options.timeout_ms.has_value() && !params.contains("timeout_ms")) {
    params.insert("timeout_ms", *options.timeout_ms);
  }
  const int timeout_ms = params.value("timeout_ms").toInt(options.timeout_ms.value_or(120000));

  AutomationClient client;
  if (!client.Connect(session->socket_name, kConnectTimeoutMs)) {
    WriteErr(QStringLiteral("alcedo-cli: %1\n").arg(client.error_string()));
    return Exit(CliExitCode::ConnectionError);
  }
  const auto response = client.Call(method, params, timeout_ms + kResponseWaitMarginMs);
  if (!response.has_value()) {
    WriteErr(QStringLiteral("alcedo-cli: socket '%1': %2\n")
                 .arg(session->socket_name, client.error_string()));
    return Exit(CliExitCode::ConnectionError);
  }
  return print(*response);
}

auto RunCall(const CliOptions& options) -> int {
  if (options.positional.size() < 2 || options.positional.size() > 3) {
    return UsageFailure(QStringLiteral("call needs <method> and optional <params-json>"));
  }
  QJsonObject params;
  if (options.positional.size() == 3) {
    QJsonParseError     parse_error{};
    const QJsonDocument document =
        QJsonDocument::fromJson(options.positional.at(2).toUtf8(), &parse_error);
    if (parse_error.error != QJsonParseError::NoError || !document.isObject()) {
      return UsageFailure(QStringLiteral("<params-json> must be a JSON object"));
    }
    params = document.object();
  }
  return CallSession(
      options, options.positional.at(1), params,
      [&options](const AutomationResponse& response) { return PrintResponse(options, response); });
}

auto RunSchema(const CliOptions& options) -> int {
  if (options.positional.size() != 1) {
    return UsageFailure(QStringLiteral("schema takes no arguments"));
  }
  return CallSession(
      options, QStringLiteral("session.describe"), {},
      [&options](const AutomationResponse& response) {
        if (response.error.has_value() || options.json) {
          AutomationResponse commands = response;
          if (!response.error.has_value()) {
            commands.result = response.result.toObject().value("commands");
          }
          return PrintResponse(options, commands);
        }
        QString text;
        for (const QJsonValue& command : response.result.toObject().value("commands").toArray()) {
          const QJsonObject object = command.toObject();
          text +=
              QStringLiteral("%1%2\n    %3\n")
                  .arg(object.value("method").toString(),
                       object.value("changes_state").toBool() ? QStringLiteral(" (changes state)")
                                                              : QString(),
                       object.value("description").toString());
        }
        WriteOut(text);
        return Exit(CliExitCode::Success);
      });
}

auto SessionEntryState(const AutomationSessionFileEntry& entry) -> QString {
  if (!entry.file.has_value()) {
    return QStringLiteral("unreadable");
  }
  if (entry.file->protocol_version != automation::kAutomationProtocolVersion) {
    return QStringLiteral("unsupported-protocol");
  }
  return entry.stale ? QStringLiteral("stale") : QStringLiteral("live");
}

auto RunSessionList(const CliOptions& options) -> int {
  const auto entries = ReadSessionStates(options.session_dir);
  if (options.json) {
    QJsonArray sessions;
    for (const AutomationSessionFileEntry& entry : entries) {
      QJsonObject object{{"path", entry.path}, {"state", SessionEntryState(entry)}};
      if (entry.file.has_value()) {
        object.insert("session_name", entry.file->session_name);
        object.insert("socket_name", entry.file->socket_name);
        object.insert("pid", static_cast<double>(entry.file->pid));
        object.insert("host_mode", entry.file->host_mode);
        object.insert("project_path", entry.file->project_path);
        object.insert("protocol_version", entry.file->protocol_version);
      } else {
        object.insert("error", entry.read_error);
      }
      sessions.push_back(object);
    }
    WriteOut(ToJsonText(sessions, QJsonDocument::Compact) + '\n');
    return Exit(CliExitCode::Success);
  }

  if (entries.empty()) {
    WriteOut(QStringLiteral("no session files in '%1'\n").arg(options.session_dir));
    return Exit(CliExitCode::Success);
  }
  QString text;
  for (const AutomationSessionFileEntry& entry : entries) {
    if (entry.file.has_value()) {
      text += QStringLiteral("%1\t%2\t%3\tpid %4\t%5\n")
                  .arg(entry.file->session_name, SessionEntryState(entry), entry.file->host_mode)
                  .arg(entry.file->pid)
                  .arg(entry.file->project_path);
    } else {
      text += QStringLiteral("%1\tunreadable\t%2\n").arg(entry.path, entry.read_error);
    }
  }
  WriteOut(text);
  return Exit(CliExitCode::Success);
}

auto RunSessionPrune(const CliOptions& options) -> int {
  QJsonArray removed;
  QString    text;
  for (const AutomationSessionFileEntry& entry : ReadSessionStates(options.session_dir)) {
    if (!entry.stale) {
      continue;
    }
    if (QFile::remove(entry.path)) {
      removed.push_back(entry.path);
      text += QStringLiteral("removed %1\n").arg(entry.path);
    } else {
      WriteErr(QStringLiteral("alcedo-cli: cannot remove '%1'\n").arg(entry.path));
    }
  }
  if (options.json) {
    WriteOut(ToJsonText(QJsonObject{{"removed", removed}}, QJsonDocument::Compact) + '\n');
  } else {
    WriteOut(text.isEmpty() ? QStringLiteral("no stale session files\n") : text);
  }
  return Exit(CliExitCode::Success);
}

auto RunWatch(const CliOptions& options) -> int {
  if (options.positional.size() != 1) {
    return UsageFailure(QStringLiteral("watch takes no arguments"));
  }
  QString    error;
  const auto session = SelectSession(options, &error);
  if (!session.has_value()) {
    WriteErr(QStringLiteral("alcedo-cli: %1\n").arg(error));
    return Exit(CliExitCode::SessionSelectionError);
  }
  AutomationClient client;
  if (!client.Connect(session->socket_name, kConnectTimeoutMs)) {
    WriteErr(QStringLiteral("alcedo-cli: %1\n").arg(client.error_string()));
    return Exit(CliExitCode::ConnectionError);
  }

  while (true) {
    const auto notification = client.ReadNotification(-1);
    if (!notification.has_value()) {
      WriteErr(QStringLiteral("alcedo-cli: socket '%1': %2\n")
                   .arg(session->socket_name, client.error_string()));
      return Exit(CliExitCode::ConnectionError);
    }
    if (options.json) {
      WriteOut(QString::fromUtf8(automation::SerializeAutomationMessage(*notification)));
    } else {
      WriteOut(QStringLiteral("%1 %2\n").arg(
          notification->method, ToJsonText(notification->params, QJsonDocument::Compact)));
    }
  }
}

}  // namespace

auto RunAlcedoCli(const QStringList& arguments) -> int {
  CliOptions options;
  QString    error;
  if (!ParseOptions(arguments, &options, &error)) {
    return UsageFailure(error);
  }
  if (options.positional.isEmpty()) {
    return UsageFailure(QStringLiteral("no command"));
  }

  const QString command = options.positional.first();
  if (command == QStringLiteral("help") || command == QStringLiteral("--help") ||
      command == QStringLiteral("-h")) {
    WriteOut(QString::fromUtf8(kUsage));
    return Exit(CliExitCode::Success);
  }
  if (command == QStringLiteral("call")) {
    return RunCall(options);
  }
  if (command == QStringLiteral("schema")) {
    return RunSchema(options);
  }
  if (command == QStringLiteral("watch")) {
    return RunWatch(options);
  }
  if (command == QStringLiteral("session")) {
    const QString verb = options.positional.value(1);
    if (options.positional.size() == 2 && verb == QStringLiteral("list")) {
      return RunSessionList(options);
    }
    if (options.positional.size() == 2 && verb == QStringLiteral("prune")) {
      return RunSessionPrune(options);
    }
    return UsageFailure(QStringLiteral("unknown session command '%1'").arg(verb));
  }
  return UsageFailure(QStringLiteral("unknown command '%1'").arg(command));
}

}  // namespace alcedo::cli
