//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/automation/automation_command_registry.hpp"

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QJsonArray>
#include <optional>
#include <vector>

#include "ui/alcedo_main/automation/automation_session_commands.hpp"

namespace alcedo::automation {
namespace {

auto RatingCommand() -> AutomationCommandSpec {
  AutomationCommandSpec spec;
  spec.method        = QStringLiteral("library.rate");
  spec.params_schema = QJsonObject{
      {"type", "object"},
      {"properties",
       QJsonObject{{"rating", QJsonObject{{"type", "integer"}, {"minimum", 0}, {"maximum", 5}}},
                   {"element_ids", QJsonObject{{"type", "array"},
                                               {"items", QJsonObject{{"type", "integer"}}},
                                               {"minItems", 1}}}}},
      {"required", QJsonArray{"rating", "element_ids"}},
      {"additionalProperties", false}};
  spec.result_schema = QJsonObject{{"type", "object"}};
  spec.changes_state = true;
  spec.handler       = [](const QJsonObject& params, AutomationReply reply) {
    reply.SendResult(QJsonObject{{"applied", params.value("element_ids").toArray().size()}});
  };
  return spec;
}

/// Collects every response that one dispatch sends.
struct ResponseLog {
  std::vector<AutomationResponse> responses;

  auto                            MakeReply(QJsonValue id) -> AutomationReply {
    return AutomationReply(std::move(id), [this](AutomationResponse response) {
      responses.push_back(std::move(response));
    });
  }
};

auto MakeRequest(QString method, QJsonObject params) -> AutomationRequest {
  return AutomationRequest{QJsonValue(1), std::move(method), std::move(params)};
}

TEST(AutomationCommandRegistryTest, RejectsUnknownParameterWithJsonPointer) {
  AutomationCommandRegistry registry;
  ASSERT_TRUE(registry.Register(RatingCommand()));

  ResponseLog log;
  registry.Dispatch(
      MakeRequest(QStringLiteral("library.rate"),
                  QJsonObject{{"rating", 3}, {"element_ids", QJsonArray{1}}, {"extra", true}}),
      log.MakeReply(QJsonValue(1)));

  ASSERT_EQ(log.responses.size(), 1U);
  ASSERT_TRUE(log.responses[0].error.has_value());
  EXPECT_EQ(log.responses[0].error->code, AutomationErrorCode::InvalidParams);
  EXPECT_EQ(log.responses[0].error->data.toObject().value("pointer"), QJsonValue("/extra"));
}

TEST(AutomationCommandRegistryTest, OutOfRangeValueReportsPointerAndBound) {
  AutomationCommandRegistry registry;
  ASSERT_TRUE(registry.Register(RatingCommand()));

  ResponseLog log;
  registry.Dispatch(MakeRequest(QStringLiteral("library.rate"),
                                QJsonObject{{"rating", 6}, {"element_ids", QJsonArray{1}}}),
                    log.MakeReply(QJsonValue(1)));

  ASSERT_EQ(log.responses.size(), 1U);
  ASSERT_TRUE(log.responses[0].error.has_value());
  EXPECT_EQ(log.responses[0].error->data.toObject().value("pointer"), QJsonValue("/rating"));
  EXPECT_TRUE(log.responses[0].error->message.contains(QStringLiteral("maximum 5")));
}

TEST(AutomationCommandRegistryTest, WrongItemTypeReportsArrayIndexPointer) {
  AutomationCommandRegistry registry;
  ASSERT_TRUE(registry.Register(RatingCommand()));

  ResponseLog log;
  registry.Dispatch(MakeRequest(QStringLiteral("library.rate"),
                                QJsonObject{{"rating", 1}, {"element_ids", QJsonArray{1, "two"}}}),
                    log.MakeReply(QJsonValue(1)));

  ASSERT_EQ(log.responses.size(), 1U);
  EXPECT_EQ(log.responses[0].error->data.toObject().value("pointer"), QJsonValue("/element_ids/1"));
}

TEST(AutomationCommandRegistryTest, MissingRequiredParameterIsReported) {
  AutomationCommandRegistry registry;
  ASSERT_TRUE(registry.Register(RatingCommand()));

  ResponseLog log;
  registry.Dispatch(MakeRequest(QStringLiteral("library.rate"), QJsonObject{{"rating", 1}}),
                    log.MakeReply(QJsonValue(1)));

  ASSERT_EQ(log.responses.size(), 1U);
  EXPECT_EQ(log.responses[0].error->data.toObject().value("pointer"), QJsonValue("/element_ids"));
}

TEST(AutomationCommandRegistryTest, ValidParametersReachTheHandler) {
  AutomationCommandRegistry registry;
  ASSERT_TRUE(registry.Register(RatingCommand()));

  ResponseLog log;
  registry.Dispatch(
      MakeRequest(
          QStringLiteral("library.rate"),
          QJsonObject{{"rating", 5}, {"element_ids", QJsonArray{4, 9}}, {"timeout_ms", 500}}),
      log.MakeReply(QJsonValue(1)));

  ASSERT_EQ(log.responses.size(), 1U);
  EXPECT_FALSE(log.responses[0].error.has_value());
  EXPECT_EQ(log.responses[0].result.toObject().value("applied"), QJsonValue(2));
}

TEST(AutomationCommandRegistryTest, UnknownMethodGivesMethodNotFound) {
  AutomationCommandRegistry registry;

  ResponseLog               log;
  registry.Dispatch(MakeRequest(QStringLiteral("editor.unknown"), {}),
                    log.MakeReply(QJsonValue(1)));

  ASSERT_EQ(log.responses.size(), 1U);
  EXPECT_EQ(log.responses[0].error->code, AutomationErrorCode::MethodNotFound);
}

TEST(AutomationCommandRegistryTest, DroppedReplySendsInternalError) {
  AutomationCommandRegistry registry;
  AutomationCommandSpec     spec = RatingCommand();
  spec.handler                   = [](const QJsonObject&, AutomationReply) {};
  ASSERT_TRUE(registry.Register(std::move(spec)));

  ResponseLog log;
  registry.Dispatch(MakeRequest(QStringLiteral("library.rate"),
                                QJsonObject{{"rating", 1}, {"element_ids", QJsonArray{1}}}),
                    log.MakeReply(QJsonValue(1)));

  ASSERT_EQ(log.responses.size(), 1U);
  ASSERT_TRUE(log.responses[0].error.has_value());
  EXPECT_EQ(log.responses[0].error->code, AutomationErrorCode::InternalError);
  EXPECT_EQ(log.responses[0].error->message, QStringLiteral("handler dropped the request"));
}

TEST(AutomationCommandRegistryTest, SecondSendAfterResultIsIgnored) {
  ResponseLog     log;
  AutomationReply reply = log.MakeReply(QJsonValue(5));
  reply.SendResult(QJsonObject{{"first", true}});
  reply.SendError(AutomationErrorCode::Failed, QStringLiteral("late"));
  AutomationReply moved = std::move(reply);
  moved                 = AutomationReply();

  ASSERT_EQ(log.responses.size(), 1U);
  EXPECT_FALSE(log.responses[0].error.has_value());
}

TEST(AutomationCommandRegistryTest, HandlerThatMissesTimeoutGetsTimeoutError) {
  AutomationCommandRegistry      registry;
  std::optional<AutomationReply> held_reply;
  AutomationCommandSpec          spec = RatingCommand();
  spec.handler                        = [&held_reply](const QJsonObject&, AutomationReply reply) {
    held_reply.emplace(std::move(reply));
  };
  ASSERT_TRUE(registry.Register(std::move(spec)));

  ResponseLog log;
  registry.Dispatch(
      MakeRequest(QStringLiteral("library.rate"),
                  QJsonObject{{"rating", 1}, {"element_ids", QJsonArray{1}}, {"timeout_ms", 20}}),
      log.MakeReply(QJsonValue(1)));

  QElapsedTimer timer;
  timer.start();
  while (log.responses.empty() && timer.elapsed() < 5000) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
  }
  ASSERT_EQ(log.responses.size(), 1U);
  EXPECT_EQ(log.responses[0].error->code, AutomationErrorCode::Timeout);

