//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// Runs the real alcedo-cli program against automation servers in this test process. The test
// pumps its event loop while the program runs, so the servers can answer.

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QTemporaryDir>
#include <memory>
#include <vector>

#include "automation/automation_session_file.hpp"
#include "ui/alcedo_main/automation/automation_command_registry.hpp"
#include "ui/alcedo_main/automation/automation_server.hpp"
#include "ui/alcedo_main/automation/automation_session_commands.hpp"

#ifndef ALCEDO_CLI_PATH
#error "ALCEDO_CLI_PATH must name the alcedo-cli program"
#endif

namespace alcedo::automation {
namespace {

struct CliRun {
  int     exit_code = -1;
  QString stdout_text;
  QString stderr_text;
};

template <typename Predicate>
auto PumpUntil(Predicate predicate, int timeout_ms = 30000) -> bool {
  QElapsedTimer timer;
  timer.start();
  while (!predicate()) {
    if (timer.elapsed() > timeout_ms) {
      return false;
    }
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
  }
  return true;
}

auto CliEnvironment() -> QProcessEnvironment {
  QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
  environment.remove(QStringLiteral("ALCEDO_SESSION"));
  return environment;
}

/// One in-process session: a registry, a listening server, and its session file.
struct TestSession {
  AutomationCommandRegistry         registry;
  std::unique_ptr<AutomationServer> server;
};

class AlcedoCliTest : public ::testing::Test {
 protected:
  void SetUp() override { ASSERT_TRUE(session_dir_.isValid()); }

  auto StartSession(const QString& name) -> TestSession& {
    auto session = std::make_unique<TestSession>();
    EXPECT_TRUE(RegisterAutomationSessionCommands(
        session->registry,
        AutomationSessionDescription{QStringLiteral("headless"), QStringLiteral("test")}));

    AutomationCommandSpec reject;
    reject.method        = QStringLiteral("test.reject");
    reject.description   = QStringLiteral("Always fails with the rejected code.");
    reject.params_schema = QJsonObject{{"type", "object"}, {"additionalProperties", false}};
    reject.result_schema = QJsonObject{{"type", "object"}};
    reject.handler       = [](const QJsonObject&, AutomationReply reply) {
      reply.SendError(AutomationErrorCode::Rejected, QStringLiteral("no project is open"),
                            QJsonObject{{"reason", "closed"}});
    };
    EXPECT_TRUE(session->registry.Register(std::move(reject)));

    session->server = std::make_unique<AutomationServer>(session->registry);
    const QString socket_name =
        QStringLiteral("alcedo-cli-test-%1-%2").arg(QCoreApplication::applicationPid()).arg(name);
    EXPECT_TRUE(session->server->Listen(socket_name));
    WriteSessionFile(name, socket_name, QCoreApplication::applicationPid());

    sessions_.push_back(std::move(session));
    return *sessions_.back();
  }

  void WriteSessionFile(const QString& name, const QString& socket_name, qint64 pid) {
    AutomationSessionFile file;
    file.protocol_version = 1;
    file.session_name     = name;
    file.socket_name      = socket_name;
    file.pid              = pid;
    file.host_mode        = QStringLiteral("headless");
    file.started_at       = QDateTime::currentDateTimeUtc();
    ASSERT_TRUE(WriteAutomationSessionFile(session_dir_.path(), file));
  }

  auto StartCli(QProcess& process, QStringList arguments) -> bool {
    arguments << QStringLiteral("--session-dir") << session_dir_.path();
    process.setProcessEnvironment(CliEnvironment());
    process.start(QStringLiteral(ALCEDO_CLI_PATH), arguments);
    return process.waitForStarted(10000);
  }

  auto RunCli(const QStringList& arguments) -> CliRun {
    QProcess process;
    CliRun   run;
    if (!StartCli(process, arguments)) {
      ADD_FAILURE() << "alcedo-cli did not start: " << process.errorString().toStdString();
      return run;
    }
    EXPECT_TRUE(PumpUntil([&]() { return process.state() == QProcess::NotRunning; }));
    run.exit_code   = process.exitCode();
    run.stdout_text = QString::fromUtf8(process.readAllStandardOutput());
    run.stderr_text = QString::fromUtf8(process.readAllStandardError());
    return run;
  }

