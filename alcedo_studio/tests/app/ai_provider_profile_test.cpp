//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/ai_provider_profile.hpp"

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QTemporaryDir>
#include <QVariantList>
#include <QVariantMap>
#include <filesystem>
#include <memory>
#include <string>

namespace {

#ifdef _WIN32
auto FsPathFromQStringForTest(const QString& path) -> std::filesystem::path {
  return std::filesystem::path(path.toStdWString());
}
#else
auto FsPathFromQStringForTest(const QString& path) -> std::filesystem::path {
  return std::filesystem::path(path.toStdString());
}
#endif

constexpr auto kApiKeyProfileId = "11111111-1111-1111-1111-111111111111";
constexpr auto kCodexProfileId  = "22222222-2222-2222-2222-222222222222";
constexpr auto kNoKeyProfileId  = "33333333-3333-3333-3333-333333333333";
constexpr auto kApiKeySlot      = "alcedo_ai_apikey";
constexpr auto kCodexSlot       = "alcedo_ai_codex";
constexpr auto kNoKeySlot       = "alcedo_ai_nokey";

/// A profile object in the format that the earlier Advanced Content Analysis settings wrote.
auto StoredProfile(const QString& uuid, const QString& display_name, const QString& template_id,
                   const QString& driver, const QString& base_url, const QString& slot,
                   const QString& masked_key_label) -> QJsonObject {
  QJsonObject o;
  o.insert(QStringLiteral("uuid"), uuid);
  o.insert(QStringLiteral("display_name"), display_name);
  o.insert(QStringLiteral("based_on_template"), template_id);
  o.insert(QStringLiteral("credential_slot"), slot);
  o.insert(QStringLiteral("masked_key_label"), masked_key_label);
  o.insert(QStringLiteral("remember_key"), true);
  o.insert(QStringLiteral("last_used_ms"), 1700000000000LL);
  o.insert(QStringLiteral("provider_id"), QStringLiteral("profile_") + slot);
  o.insert(QStringLiteral("driver"), driver);
  o.insert(QStringLiteral("base_url"), base_url);
  o.insert(QStringLiteral("endpoint"), QStringLiteral("/responses"));
  o.insert(QStringLiteral("auth_type"), QStringLiteral("bearer"));
  o.insert(QStringLiteral("model_id"), QStringLiteral("stored-model"));
  o.insert(QStringLiteral("timeout_ms"), 60000);
  QJsonObject model;
  model.insert(QStringLiteral("slug"), QStringLiteral("stored-model"));
  o.insert(QStringLiteral("models"), QJsonArray{model});
  return o;
}

class AiProviderProfileTest : public ::testing::Test {
 protected:
  static void SetUpTestSuite() {
    static int    argc = 0;
    static char** argv = nullptr;
    if (app_ == nullptr) {
      app_ = new QCoreApplication(argc, argv);
    }
    QCoreApplication::setOrganizationName(QStringLiteral("PuerhLabTest"));
    QCoreApplication::setApplicationName(QStringLiteral("AiProviderProfileTest"));
  }

  AiProviderProfileTest() : store_(std::make_shared<alcedo::InMemoryAiCredentialStore>()) {}

  auto StorageFileQt() const -> QString {
    return temp_.filePath(QStringLiteral("ai_providers.json"));
  }

  auto StorageFile() const -> std::filesystem::path {
    return FsPathFromQStringForTest(StorageFileQt());
  }

  /// Writes a profile file with an API-key profile, a Codex OAuth profile, a profile without a
  /// saved key, and a profile without a credential slot (which the earlier load did not list).
  void WriteStoredProfiles() {
    QJsonArray profiles;
    profiles.push_back(StoredProfile(QString::fromLatin1(kApiKeyProfileId),
                                     QStringLiteral("OpenCode"),
                                     QStringLiteral("opencode_go_openai"),
                                     QStringLiteral("openai_chat_compatible"),
                                     QStringLiteral("https://opencode.ai/zen/go/v1"),
                                     QString::fromLatin1(kApiKeySlot), QStringLiteral("****3456")));
    profiles.push_back(StoredProfile(
        QString::fromLatin1(kCodexProfileId), QStringLiteral("OpenAI Codex OAuth"),
        QStringLiteral("openai_codex_oauth"), QStringLiteral("openai_codex_oauth"),
        QStringLiteral("https://chatgpt.com/backend-api/codex"), QString::fromLatin1(kCodexSlot),
        QStringLiteral("Codex OAuth ****abcdef")));
    profiles.push_back(StoredProfile(QString::fromLatin1(kNoKeyProfileId),
                                     QStringLiteral("Local"), QStringLiteral("custom"),
                                     QStringLiteral("openai_chat_compatible"),
                                     QStringLiteral("http://localhost:11434/v1"),
                                     QString::fromLatin1(kNoKeySlot), QString{}));
    profiles.push_back(StoredProfile(QStringLiteral("44444444-4444-4444-4444-444444444444"),
                                     QStringLiteral("No slot"), QStringLiteral("custom"),
                                     QStringLiteral("openai_chat_compatible"),
                                     QStringLiteral("http://localhost:1/v1"), QString{},
                                     QString{}));
    QJsonObject root;
    root.insert(QStringLiteral("schema_version"), 1);
    root.insert(QStringLiteral("active_profile_id"), QString::fromLatin1(kApiKeyProfileId));
    root.insert(QStringLiteral("output_language"), QStringLiteral("zh"));
    root.insert(QStringLiteral("profiles"), profiles);
    QFile file(StorageFileQt());
    ASSERT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
  }

