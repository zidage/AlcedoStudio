//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// Runs the real alcedo-cli program against automation servers in this test process. The test
// pumps its event loop while the program runs, so the servers can answer.

#include <gtest/gtest.h>

#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QTemporaryDir>
#include <QThread>
#include <algorithm>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <csignal>

extern char** environ;
#endif

#include "automation/automation_session_file.hpp"
#include "ui/alcedo_main/automation/automation_command_registry.hpp"
#include "ui/alcedo_main/automation/automation_server.hpp"
#include "ui/alcedo_main/automation/automation_session_commands.hpp"

#ifndef ALCEDO_CLI_PATH
#error "ALCEDO_CLI_PATH must name the alcedo-cli program"
#endif
#ifndef ALCEDO_CLI_TEST_HOST_PATH
#error "ALCEDO_CLI_TEST_HOST_PATH must name the alcedo_cli_test_host program"
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

/// Runs alcedo-cli with stdout and stderr on one operating-system pipe, as a shell does for
/// `out=$(alcedo-cli ...)`, and reads that pipe. QProcess is not used here, because it reports the
/// exit of the process and not the end of file on its output pipe.
class PipedCliProcess {
 public:
  PipedCliProcess()                                  = default;
  PipedCliProcess(const PipedCliProcess&)            = delete;
  PipedCliProcess& operator=(const PipedCliProcess&) = delete;
  ~PipedCliProcess() {
#if defined(_WIN32)
    if (process_ != nullptr) {
      if (WaitForSingleObject(process_, 0) == WAIT_TIMEOUT) {
        TerminateProcess(process_, 1);
      }
      CloseHandle(process_);
    }
    if (read_end_ != nullptr) {
      CloseHandle(read_end_);
    }
#else
    if (pid_ > 0 && !exited_) {
      ::kill(pid_, SIGKILL);
      ::waitpid(pid_, nullptr, 0);
    }
    if (read_end_ >= 0) {
      ::close(read_end_);
    }
#endif
  }