  // The operation finishes later. The client already has its one response.
  held_reply->SendResult(QJsonObject{});
  held_reply.reset();
  EXPECT_EQ(log.responses.size(), 1U);
}

TEST(AutomationCommandRegistryTest, DuplicateMethodRegistrationFails) {
  AutomationCommandRegistry registry;
  ASSERT_TRUE(registry.Register(RatingCommand()));

  QString error;
  EXPECT_FALSE(registry.Register(RatingCommand(), &error));
  EXPECT_TRUE(error.contains(QStringLiteral("already registered")));
}

TEST(AutomationCommandRegistryTest, OpenParameterSchemaRegistrationFails) {
  AutomationCommandRegistry registry;
  AutomationCommandSpec     spec = RatingCommand();
  spec.params_schema.remove("additionalProperties");

  QString error;
  EXPECT_FALSE(registry.Register(std::move(spec), &error));
  EXPECT_TRUE(error.contains(QStringLiteral("additionalProperties")));
}

TEST(AutomationCommandRegistryTest, UnsupportedSchemaKeywordRegistrationFails) {
  AutomationCommandRegistry registry;
  AutomationCommandSpec     spec = RatingCommand();
  spec.params_schema.insert(
      "properties",
      QJsonObject{{"name", QJsonObject{{"type", "string"}, {"pattern", "^[a-z]+$"}}}});

  QString error;
  EXPECT_FALSE(registry.Register(std::move(spec), &error));
  EXPECT_TRUE(error.contains(QStringLiteral("/properties/name/pattern")));
}

TEST(AutomationCommandRegistryTest, DescribeListsSessionCommandsWithSchemas) {
  AutomationCommandRegistry registry;
  ASSERT_TRUE(RegisterAutomationSessionCommands(
      registry, AutomationSessionDescription{QStringLiteral("headless"), QStringLiteral("1.0")}));

  const QJsonArray commands = registry.Describe();
  ASSERT_EQ(commands.size(), 2);
  EXPECT_EQ(commands[0].toObject().value("method"), QJsonValue("session.describe"));
  EXPECT_EQ(commands[1].toObject().value("method"), QJsonValue("session.ping"));
  for (const QJsonValue& command : commands) {
    const QJsonObject params = command.toObject().value("params_schema").toObject();
    EXPECT_EQ(params.value("additionalProperties"), QJsonValue(false));
    EXPECT_TRUE(params.value("properties").toObject().contains("timeout_ms"));
    EXPECT_FALSE(command.toObject().value("result_schema").toObject().isEmpty());
  }

  ResponseLog log;
  registry.Dispatch(MakeRequest(QStringLiteral("session.describe"), {}),
                    log.MakeReply(QJsonValue(1)));
  ASSERT_EQ(log.responses.size(), 1U);
  const QJsonObject result = log.responses[0].result.toObject();
  EXPECT_EQ(result.value("protocol_version"), QJsonValue(1));
  EXPECT_EQ(result.value("host_mode"), QJsonValue("headless"));
  EXPECT_EQ(result.value("commands").toArray().size(), 2);
}

}  // namespace
}  // namespace alcedo::automation