  QTemporaryDir                             session_dir_;
  std::vector<std::unique_ptr<TestSession>> sessions_;
};

TEST_F(AlcedoCliTest, CallPrintsResultAndExitsZero) {
  StartSession(QStringLiteral("default"));

  const CliRun run = RunCli({"--json", "call", "session.ping", "{}"});

  EXPECT_EQ(run.exit_code, 0) << run.stderr_text.toStdString();
  EXPECT_EQ(run.stdout_text.trimmed(), QStringLiteral(R"({"pong":true})"));
}

TEST_F(AlcedoCliTest, AmbiguousSessionExitsTwoAndListsNames) {
  StartSession(QStringLiteral("alpha"));
  StartSession(QStringLiteral("beta"));

  const CliRun run = RunCli({"call", "session.ping"});

  EXPECT_EQ(run.exit_code, 2);
  EXPECT_TRUE(run.stderr_text.contains(QStringLiteral("alpha")));
  EXPECT_TRUE(run.stderr_text.contains(QStringLiteral("beta")));

  const CliRun named = RunCli({"--session", "beta", "--json", "call", "session.ping"});
  EXPECT_EQ(named.exit_code, 0) << named.stderr_text.toStdString();
}

TEST_F(AlcedoCliTest, CommandErrorExitsOneWithCode) {
  StartSession(QStringLiteral("default"));

  const CliRun json_run = RunCli({"--json", "call", "test.reject"});
  EXPECT_EQ(json_run.exit_code, 1);
  const QJsonObject error =
      QJsonDocument::fromJson(json_run.stdout_text.toUtf8()).object().value("error").toObject();
  EXPECT_EQ(error.value("code"), QJsonValue(-32002));
  EXPECT_EQ(error.value("data").toObject().value("reason"), QJsonValue("closed"));

  const CliRun text_run = RunCli({"call", "test.reject"});
  EXPECT_EQ(text_run.exit_code, 1);
  EXPECT_TRUE(text_run.stderr_text.contains(QStringLiteral("-32002 rejected")));
}

TEST_F(AlcedoCliTest, WatchPrintsNotification) {
  TestSession& session = StartSession(QStringLiteral("default"));

  QProcess     process;
  ASSERT_TRUE(StartCli(process, {"--session", "default", "watch"}));
  ASSERT_TRUE(PumpUntil([&]() { return session.server->connection_count() == 1; }));

  session.server->SendNotification(QStringLiteral("task.finished"),
                                   QJsonObject{{"task_id", 3}, {"state", "succeeded"}});
  QString    output;
  const bool printed = PumpUntil([&]() {
    output += QString::fromUtf8(process.readAllStandardOutput());
    return output.contains('\n');
  });
  process.kill();
  process.waitForFinished(10000);

  ASSERT_TRUE(printed);
  EXPECT_EQ(output.trimmed(), QStringLiteral(R"(task.finished {"state":"succeeded","task_id":3})"));
}

TEST_F(AlcedoCliTest, SchemaListsRegisteredCommands) {
  StartSession(QStringLiteral("default"));

  const CliRun run = RunCli({"--json", "schema"});

  EXPECT_EQ(run.exit_code, 0) << run.stderr_text.toStdString();
  const QJsonArray commands = QJsonDocument::fromJson(run.stdout_text.toUtf8()).array();
  QStringList      methods;
  for (const QJsonValue& command : commands) {
    methods.push_back(command.toObject().value("method").toString());
  }
  EXPECT_EQ(methods, (QStringList{"session.describe", "session.ping", "test.reject"}));
}

TEST_F(AlcedoCliTest, SilentSocketExitsThreeWithSocketName) {
  // The process runs, but nothing listens on the socket.
  WriteSessionFile(QStringLiteral("silent"), QStringLiteral("alcedo-cli-test-no-listener"),
                   QCoreApplication::applicationPid());

  const CliRun run = RunCli({"--session", "silent", "call", "session.ping"});

  EXPECT_EQ(run.exit_code, 3);
  EXPECT_TRUE(run.stderr_text.contains(QStringLiteral("alcedo-cli-test-no-listener")));
}

TEST_F(AlcedoCliTest, StaleSessionIsListedAndPruned) {
  StartSession(QStringLiteral("live"));
  WriteSessionFile(QStringLiteral("silent"), QStringLiteral("alcedo-cli-test-no-listener"),
                   QCoreApplication::applicationPid());

  const CliRun list = RunCli({"--json", "session", "list"});
  ASSERT_EQ(list.exit_code, 0) << list.stderr_text.toStdString();
  QStringList states;
  for (const QJsonValue& entry : QJsonDocument::fromJson(list.stdout_text.toUtf8()).array()) {
    states.push_back(entry.toObject().value("session_name").toString() + QLatin1Char('=') +
                     entry.toObject().value("state").toString());
  }
  EXPECT_EQ(states, (QStringList{"live=live", "silent=stale"}));

  const CliRun prune = RunCli({"session", "prune"});
  EXPECT_EQ(prune.exit_code, 0);
  EXPECT_FALSE(QFile::exists(session_dir_.filePath("silent.json")));
  EXPECT_TRUE(QFile::exists(session_dir_.filePath("live.json")));

  // With one live session left, the implicit selection uses it.
  EXPECT_EQ(RunCli({"call", "session.ping"}).exit_code, 0);
}

TEST_F(AlcedoCliTest, UsageErrorsExitFour) {
  EXPECT_EQ(RunCli({}).exit_code, 4);
  EXPECT_EQ(RunCli({"call"}).exit_code, 4);
  EXPECT_EQ(RunCli({"call", "session.ping", "[1]"}).exit_code, 4);
  EXPECT_EQ(RunCli({"--timeout", "zero", "schema"}).exit_code, 4);
  EXPECT_EQ(RunCli({"session", "explode"}).exit_code, 4);
}

}  // namespace
}  // namespace alcedo::automation
