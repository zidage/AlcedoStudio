//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "automation/automation_session_file.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <utility>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <sys/types.h>

#include <cerrno>
#include <csignal>
#endif

namespace alcedo::automation {
namespace {

void SetError(QString* error, QString message) {
  if (error != nullptr) {
    *error = std::move(message);
  }
}

auto ToJson(const AutomationSessionFile& file) -> QJsonObject {
  return QJsonObject{{"protocol_version", file.protocol_version},
                     {"session_name", file.session_name},
                     {"socket_name", file.socket_name},
                     {"pid", static_cast<double>(file.pid)},
                     {"host_mode", file.host_mode},
                     {"project_path", file.project_path},
                     {"started_at", file.started_at.toUTC().toString(Qt::ISODate)}};
}

}  // namespace

auto DefaultAutomationSessionDir() -> QString {
  return QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation))
      .filePath(QStringLiteral("automation/sessions"));
}

auto IsValidAutomationSessionName(const QString& name) -> bool {
  static const QRegularExpression kNamePattern(QStringLiteral("^[A-Za-z0-9._-]{1,64}$"));
  return kNamePattern.match(name).hasMatch() && name != QStringLiteral(".") &&
         name != QStringLiteral("..");
}

auto AutomationSessionFilePath(const QString& session_dir, const QString& session_name) -> QString {
  return QDir(session_dir).absoluteFilePath(session_name + QStringLiteral(".json"));
}

auto WriteAutomationSessionFile(const QString& session_dir, const AutomationSessionFile& file,
                                QString* error) -> bool {
  if (!IsValidAutomationSessionName(file.session_name)) {
    SetError(error, QStringLiteral("invalid session name '%1'").arg(file.session_name));
    return false;
  }
  if (!QDir().mkpath(session_dir)) {
    SetError(error, QStringLiteral("cannot create the session directory '%1'").arg(session_dir));
    return false;
  }

  QSaveFile output(AutomationSessionFilePath(session_dir, file.session_name));
  if (!output.open(QIODevice::WriteOnly)) {
    SetError(error,
             QStringLiteral("cannot write '%1': %2").arg(output.fileName(), output.errorString()));
    return false;
  }
  output.write(QJsonDocument(ToJson(file)).toJson(QJsonDocument::Indented));
  if (!output.commit()) {
    SetError(error,
             QStringLiteral("cannot write '%1': %2").arg(output.fileName(), output.errorString()));
    return false;
  }
  return true;
}

auto ReadAutomationSessionFile(const QString& path, QString* error)
    -> std::optional<AutomationSessionFile> {
  QFile input(path);
  if (!input.open(QIODevice::ReadOnly)) {
    SetError(error, QStringLiteral("cannot read '%1': %2").arg(path, input.errorString()));
    return std::nullopt;
  }

  QJsonParseError     parse_error{};
  const QJsonDocument document = QJsonDocument::fromJson(input.readAll(), &parse_error);
  if (parse_error.error != QJsonParseError::NoError || !document.isObject()) {
    SetError(error, QStringLiteral("'%1' is not a JSON object").arg(path));
    return std::nullopt;
  }

  const QJsonObject object = document.object();
  for (const char* key : {"protocol_version", "pid"}) {
    if (!object.value(QLatin1String(key)).isDouble()) {
      SetError(error, QStringLiteral("'%1' has no number '%2'").arg(path, QLatin1String(key)));
      return std::nullopt;
    }
  }
  for (const char* key : {"session_name", "socket_name", "host_mode"}) {
    if (object.value(QLatin1String(key)).toString().isEmpty()) {
      SetError(error, QStringLiteral("'%1' has no string '%2'").arg(path, QLatin1String(key)));
      return std::nullopt;
    }
  }

  AutomationSessionFile file;
  file.protocol_version = object.value("protocol_version").toInt();
  file.session_name     = object.value("session_name").toString();
  file.socket_name      = object.value("socket_name").toString();
  file.pid              = static_cast<qint64>(object.value("pid").toDouble());
  file.host_mode        = object.value("host_mode").toString();
  file.project_path     = object.value("project_path").toString();
  file.started_at       = QDateTime::fromString(object.value("started_at").toString(), Qt::ISODate);
  return file;
}

auto ReadAutomationSessionFiles(const QString& session_dir)
    -> std::vector<AutomationSessionFileEntry> {
  std::vector<AutomationSessionFileEntry> entries;
  const QDir                              dir(session_dir);
  const QFileInfoList                     files =
      dir.entryInfoList({QStringLiteral("*.json")}, QDir::Files, QDir::Name);
  for (const QFileInfo& info : files) {
    AutomationSessionFileEntry entry;
    entry.path  = info.absoluteFilePath();
    entry.file  = ReadAutomationSessionFile(entry.path, &entry.read_error);
    entry.stale = !entry.file.has_value() || !IsAutomationProcessRunning(entry.file->pid);
    entries.push_back(std::move(entry));
  }
  return entries;
}

auto RemoveAutomationSessionFile(const QString& session_dir, const QString& session_name) -> bool {
  if (!IsValidAutomationSessionName(session_name)) {
    return false;
  }
  const QString path = AutomationSessionFilePath(session_dir, session_name);
  return !QFileInfo::exists(path) || QFile::remove(path);
}

auto IsAutomationProcessRunning(qint64 pid) -> bool {
  if (pid <= 0) {
    return false;
  }
#if defined(_WIN32)
  HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
  if (process == nullptr) {
    // The process exists, but this user cannot query it.
    return GetLastError() == ERROR_ACCESS_DENIED;
  }
  DWORD      exit_code = 0;
  const BOOL queried   = GetExitCodeProcess(process, &exit_code);
  CloseHandle(process);
  return queried != FALSE && exit_code == STILL_ACTIVE;
#else
  return ::kill(static_cast<pid_t>(pid), 0) == 0 || errno == EPERM;
#endif
}

}  // namespace alcedo::automation
