//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/ai_provider_profile.hpp"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonValue>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <string>
#include <utility>

namespace alcedo {
namespace {

constexpr int kStoreSchemaVersion = 1;

const QString kProfilesKey       = QStringLiteral("profiles");
const QString kMaskedKeyLabelKey = QStringLiteral("masked_key_label");

#ifdef _WIN32
std::filesystem::path FsPathFromQString(const QString& path) {
  return std::filesystem::path(path.toStdWString());
}

QString QStringFromFsPath(const std::filesystem::path& path) {
  return QString::fromStdWString(path.wstring());
}
#else
std::filesystem::path FsPathFromQString(const QString& path) {
  return std::filesystem::path(path.toStdString());
}

QString QStringFromFsPath(const std::filesystem::path& path) {
  return QString::fromStdString(path.string());
}
#endif

bool LooksLikeRawSecret(const QString& value) {
  const QString                   v = value.trimmed();
  static const QRegularExpression kOpenAiStyleKey(QStringLiteral("sk-[A-Za-z0-9_-]{16,}"));
  static const QRegularExpression kBearerValue(QStringLiteral("Bearer\\s+\\S{8,}"),
                                               QRegularExpression::CaseInsensitiveOption);
  static const QRegularExpression kAwsAccessKey(QStringLiteral("AKIA[A-Z0-9]{16}"));
  return kOpenAiStyleKey.match(v).hasMatch() || kBearerValue.match(v).hasMatch() ||
         kAwsAccessKey.match(v).hasMatch();
}

/// The text of @p key in @p profile, or an empty string when the text looks like a secret, so
/// the page never shows a key that a user typed into a wrong field.
QString NonSecretString(const QJsonObject& profile, const QString& key) {
  const QString value = profile.value(key).toString();
  return LooksLikeRawSecret(value) ? QString{} : value.trimmed();
}

bool IsSlotChar(QChar c) {
  const ushort ch = c.unicode();
  return (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_';
}

/// The credential slot of @p profile, or an empty string when the stored slot is not a valid
/// slot label (`[a-z0-9_]+`).
QString CredentialSlot(const QJsonObject& profile) {
  const QString slot = NonSecretString(profile, QStringLiteral("credential_slot")).toLower();
  for (QChar c : slot) {
    if (!IsSlotChar(c)) {
      return {};
    }
  }
  return slot;
}

/// The load of the earlier analysis feature listed only profiles with a uuid, a provider id, and
/// a credential slot. The page lists the same profiles.
bool IsListedProfile(const QJsonObject& profile) {
  return !NonSecretString(profile, QStringLiteral("uuid")).isEmpty() &&
         !NonSecretString(profile, QStringLiteral("provider_id")).isEmpty() &&
         !CredentialSlot(profile).isEmpty();
}

bool IsCodexOAuthProfile(const QJsonObject& profile) {
  const QString codex = QStringLiteral("openai_codex_oauth");
  return profile.value(QStringLiteral("driver")).toString() == codex ||
         profile.value(QStringLiteral("based_on_template")).toString() == codex;
}

}  // namespace

AiProviderProfileController::AiProviderProfileController(QObject* parent)
    : AiProviderProfileController(DefaultStorageFile(), MakeDefaultAiCredentialStore(), parent) {}

AiProviderProfileController::AiProviderProfileController(
    std::filesystem::path storage_file, std::shared_ptr<IAiCredentialStore> credential_store,
    QObject* parent)
    : QObject(parent),
      storage_file_(std::move(storage_file)),
      credential_store_(std::move(credential_store)) {
  if (!credential_store_) {
    credential_store_ = MakeDefaultAiCredentialStore();
  }
  Load();
}

auto AiProviderProfileController::DefaultStorageFile() -> std::filesystem::path {
  QString root = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
  if (root.isEmpty()) {
    root = QDir::homePath() + QStringLiteral("/.alcedo_studio");
  }
  return FsPathFromQString(root) / "ai_providers.json";
}

QVariantList AiProviderProfileController::Profiles() const {
  QVariantList out;
  const QJsonArray profiles = store_.value(kProfilesKey).toArray();
  for (const auto& value : profiles) {
    const QJsonObject profile = value.toObject();
    if (IsListedProfile(profile)) {
      out.push_back(ProfileToVariant(profile));
    }
  }
  return out;
}

bool AiProviderProfileController::HasProfiles() const {
  const QJsonArray profiles = store_.value(kProfilesKey).toArray();
  for (const auto& value : profiles) {
    if (IsListedProfile(value.toObject())) {
      return true;
    }
  }
  return false;
}

bool AiProviderProfileController::HasSavedCredentials() const {
  const QJsonArray profiles = store_.value(kProfilesKey).toArray();
  for (const auto& value : profiles) {
    const QJsonObject profile = value.toObject();
    if (IsListedProfile(profile) && HasSavedCredential(profile)) {
      return true;
    }
  }
  return false;
}

QVariantMap AiProviderProfileController::Profile(const QString& profile_id) const {
  const int index = ProfileIndex(profile_id);
  return index < 0 ? QVariantMap{} : ProfileToVariant(ProfileAt(index));
}

void AiProviderProfileController::DeleteApiKey(const QString& profile_id) {
  const int index = ProfileIndex(profile_id);
  if (index < 0) {
    return;
  }
  std::string error;
  credential_store_->DeleteCredential(CredentialSlot(ProfileAt(index)).toStdString(), &error);
  ClearMaskedKeyLabel(index);
  Save();
  emit ProfilesChanged();
}

bool AiProviderProfileController::DeleteProfile(const QString& profile_id, bool delete_credential) {
  const int index = ProfileIndex(profile_id);
  if (index < 0) {
    return false;
  }
  if (delete_credential) {
    std::string error;
    credential_store_->DeleteCredential(CredentialSlot(ProfileAt(index)).toStdString(), &error);
  }
  QJsonArray profiles = store_.value(kProfilesKey).toArray();
  profiles.removeAt(index);
  store_.insert(kProfilesKey, profiles);
  Save();
  emit ProfilesChanged();
  return true;
}

int AiProviderProfileController::DeleteAllApiKeys() {
  int              deleted  = 0;
  const QJsonArray profiles = store_.value(kProfilesKey).toArray();
  for (int index = 0; index < profiles.size(); ++index) {
    const QJsonObject profile = profiles.at(index).toObject();
    if (!IsListedProfile(profile) || !HasSavedCredential(profile)) {
      continue;
    }
    std::string error;
    if (credential_store_->DeleteCredential(CredentialSlot(profile).toStdString(), &error)) {
      ++deleted;
      ClearMaskedKeyLabel(index);
    }
  }
  Save();
  emit ProfilesChanged();
  return deleted;
}

auto AiProviderProfileController::ProfileIndex(const QString& profile_id) const -> int {
  if (profile_id.isEmpty()) {
    return -1;
  }
  const QJsonArray profiles = store_.value(kProfilesKey).toArray();
  for (int index = 0; index < profiles.size(); ++index) {
    const QJsonObject profile = profiles.at(index).toObject();
    if (IsListedProfile(profile) && NonSecretString(profile, QStringLiteral("uuid")) == profile_id) {
      return index;
    }
  }
  return -1;
}

auto AiProviderProfileController::ProfileAt(int index) const -> QJsonObject {
  return store_.value(kProfilesKey).toArray().at(index).toObject();
}

auto AiProviderProfileController::ProfileToVariant(const QJsonObject& profile) const
    -> QVariantMap {
  QVariantMap m;
  m.insert(QStringLiteral("uuid"), NonSecretString(profile, QStringLiteral("uuid")));
  m.insert(QStringLiteral("displayName"), NonSecretString(profile, QStringLiteral("display_name")));
  m.insert(QStringLiteral("baseUrl"), NonSecretString(profile, QStringLiteral("base_url")));
  m.insert(QStringLiteral("isCodexOAuth"), IsCodexOAuthProfile(profile));
  m.insert(QStringLiteral("maskedKeyLabel"), NonSecretString(profile, kMaskedKeyLabelKey));
  m.insert(QStringLiteral("credentialAvailable"), HasSavedCredential(profile));
  return m;
}

auto AiProviderProfileController::HasSavedCredential(const QJsonObject& profile) const -> bool {
  const QString slot = CredentialSlot(profile);
  return !slot.isEmpty() && credential_store_->HasCredential(slot.toStdString());
}

void AiProviderProfileController::ClearMaskedKeyLabel(int index) {
  QJsonArray  profiles = store_.value(kProfilesKey).toArray();
  QJsonObject profile  = profiles.at(index).toObject();
  profile.insert(kMaskedKeyLabelKey, QString{});
  profiles.replace(index, profile);
  store_.insert(kProfilesKey, profiles);
}

void AiProviderProfileController::Load() {
  store_ = {};
  QFile file(QStringFromFsPath(storage_file_));
  if (!file.exists() || !file.open(QIODevice::ReadOnly)) {
    return;
  }
  const auto doc = QJsonDocument::fromJson(file.readAll());
  if (!doc.isObject() ||
      doc.object().value(QStringLiteral("schema_version")).toInt() != kStoreSchemaVersion) {
    return;
  }
  store_ = doc.object();
}

void AiProviderProfileController::Save() {
  if (store_.isEmpty()) {
    return;
  }
  QDir dir(QStringFromFsPath(storage_file_.parent_path()));
  if (!dir.exists()) {
    dir.mkpath(QStringLiteral("."));
  }
  QSaveFile file(QStringFromFsPath(storage_file_));
  if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
    return;
  }
  file.write(QJsonDocument(store_).toJson(QJsonDocument::Indented));
  file.commit();
}

}  // namespace alcedo
