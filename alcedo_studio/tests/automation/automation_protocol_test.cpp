//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "automation/automation_protocol.hpp"

#include <gtest/gtest.h>

#include <QJsonDocument>
#include <variant>

namespace alcedo::automation {
namespace {

TEST(AutomationProtocolTest, ParsesValidRequestLine) {
  const AutomationParseResult parsed = ParseAutomationLine(
      R"({"jsonrpc":"2.0","id":7,"method":"editor.set","params":{"field":"exposure","value":1}})");

  ASSERT_FALSE(parsed.error.has_value());
  ASSERT_TRUE(parsed.message.has_value());
  const auto* request = std::get_if<AutomationRequest>(&*parsed.message);
  ASSERT_NE(request, nullptr);
  EXPECT_EQ(request->id, QJsonValue(7));
  EXPECT_EQ(request->method, QStringLiteral("editor.set"));
  EXPECT_EQ(request->params.value("field"), QJsonValue("exposure"));
  EXPECT_EQ(request->params.value("value"), QJsonValue(1));
}

TEST(AutomationProtocolTest, RejectsLineWithoutJsonRpcVersion) {
  const AutomationParseResult parsed = ParseAutomationLine(R"({"id":"a","method":"session.ping"})");

  ASSERT_TRUE(parsed.error.has_value());
  EXPECT_EQ(parsed.error->code, AutomationErrorCode::InvalidRequest);
  // The id is readable, so the error response names the request.
  EXPECT_EQ(parsed.id, QJsonValue("a"));
}

TEST(AutomationProtocolTest, InvalidJsonGivesParseErrorWithNullId) {
  const AutomationParseResult parsed = ParseAutomationLine(R"({"jsonrpc":"2.0","id":1,)");

  ASSERT_TRUE(parsed.error.has_value());
  EXPECT_EQ(parsed.error->code, AutomationErrorCode::ParseError);
  EXPECT_TRUE(parsed.id.isNull());
}

TEST(AutomationProtocolTest, ArrayParamsGiveInvalidParams) {
  const AutomationParseResult parsed =
      ParseAutomationLine(R"({"jsonrpc":"2.0","id":3,"method":"session.ping","params":[1]})");

  ASSERT_TRUE(parsed.error.has_value());
  EXPECT_EQ(parsed.error->code, AutomationErrorCode::InvalidParams);
  EXPECT_EQ(parsed.id, QJsonValue(3));
}

TEST(AutomationProtocolTest, MethodWithoutIdParsesAsNotification) {
  const AutomationParseResult parsed = ParseAutomationLine(
      "{\"jsonrpc\":\"2.0\",\"method\":\"task.finished\",\"params\":{\"task_id\":4}}\r");

  ASSERT_FALSE(parsed.error.has_value());
  const auto* notification = std::get_if<AutomationNotification>(&*parsed.message);
  ASSERT_NE(notification, nullptr);
  EXPECT_EQ(notification->method, QStringLiteral("task.finished"));
  EXPECT_EQ(notification->params.value("task_id"), QJsonValue(4));
}

TEST(AutomationProtocolTest, ErrorResponseRoundTripsThroughSerialization) {
  const AutomationResponse sent = MakeAutomationErrorResponse(
      QJsonValue("req-1"), AutomationErrorCode::NotReady, QStringLiteral("no project is open"),
      QJsonObject{{"reason", "closed"}});

  const QByteArray line = SerializeAutomationMessage(sent);
  ASSERT_TRUE(line.endsWith('\n'));
  EXPECT_EQ(line.count('\n'), 1);

  const AutomationParseResult parsed = ParseAutomationLine(line.chopped(1));
  ASSERT_FALSE(parsed.error.has_value());
  const auto* received = std::get_if<AutomationResponse>(&*parsed.message);
  ASSERT_NE(received, nullptr);
  EXPECT_EQ(received->id, QJsonValue("req-1"));
  ASSERT_TRUE(received->error.has_value());
  EXPECT_EQ(received->error->code, AutomationErrorCode::NotReady);
  EXPECT_EQ(received->error->message, QStringLiteral("no project is open"));
  EXPECT_EQ(received->error->data, QJsonValue(QJsonObject{{"reason", "closed"}}));
  EXPECT_EQ(AutomationErrorName(received->error->code), QStringLiteral("not_ready"));
}

TEST(AutomationProtocolTest, SuccessResponseSerializesCompactResult) {
  AutomationResponse response;
  response.id     = QJsonValue(12);
  response.result = QJsonObject{{"pong", true}};

  EXPECT_EQ(SerializeAutomationMessage(response),
            QByteArray(R"({"id":12,"jsonrpc":"2.0","result":{"pong":true}})"
                       "\n"));
}

TEST(AutomationProtocolTest, UnknownIntegerIsNotAKnownErrorCode) {
  EXPECT_FALSE(AutomationErrorCodeFromInt(-31999).has_value());
  EXPECT_EQ(AutomationErrorCodeFromInt(-32004), AutomationErrorCode::ControlRevokedByUser);
}

}  // namespace
}  // namespace alcedo::automation
