//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// The command reference of the alcedo skill (https://github.com/zidage/alcedo-skills, in
// skills/alcedo/references/reference/) is generated from commands.json and editor_catalog.json.
// These tests check that both files of the local checkout in ALCEDO_SKILLS_DIR are the same as
// the schemas that a GUI session of this build reports. On a difference they write the schemas
// of the build to ALCEDO_REFERENCE_OUT_DIR, and the failure message names the command that
// updates the docs.

#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>
#include <QTemporaryDir>
#include <algorithm>
#include <optional>
#include <string>
#include <vector>

#include "automation/automation_protocol.hpp"
#include "ui/album_backend_test_fixture.hpp"
#include "ui/alcedo_main/automation/automation_host_session.hpp"

namespace alcedo::automation {
namespace {

auto ReadJsonFile(const QString& path) -> std::optional<QJsonDocument> {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) {
    return std::nullopt;
  }
  QJsonParseError error{};
  QJsonDocument   document = QJsonDocument::fromJson(file.readAll(), &error);
  if (error.error != QJsonParseError::NoError) {
    return std::nullopt;
  }
  return document;
}

/// Writes @p document to ALCEDO_REFERENCE_OUT_DIR/@p name and returns the path.
auto WriteBuildCopy(const QString& name, const QJsonDocument& document) -> QString {
  const QDir out_dir(QStringLiteral(ALCEDO_REFERENCE_OUT_DIR));
  QDir().mkpath(out_dir.absolutePath());
  const QString path = out_dir.absoluteFilePath(name);
  QFile         file(path);
  if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
    file.write(document.toJson(QJsonDocument::Indented));
  }
  return path;
}

auto SortedByMethod(QJsonArray commands) -> QJsonArray {
  std::vector<QJsonValue> values(commands.begin(), commands.end());
  std::sort(values.begin(), values.end(), [](const QJsonValue& left, const QJsonValue& right) {
    return left.toObject().value("method").toString() < right.toObject().value("method").toString();
  });
  QJsonArray sorted;
  for (const QJsonValue& value : values) {
    sorted.push_back(value);
  }
  return sorted;
}

auto MissingHint(const char* name) -> std::string {
  return "cannot read " + std::string(ALCEDO_REFERENCE_DOCS_DIR) + "/" + name +
         ". ALCEDO_SKILLS_DIR must name an alcedo-skills checkout.";
}

auto UpdateHint() -> std::string {
  return "The command reference is out of date. Review the new schemas in " +
         std::string(ALCEDO_REFERENCE_OUT_DIR) +
         ", then run: python scripts/automation/generate_cli_reference.py --skills-dir " +
         std::string(ALCEDO_SKILLS_DIR) + " --schema " +
         std::string(ALCEDO_REFERENCE_OUT_DIR) + "/commands.json --catalog " +
         std::string(ALCEDO_REFERENCE_OUT_DIR) +
         "/editor_catalog.json, and write the errors and the example of each new command.";
}

class AutomationReferenceDocsTest : public ui::test::ApplicationModuleHostTestFixture {
 protected:
  void SetUp() override {
    ApplicationModuleHostTestFixture::SetUp();
    ASSERT_TRUE(session_dir_.isValid());
    AutomationHostSessionOptions options;
    options.host_mode    = QStringLiteral("gui");
    options.session_name = QStringLiteral("reference-docs");
    options.session_dir  = session_dir_.path();
    session_.emplace(&host_, std::move(options));
    QString error;
    ASSERT_TRUE(session_->Start(&error)) << error.toStdString();
  }

  void TearDown() override {
    session_.reset();
    ApplicationModuleHostTestFixture::TearDown();
  }

  ui::ApplicationModuleHost            host_;
  QTemporaryDir                        session_dir_;
  std::optional<AutomationHostSession> session_;
};

TEST_F(AutomationReferenceDocsTest, CommittedCommandsMatchSessionDescribe) {
  const auto committed = ReadJsonFile(QStringLiteral(ALCEDO_REFERENCE_DOCS_DIR "/commands.json"));
  ASSERT_TRUE(committed.has_value() && committed->isArray())
      << MissingHint("commands.json");

  const QJsonArray live = SortedByMethod(session_->registry().Describe());
  if (SortedByMethod(committed->array()) != live) {
    WriteBuildCopy(QStringLiteral("commands.json"), QJsonDocument(live));
    ADD_FAILURE() << UpdateHint();
  }
}

TEST_F(AutomationReferenceDocsTest, CommittedCatalogMatchesEditorCatalog) {
  const auto committed =
      ReadJsonFile(QStringLiteral(ALCEDO_REFERENCE_DOCS_DIR "/editor_catalog.json"));
  ASSERT_TRUE(committed.has_value() && committed->isObject())
      << MissingHint("editor_catalog.json");

  std::optional<AutomationResponse> response;
  const AutomationRequest           request{QJsonValue(1), QStringLiteral("editor.catalog"), {}};
  session_->registry().Dispatch(request,
                                AutomationReply(request.id, [&response](AutomationResponse value) {
                                  response = std::move(value);
                                }));
  ASSERT_TRUE(response.has_value()) << "editor.catalog did not answer at once";
  ASSERT_FALSE(response->error.has_value()) << response->error->message.toStdString();

  const QJsonObject live = response->result.toObject();
  if (committed->object() != live) {
    WriteBuildCopy(QStringLiteral("editor_catalog.json"), QJsonDocument(live));
    ADD_FAILURE() << UpdateHint();
  }
}

}  // namespace
}  // namespace alcedo::automation