  /// Starts alcedo-cli with @p arguments. Only this object keeps the read end of the pipe.
  auto Start(const QStringList& arguments) -> bool {
#if defined(_WIN32)
    SECURITY_ATTRIBUTES inheritable{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE              write_end = nullptr;
    if (CreatePipe(&read_end_, &write_end, &inheritable, 0) == FALSE ||
        SetHandleInformation(read_end_, HANDLE_FLAG_INHERIT, 0) == FALSE) {
      return false;
    }
    HANDLE  null_input = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                     &inheritable, OPEN_EXISTING, 0, nullptr);
    QString command_line =
        QStringLiteral("\"%1\"").arg(QDir::toNativeSeparators(QStringLiteral(ALCEDO_CLI_PATH)));
    for (const QString& argument : arguments) {
      command_line += QStringLiteral(" \"%1\"").arg(argument);
    }
    std::wstring        command = command_line.toStdWString();
    STARTUPINFOW        startup{};
    PROCESS_INFORMATION information{};
    startup.cb         = sizeof(startup);
    startup.dwFlags    = STARTF_USESTDHANDLES;
    startup.hStdInput  = null_input;
    startup.hStdOutput = write_end;
    startup.hStdError  = write_end;
    const BOOL started = CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE,
                                        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &information);
    CloseHandle(write_end);
    CloseHandle(null_input);
    if (started == FALSE) {
      return false;
    }
    CloseHandle(information.hThread);
    process_ = information.hProcess;
    return true;
#else
    int pipe_ends[2] = {-1, -1};
    if (::pipe(pipe_ends) != 0) {
      return false;
    }
    read_end_ = pipe_ends[0];
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_adddup2(&actions, pipe_ends[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, pipe_ends[1], STDERR_FILENO);
    posix_spawn_file_actions_addclose(&actions, pipe_ends[0]);
    posix_spawn_file_actions_addclose(&actions, pipe_ends[1]);
    std::vector<QByteArray> words{QByteArray(ALCEDO_CLI_PATH)};
    for (const QString& argument : arguments) {
      words.push_back(argument.toLocal8Bit());
    }
    std::vector<char*> argv;
    for (QByteArray& word : words) {
      argv.push_back(word.data());
    }
    argv.push_back(nullptr);
    const int spawned =
        posix_spawn(&pid_, ALCEDO_CLI_PATH, &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    ::close(pipe_ends[1]);
    return spawned == 0;
#endif
  }

  /// Waits for the exit of alcedo-cli. Returns its exit code, or -1 when it did not exit within
  /// @p timeout_ms.
  auto WaitForExit(int timeout_ms) -> int {
#if defined(_WIN32)
    DWORD exit_code = 0;
    if (WaitForSingleObject(process_, static_cast<DWORD>(timeout_ms)) != WAIT_OBJECT_0 ||
        GetExitCodeProcess(process_, &exit_code) == FALSE) {
      return -1;
    }
    return static_cast<int>(exit_code);
#else
    QElapsedTimer timer;
    timer.start();
    int status = 0;
    while (::waitpid(pid_, &status, WNOHANG) == 0) {
      if (timer.elapsed() > timeout_ms) {
        return -1;
      }
      QThread::msleep(10);
    }
    exited_ = true;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
  }

  /// Reads the pipe until end of file. Returns false when end of file did not arrive within
  /// @p timeout_ms, which means that another process still holds the write end.
  auto ReadUntilEndOfFile(int timeout_ms) -> bool {
    QElapsedTimer timer;
    timer.start();
    char buffer[4096];
    while (timer.elapsed() <= timeout_ms) {
#if defined(_WIN32)
      DWORD available = 0;
      if (PeekNamedPipe(read_end_, nullptr, 0, nullptr, &available, nullptr) == FALSE) {
        return GetLastError() == ERROR_BROKEN_PIPE;
      }
      if (available == 0) {
        QThread::msleep(10);
        continue;
      }
      DWORD read = 0;
      if (ReadFile(read_end_, buffer, sizeof(buffer), &read, nullptr) == FALSE) {
        return GetLastError() == ERROR_BROKEN_PIPE;
      }
      output_.append(buffer, static_cast<qsizetype>(read));
#else
      pollfd descriptor{read_end_, POLLIN, 0};
      if (::poll(&descriptor, 1, 10) <= 0) {
        continue;
      }
      const ssize_t read = ::read(read_end_, buffer, sizeof(buffer));
      if (read == 0) {
        return true;
      }
      if (read < 0) {
        return false;
      }
      output_.append(buffer, static_cast<qsizetype>(read));
#endif
    }
    return false;
  }

  [[nodiscard]] auto output() const -> const QByteArray& { return output_; }

 private:
#if defined(_WIN32)
  HANDLE process_  = nullptr;
  HANDLE read_end_ = nullptr;
#else
  pid_t pid_      = -1;
  bool  exited_   = false;
  int   read_end_ = -1;
#endif
  QByteArray output_;
};

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

TEST_F(AlcedoCliTest, VersionPrintsApplicationVersionWithoutSession) {
  for (const QString& command : {QStringLiteral("--version"), QStringLiteral("version")}) {
    const CliRun run = RunCli({command});

    EXPECT_EQ(run.exit_code, 0) << run.stderr_text.toStdString();
    EXPECT_EQ(run.stdout_text.trimmed(), QStringLiteral(ALCEDO_APP_VERSION));
  }
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

TEST_F(AlcedoCliTest, BatchAnswersEachStdinLineInOrderOverOneConnection) {
  TestSession& session = StartSession(QStringLiteral("default"));

  QProcess     process;
  ASSERT_TRUE(StartCli(process, {"batch"}));
  // The first request is answered before the next is written, as a script that reads each
  // answer before it decides the next request does it.
  process.write(R"({"id":"first","method":"session.ping"})"
                "\n");
  QString output;
  ASSERT_TRUE(PumpUntil([&]() {
    output += QString::fromUtf8(process.readAllStandardOutput());
    return output.contains('\n');
  }));
  EXPECT_EQ(session.server->connection_count(), 1);
  process.write("\n");
  process.write(R"({"method":"test.reject","params":{}})"
                "\n");
  process.write("not json\n");
  process.write(R"({"id":7,"method":"session.ping","params":{}})"
                "\n");
  process.closeWriteChannel();
  int        max_connections = session.server->connection_count();
  const bool exited          = PumpUntil([&]() {
    max_connections = std::max(max_connections, session.server->connection_count());
    return process.state() == QProcess::NotRunning;
  });
  ASSERT_TRUE(exited);
  output += QString::fromUtf8(process.readAllStandardOutput());

  EXPECT_EQ(process.exitCode(), 1) << "a command error makes the batch exit with 1";
  EXPECT_EQ(max_connections, 1);
  QStringList lines = output.trimmed().split('\n');
  for (QString& line : lines) {
    line = line.trimmed();  // stdout is in text mode on Windows: lines end with \r\n.
  }
  ASSERT_EQ(lines.size(), 4) << output.toStdString();
  EXPECT_EQ(lines.at(0), QStringLiteral(R"({"id":"first","result":{"pong":true}})"));
  const QJsonObject rejected = QJsonDocument::fromJson(lines.at(1).toUtf8()).object();
  EXPECT_EQ(rejected.value("id"), QJsonValue(3)) << "without an id, the line number";
  EXPECT_EQ(rejected.value("error").toObject().value("code"), QJsonValue(-32002));
  const QJsonObject invalid = QJsonDocument::fromJson(lines.at(2).toUtf8()).object();
  EXPECT_EQ(invalid.value("id"), QJsonValue(4));
  EXPECT_EQ(invalid.value("error").toObject().value("code"), QJsonValue(-32600));
  EXPECT_EQ(lines.at(3), QStringLiteral(R"({"id":7,"result":{"pong":true}})"));
}

TEST_F(AlcedoCliTest, BatchWithOnlySuccessfulCommandsExitsZero) {
  StartSession(QStringLiteral("default"));

  QProcess process;
  ASSERT_TRUE(StartCli(process, {"batch"}));
  process.write(R"({"method":"session.ping"})"
                "\n"
                R"({"method":"session.ping"})"
                "\n");
  process.closeWriteChannel();
  ASSERT_TRUE(PumpUntil([&]() { return process.state() == QProcess::NotRunning; }));

  EXPECT_EQ(process.exitCode(), 0) << process.readAllStandardError().toStdString();
  EXPECT_EQ(QString::fromUtf8(process.readAllStandardOutput()).trimmed().split('\n').size(), 2);
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

TEST_F(AlcedoCliTest, PruneRemovesOnlyStaleSessionRecordsAndReportsSkippedFiles) {
  StartSession(QStringLiteral("live"));
  WriteSessionFile(QStringLiteral("silent"), QStringLiteral("alcedo-cli-test-no-listener"),
                   QCoreApplication::applicationPid());
  QProcess exited;
  exited.start(QCoreApplication::applicationFilePath(), {QStringLiteral("--gtest_list_tests")});
  ASSERT_TRUE(exited.waitForStarted(10000));
  const qint64 dead_pid = exited.processId();
  ASSERT_TRUE(exited.waitForFinished(30000));
  WriteSessionFile(QStringLiteral("dead"), QStringLiteral("alcedo-cli-test-dead"), dead_pid);

  // Files that are not session records. Some of them name the dead process, so a reader that
  // accepts them would find them stale.
  const QJsonObject partial_record{{"protocol_version", 1},
                                   {"session_name", "partial"},
                                   {"pid", static_cast<double>(dead_pid)},
                                   {"host_mode", "headless"}};
  // A complete record in a file whose name is not `<session_name>.json`.
  const QJsonObject renamed_record{{"protocol_version", 1},
                                   {"session_name", "other"},
                                   {"socket_name", "alcedo-cli-test-dead"},
                                   {"pid", static_cast<double>(dead_pid)},
                                   {"host_mode", "headless"},
                                   {"project_path", ""},
                                   {"started_at", "2026-10-08T09:00:00Z"}};
  const QByteArray  dead_pid_text = QByteArray::number(dead_pid);
  using NamedFileBytes            = std::vector<std::pair<QString, QByteArray>>;
  const NamedFileBytes kept_files = {
      {"user-settings.json", R"({"theme":"dark","valuable":"keep"})"},
      {"malformed.json", R"({"pid": )" + dead_pid_text + ","},
      {"array.json", "[" + dead_pid_text + "]"},
      {"partial.json", QJsonDocument(partial_record).toJson(QJsonDocument::Compact)},
      {"renamed.json", QJsonDocument(renamed_record).toJson(QJsonDocument::Compact)},
  };
  for (const auto& [name, bytes] : kept_files) {
    QFile output(session_dir_.filePath(name));
    ASSERT_TRUE(output.open(QIODevice::WriteOnly));
    ASSERT_EQ(output.write(bytes), bytes.size());
  }
  const auto file_names = [](const QJsonArray& values, const QString& key) {
    QStringList names;
    for (const QJsonValue& value : values) {
      names.push_back(
          QFileInfo(key.isEmpty() ? value.toString() : value.toObject().value(key).toString())
              .fileName());
    }
    names.sort();
    return names;
  };

  const CliRun prune = RunCli({"--json", "session", "prune"});

  ASSERT_EQ(prune.exit_code, 0) << prune.stderr_text.toStdString();
  const QJsonObject result = QJsonDocument::fromJson(prune.stdout_text.toUtf8()).object();
  EXPECT_EQ(file_names(result.value("removed").toArray(), {}),
            (QStringList{"dead.json", "silent.json"}));
  EXPECT_EQ(file_names(result.value("skipped").toArray(), QStringLiteral("path")),
            (QStringList{"array.json", "malformed.json", "partial.json", "renamed.json",
                         "user-settings.json"}));
  for (const QJsonValue& skipped : result.value("skipped").toArray()) {
    EXPECT_FALSE(skipped.toObject().value("reason").toString().isEmpty());
  }
  EXPECT_FALSE(QFile::exists(session_dir_.filePath("dead.json")));
  EXPECT_FALSE(QFile::exists(session_dir_.filePath("silent.json")));
  EXPECT_TRUE(QFile::exists(session_dir_.filePath("live.json")));
  for (const auto& [name, bytes] : kept_files) {
    QFile input(session_dir_.filePath(name));
    ASSERT_TRUE(input.open(QIODevice::ReadOnly)) << name.toStdString();
    EXPECT_EQ(input.readAll(), bytes) << name.toStdString();
  }

  const CliRun text_prune = RunCli({"session", "prune"});
  EXPECT_EQ(text_prune.exit_code, 0) << text_prune.stderr_text.toStdString();
  const QStringList lines = text_prune.stdout_text.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
  ASSERT_EQ(lines.size(), 1 + static_cast<qsizetype>(kept_files.size()))
      << text_prune.stdout_text.toStdString();
  EXPECT_EQ(lines.front().trimmed(), QStringLiteral("no stale session files"));
  for (qsizetype i = 1; i < lines.size(); ++i) {
    EXPECT_TRUE(lines.at(i).startsWith(QStringLiteral("skipped "))) << lines.at(i).toStdString();
  }
  EXPECT_TRUE(text_prune.stdout_text.contains(QStringLiteral("user-settings.json")));
  EXPECT_TRUE(QFile::exists(session_dir_.filePath("user-settings.json")));
}

TEST_F(AlcedoCliTest, UsageErrorsExitFour) {
  EXPECT_EQ(RunCli({}).exit_code, 4);
  EXPECT_EQ(RunCli({"call"}).exit_code, 4);
  EXPECT_EQ(RunCli({"call", "session.ping", "[1]"}).exit_code, 4);
  EXPECT_EQ(RunCli({"--timeout", "zero", "schema"}).exit_code, 4);
  EXPECT_EQ(RunCli({"session", "explode"}).exit_code, 4);
}

// Issue #354: a caller that reads the output of `session start` through a pipe must read end of
// file when alcedo-cli exits, not when the session host exits.
TEST_F(AlcedoCliTest, SessionStartOutputPipeClosesWhenTheCliExits) {
  PipedCliProcess cli;
  ASSERT_TRUE(cli.Start({"--session-dir", session_dir_.path(), "--session", "piped", "--json",
                         "session", "start", "--headless", "--create",
                         session_dir_.path() + QStringLiteral(",piped_project"), "--host-binary",
                         QStringLiteral(ALCEDO_CLI_TEST_HOST_PATH)}));
  const int  exit_code   = cli.WaitForExit(60000);
  const bool end_of_file = cli.ReadUntilEndOfFile(10000);
  ASSERT_EQ(exit_code, 0) << cli.output().toStdString();

  const QJsonObject started   = QJsonDocument::fromJson(cli.output()).object();
  const qint64      host_pid  = static_cast<qint64>(started.value("pid").toDouble());
  const bool        host_runs = IsAutomationProcessRunning(host_pid);
  // Stop the host before the checks, so that a failed check does not leave it running.
  const CliRun      stop      = RunCli({"--session", "piped", "session", "stop"});

  EXPECT_TRUE(end_of_file) << "the session host holds the output pipe of alcedo-cli";
  EXPECT_TRUE(host_runs) << cli.output().toStdString();
  EXPECT_EQ(started.value("session_name"), QJsonValue("piped"));
  EXPECT_EQ(stop.exit_code, 0) << stop.stderr_text.toStdString();
  EXPECT_FALSE(IsAutomationProcessRunning(host_pid));
}

}  // namespace
}  // namespace alcedo::automation