  void SaveCredential(const char* slot, const std::string& secret) {
    std::string error;
    ASSERT_TRUE(store_->SaveCredential(slot, secret, &error)) << error;
  }

  auto ReadStoredRoot() const -> QJsonObject {
    QFile file(StorageFileQt());
    EXPECT_TRUE(file.open(QIODevice::ReadOnly));
    return QJsonDocument::fromJson(file.readAll()).object();
  }

  static auto StoredProfileById(const QJsonObject& root, const QString& uuid) -> QJsonObject {
    for (const auto& value : root.value(QStringLiteral("profiles")).toArray()) {
      if (value.toObject().value(QStringLiteral("uuid")).toString() == uuid) {
        return value.toObject();
      }
    }
    return {};
  }

  static QCoreApplication*                           app_;
  QTemporaryDir                                      temp_;
  std::shared_ptr<alcedo::InMemoryAiCredentialStore> store_;
};

QCoreApplication* AiProviderProfileTest::app_ = nullptr;

TEST_F(AiProviderProfileTest, MissingProfileFileListsNoProfiles) {
  alcedo::AiProviderProfileController controller(StorageFile(), store_);
  EXPECT_TRUE(controller.Profiles().isEmpty());
  EXPECT_FALSE(controller.HasProfiles());
  EXPECT_FALSE(controller.HasSavedCredentials());
  EXPECT_EQ(controller.DeleteAllApiKeys(), 0);
  EXPECT_FALSE(QFile::exists(StorageFileQt()));
}

TEST_F(AiProviderProfileTest, LoadListsStoredProfilesWithTheirCredentialState) {
  WriteStoredProfiles();
  SaveCredential(kApiKeySlot, "sk-stored-api-key-123456");
  SaveCredential(kCodexSlot, R"({"tokens":{"access_token":"codex"}})");

  alcedo::AiProviderProfileController controller(StorageFile(), store_);
  ASSERT_EQ(controller.Profiles().size(), 3);
  EXPECT_TRUE(controller.HasProfiles());
  EXPECT_TRUE(controller.HasSavedCredentials());

  const QVariantMap api_key = controller.Profile(QString::fromLatin1(kApiKeyProfileId));
  EXPECT_EQ(api_key.value(QStringLiteral("displayName")).toString(), QStringLiteral("OpenCode"));
  EXPECT_EQ(api_key.value(QStringLiteral("baseUrl")).toString(),
            QStringLiteral("https://opencode.ai/zen/go/v1"));
  EXPECT_FALSE(api_key.value(QStringLiteral("isCodexOAuth")).toBool());
  EXPECT_EQ(api_key.value(QStringLiteral("maskedKeyLabel")).toString(), QStringLiteral("****3456"));
  EXPECT_TRUE(api_key.value(QStringLiteral("credentialAvailable")).toBool());

  const QVariantMap codex = controller.Profile(QString::fromLatin1(kCodexProfileId));
  EXPECT_TRUE(codex.value(QStringLiteral("isCodexOAuth")).toBool());
  EXPECT_TRUE(codex.value(QStringLiteral("credentialAvailable")).toBool());

  const QVariantMap no_key = controller.Profile(QString::fromLatin1(kNoKeyProfileId));
  EXPECT_FALSE(no_key.value(QStringLiteral("credentialAvailable")).toBool());

  EXPECT_TRUE(controller.Profile(QStringLiteral("44444444-4444-4444-4444-444444444444")).isEmpty());
}

TEST_F(AiProviderProfileTest, DeleteApiKeyDeletesCredentialAndKeepsTheOtherStoredFields) {
  WriteStoredProfiles();
  SaveCredential(kApiKeySlot, "sk-stored-api-key-123456");
  alcedo::AiProviderProfileController controller(StorageFile(), store_);

  controller.DeleteApiKey(QString::fromLatin1(kApiKeyProfileId));

  EXPECT_FALSE(store_->HasCredential(kApiKeySlot));
  const QVariantMap api_key = controller.Profile(QString::fromLatin1(kApiKeyProfileId));
  EXPECT_FALSE(api_key.value(QStringLiteral("credentialAvailable")).toBool());
  EXPECT_TRUE(api_key.value(QStringLiteral("maskedKeyLabel")).toString().isEmpty());

  const QJsonObject root = ReadStoredRoot();
  EXPECT_EQ(root.value(QStringLiteral("schema_version")).toInt(), 1);
  EXPECT_EQ(root.value(QStringLiteral("active_profile_id")).toString(),
            QString::fromLatin1(kApiKeyProfileId));
  EXPECT_EQ(root.value(QStringLiteral("output_language")).toString(), QStringLiteral("zh"));
  EXPECT_EQ(root.value(QStringLiteral("profiles")).toArray().size(), 4);
  const QJsonObject stored = StoredProfileById(root, QString::fromLatin1(kApiKeyProfileId));
  EXPECT_TRUE(stored.value(QStringLiteral("masked_key_label")).toString().isEmpty());
  EXPECT_EQ(stored.value(QStringLiteral("model_id")).toString(), QStringLiteral("stored-model"));
  EXPECT_EQ(stored.value(QStringLiteral("endpoint")).toString(), QStringLiteral("/responses"));
  EXPECT_EQ(stored.value(QStringLiteral("models")).toArray().size(), 1);
  EXPECT_EQ(StoredProfileById(root, QString::fromLatin1(kCodexProfileId))
                .value(QStringLiteral("masked_key_label"))
                .toString(),
            QStringLiteral("Codex OAuth ****abcdef"));
}

TEST_F(AiProviderProfileTest, DeleteProfileRemovesProfileAndItsCredential) {
  WriteStoredProfiles();
  SaveCredential(kCodexSlot, "codex-token-json");
  alcedo::AiProviderProfileController controller(StorageFile(), store_);

  EXPECT_TRUE(controller.DeleteProfile(QString::fromLatin1(kCodexProfileId), true));
  EXPECT_FALSE(store_->HasCredential(kCodexSlot));
  EXPECT_EQ(controller.Profiles().size(), 2);
  EXPECT_TRUE(controller.Profile(QString::fromLatin1(kCodexProfileId)).isEmpty());

  alcedo::AiProviderProfileController reloaded(StorageFile(), store_);
  EXPECT_EQ(reloaded.Profiles().size(), 2);
  EXPECT_TRUE(reloaded.Profile(QString::fromLatin1(kCodexProfileId)).isEmpty());
  EXPECT_FALSE(controller.DeleteProfile(QString::fromLatin1(kCodexProfileId), true));
}

TEST_F(AiProviderProfileTest, DeleteProfileCanKeepCredentialWhenRequested) {
  WriteStoredProfiles();
  SaveCredential(kApiKeySlot, "sk-keep-me-secret-123456");
  alcedo::AiProviderProfileController controller(StorageFile(), store_);

  EXPECT_TRUE(controller.DeleteProfile(QString::fromLatin1(kApiKeyProfileId), false));
  EXPECT_TRUE(store_->HasCredential(kApiKeySlot));
  EXPECT_TRUE(controller.Profile(QString::fromLatin1(kApiKeyProfileId)).isEmpty());
}

TEST_F(AiProviderProfileTest, DeleteAllApiKeysDeletesEveryStoredCredentialAndReturnsTheCount) {
  WriteStoredProfiles();
  SaveCredential(kApiKeySlot, "sk-stored-api-key-123456");
  SaveCredential(kCodexSlot, "codex-token-json");
  alcedo::AiProviderProfileController controller(StorageFile(), store_);
  ASSERT_TRUE(controller.HasSavedCredentials());

  EXPECT_EQ(controller.DeleteAllApiKeys(), 2);

  EXPECT_FALSE(store_->HasCredential(kApiKeySlot));
  EXPECT_FALSE(store_->HasCredential(kCodexSlot));
  EXPECT_FALSE(controller.HasSavedCredentials());
  const QVariantList profiles = controller.Profiles();
  ASSERT_EQ(profiles.size(), 3);
  for (const auto& value : profiles) {
    EXPECT_FALSE(value.toMap().value(QStringLiteral("credentialAvailable")).toBool());
  }
  EXPECT_TRUE(StoredProfileById(ReadStoredRoot(), QString::fromLatin1(kApiKeyProfileId))
                  .value(QStringLiteral("masked_key_label"))
                  .toString()
                  .isEmpty());
}

TEST_F(AiProviderProfileTest, ProfileListShowsNoTextThatLooksLikeAnApiKey) {
  QJsonArray profiles;
  profiles.push_back(StoredProfile(QString::fromLatin1(kApiKeyProfileId),
                                   QStringLiteral("sk-abcdefghijklmnopqrstuvwx"),
                                   QStringLiteral("custom"), QStringLiteral("openai_chat_compatible"),
                                   QStringLiteral("Bearer abcdefghijklmnop"),
                                   QString::fromLatin1(kApiKeySlot), QString{}));
  QJsonObject root;
  root.insert(QStringLiteral("schema_version"), 1);
  root.insert(QStringLiteral("profiles"), profiles);
  QFile file(StorageFileQt());
  ASSERT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
  file.write(QJsonDocument(root).toJson());
  file.close();

  alcedo::AiProviderProfileController controller(StorageFile(), store_);
  const QVariantMap profile = controller.Profile(QString::fromLatin1(kApiKeyProfileId));
  EXPECT_TRUE(profile.value(QStringLiteral("displayName")).toString().isEmpty());
  EXPECT_TRUE(profile.value(QStringLiteral("baseUrl")).toString().isEmpty());
}

}  // namespace
