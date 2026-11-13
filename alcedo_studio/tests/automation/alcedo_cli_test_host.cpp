//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// A small session host for AlcedoCliTest. `alcedo-cli session start --host-binary` starts it in
// place of alcedo_main. It reads --session and --session-dir, ignores the other host options,
// answers session.ping and session.describe, and exits on session.shutdown.

#include <QCoreApplication>
#include <QJsonObject>
#include <QStringList>
#include <QTimer>
#include <cstdio>
#include <utility>

#include "automation/automation_session_file.hpp"
#include "ui/alcedo_main/automation/automation_command_registry.hpp"
#include "ui/alcedo_main/automation/automation_server.hpp"
#include "ui/alcedo_main/automation/automation_session_commands.hpp"

namespace {

using namespace alcedo::automation;

auto OptionValue(const QStringList& arguments, const QString& name) -> QString {
  const qsizetype index = arguments.indexOf(name);
  return index >= 0 && index + 1 < arguments.size() ? arguments.at(index + 1) : QString();
}

}  // namespace

int main(int argc, char** argv) {
  QCoreApplication  app(argc, argv);
  const QStringList arguments    = QCoreApplication::arguments();
  const QString     session_name = OptionValue(arguments, QStringLiteral("--session"));
  const QString     session_dir  = OptionValue(arguments, QStringLiteral("--session-dir"));
  if (session_name.isEmpty() || session_dir.isEmpty()) {
    std::fputs("alcedo_cli_test_host: needs --session and --session-dir\n", stderr);
    return 2;
  }

  AutomationCommandRegistry registry;
  QString                   error;
  if (!RegisterAutomationSessionCommands(
          registry,
          AutomationSessionDescription{QStringLiteral("headless"), QStringLiteral("test")},
          &error)) {
    std::fprintf(stderr, "alcedo_cli_test_host: %s\n", qPrintable(error));
    return 1;
  }
  AutomationCommandSpec shutdown;
  shutdown.method        = QStringLiteral("session.shutdown");
  shutdown.description   = QStringLiteral("Ends the test host process.");
  shutdown.changes_state = true;
  shutdown.params_schema = QJsonObject{{"type", "object"}, {"additionalProperties", false}};
  shutdown.result_schema = QJsonObject{{"type", "object"}};
  shutdown.handler       = [](const QJsonObject&, AutomationReply reply) {
    reply.SendResult(QJsonObject{{"shut_down", true}});
    QTimer::singleShot(0, QCoreApplication::instance(), &QCoreApplication::quit);
  };
  if (!registry.Register(std::move(shutdown), &error)) {
    std::fprintf(stderr, "alcedo_cli_test_host: %s\n", qPrintable(error));
    return 1;
  }

  AutomationServer server(registry);
  const QString    socket_name = QStringLiteral("alcedo-cli-test-host-%1-%2")
                                  .arg(QCoreApplication::applicationPid())
                                  .arg(session_name);
  if (!server.Listen(socket_name, &error)) {
    std::fprintf(stderr, "alcedo_cli_test_host: %s\n", qPrintable(error));
    return 1;
  }
  AutomationSessionFile file;
  file.protocol_version = 1;
  file.session_name     = session_name;
  file.socket_name      = socket_name;
  file.pid              = QCoreApplication::applicationPid();
  file.host_mode        = QStringLiteral("headless");
  file.started_at       = QDateTime::currentDateTimeUtc();
  if (!WriteAutomationSessionFile(session_dir, file, &error)) {
    std::fprintf(stderr, "alcedo_cli_test_host: %s\n", qPrintable(error));
    return 1;
  }

  const int exit_code = QCoreApplication::exec();
  server.Close();
  RemoveAutomationSessionFile(session_dir, session_name);
  return exit_code;
}
