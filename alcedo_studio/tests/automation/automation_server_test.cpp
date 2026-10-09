//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/automation/automation_server.hpp"

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QLocalSocket>
#include <memory>
#include <variant>
#include <vector>

#include "automation/automation_protocol.hpp"
#include "ui/alcedo_main/automation/automation_command_registry.hpp"
#include "ui/alcedo_main/automation/automation_session_commands.hpp"

namespace alcedo::automation {
namespace {

auto UniqueSocketName() -> QString {
  static int counter = 0;
  return QStringLiteral("alcedo-automation-test-%1-%2")
      .arg(QCoreApplication::applicationPid())
      .arg(++counter);
}

/// A real `QLocalSocket` client on the test thread. Reads pump the event loop so that the
/// server, which lives on the same thread, can answer.
class TestClient {
 public:
  auto Connect(const QString& socket_name) -> bool {
    socket_.connectToServer(socket_name);
    return WaitUntil([this]() { return socket_.state() == QLocalSocket::ConnectedState; });
  }

  void SendRaw(const QByteArray& bytes) { socket_.write(bytes); }

  void SendRequest(QJsonValue id, const QString& method, QJsonObject params = {}) {
    SendRaw(SerializeAutomationMessage(AutomationRequest{std::move(id), method, params}));
  }

  /// Reads @p count messages or fails after five seconds.
  auto ReadMessages(int count) -> std::vector<AutomationMessage> {
    std::vector<AutomationMessage> messages;
    WaitUntil([&]() {
      buffer_.append(socket_.readAll());
      qsizetype newline = 0;
      while ((newline = buffer_.indexOf('\n')) >= 0) {
        const AutomationParseResult parsed = ParseAutomationLine(buffer_.left(newline));
        buffer_.remove(0, newline + 1);
        EXPECT_FALSE(parsed.error.has_value());
        if (parsed.message.has_value()) {
          messages.push_back(*parsed.message);
        }
      }
      return static_cast<int>(messages.size()) >= count;
    });
    return messages;
  }

  auto WaitForDisconnect() -> bool {
    return WaitUntil([this]() { return socket_.state() == QLocalSocket::UnconnectedState; });
  }

  [[nodiscard]] auto state() const -> QLocalSocket::LocalSocketState { return socket_.state(); }

 private:
  template <typename Predicate>
  static auto WaitUntil(Predicate predicate) -> bool {
    QElapsedTimer timer;
    timer.start();
    while (!predicate()) {
      if (timer.elapsed() > 5000) {
        return false;
      }
      QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    }
    return true;
  }

  QLocalSocket socket_;
  QByteArray   buffer_;
};

class AutomationServerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(RegisterAutomationSessionCommands(
        registry_,
        AutomationSessionDescription{QStringLiteral("headless"), QStringLiteral("test")}));
    server_ = std::make_unique<AutomationServer>(registry_);
    QString error;
    ASSERT_TRUE(server_->Listen(UniqueSocketName(), &error)) << error.toStdString();
  }

  void                              TearDown() override { server_.reset(); }

  AutomationCommandRegistry         registry_;
  std::unique_ptr<AutomationServer> server_;
};

auto AsResponse(const AutomationMessage& message) -> const AutomationResponse& {
  return std::get<AutomationResponse>(message);
}

TEST_F(AutomationServerTest, PingRoundTripsOverLocalSocket) {
  TestClient client;
  ASSERT_TRUE(client.Connect(server_->socket_name()));

  client.SendRequest(QJsonValue("ping-1"), QStringLiteral("session.ping"));
  const auto messages = client.ReadMessages(1);

  ASSERT_EQ(messages.size(), 1U);
  const AutomationResponse& response = AsResponse(messages[0]);
  EXPECT_EQ(response.id, QJsonValue("ping-1"));
  EXPECT_FALSE(response.error.has_value());
  EXPECT_EQ(response.result, QJsonValue(QJsonObject{{"pong", true}}));
}

TEST_F(AutomationServerTest, NotificationReachesAllConnections) {
  TestClient first;
  TestClient second;
  ASSERT_TRUE(first.Connect(server_->socket_name()));
  ASSERT_TRUE(second.Connect(server_->socket_name()));
  // A round trip on each connection proves that the server accepted both.
  first.SendRequest(QJsonValue(1), QStringLiteral("session.ping"));
  second.SendRequest(QJsonValue(1), QStringLiteral("session.ping"));
  ASSERT_EQ(first.ReadMessages(1).size(), 1U);
  ASSERT_EQ(second.ReadMessages(1).size(), 1U);
  ASSERT_EQ(server_->connection_count(), 2);

  server_->SendNotification(QStringLiteral("task.finished"), QJsonObject{{"task_id", 9}});

  for (TestClient* client : {&first, &second}) {
    const auto messages = client->ReadMessages(1);
    ASSERT_EQ(messages.size(), 1U);
    const auto* notification = std::get_if<AutomationNotification>(&messages[0]);
    ASSERT_NE(notification, nullptr);
    EXPECT_EQ(notification->method, QStringLiteral("task.finished"));
    EXPECT_EQ(notification->params.value("task_id"), QJsonValue(9));
  }
}

TEST_F(AutomationServerTest, TwoRequestsOnOneLineBufferAreAnsweredInOrder) {
  TestClient client;
  ASSERT_TRUE(client.Connect(server_->socket_name()));

  client.SendRaw(SerializeAutomationMessage(
                     AutomationRequest{QJsonValue(1), QStringLiteral("session.describe"), {}}) +
                 SerializeAutomationMessage(
                     AutomationRequest{QJsonValue(2), QStringLiteral("session.ping"), {}}));
  const auto messages = client.ReadMessages(2);

  ASSERT_EQ(messages.size(), 2U);
  EXPECT_EQ(AsResponse(messages[0]).id, QJsonValue(1));
  EXPECT_EQ(AsResponse(messages[1]).id, QJsonValue(2));
  const QJsonArray commands = AsResponse(messages[0]).result.toObject().value("commands").toArray();
  EXPECT_EQ(commands.size(), 2);
}

TEST_F(AutomationServerTest, InvalidParamsAndBadLinesKeepConnectionOpen) {
  TestClient client;
  ASSERT_TRUE(client.Connect(server_->socket_name()));

  client.SendRequest(QJsonValue(1), QStringLiteral("session.ping"), QJsonObject{{"extra", 1}});
  client.SendRaw("not json\n");
  client.SendRaw(R"({"jsonrpc":"2.0","method":"session.ping"})"
                 "\n");
  client.SendRequest(QJsonValue(4), QStringLiteral("session.ping"));
  const auto messages = client.ReadMessages(4);

  ASSERT_EQ(messages.size(), 4U);
  EXPECT_EQ(AsResponse(messages[0]).error->code, AutomationErrorCode::InvalidParams);
  EXPECT_EQ(AsResponse(messages[0]).error->data.toObject().value("pointer"), QJsonValue("/extra"));
  EXPECT_EQ(AsResponse(messages[1]).error->code, AutomationErrorCode::ParseError);
  EXPECT_TRUE(AsResponse(messages[1]).id.isNull());
  EXPECT_EQ(AsResponse(messages[2]).error->code, AutomationErrorCode::InvalidRequest);
  EXPECT_FALSE(AsResponse(messages[3]).error.has_value());
  EXPECT_EQ(client.state(), QLocalSocket::ConnectedState);
}

TEST_F(AutomationServerTest, OversizedLineClosesConnectionWithParseError) {
  TestClient client;
  ASSERT_TRUE(client.Connect(server_->socket_name()));

  client.SendRaw(QByteArray(kMaxAutomationLineBytes + 1, 'x'));
  const auto messages = client.ReadMessages(1);

  ASSERT_EQ(messages.size(), 1U);
  EXPECT_EQ(AsResponse(messages[0]).error->code, AutomationErrorCode::ParseError);
  EXPECT_TRUE(client.WaitForDisconnect());
}

}  // namespace
}  // namespace alcedo::automation
