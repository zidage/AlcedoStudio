//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QVariantList>
#include <QVariantMap>
#include <filesystem>
#include <memory>

#include "app/ai_credential_store.hpp"

namespace alcedo {

/// The provider profiles that the user saved for Advanced Content Analysis, and the deletion of
/// their saved credentials (issue #710).
///
/// Advanced Content Analysis is removed. This controller reads the stored profile file
/// (`ai_providers.json`, schema version 1) so the Saved AI Keys settings page can list the
/// profiles and delete the API keys and the Codex OAuth login that `IAiCredentialStore` keeps
/// for them. The profile file keeps its format: a save writes back every field that the load
/// read, and changes only the deleted profiles and the masked key labels of deleted keys.
class AiProviderProfileController final : public QObject {
  Q_OBJECT
  /// One map for each stored profile: `uuid`, `displayName`, `baseUrl`, `isCodexOAuth`,
  /// `maskedKeyLabel`, and `credentialAvailable` (true when the OS credential store holds a
  /// credential for the profile).
  Q_PROPERTY(QVariantList profiles READ Profiles NOTIFY ProfilesChanged)
  Q_PROPERTY(bool hasProfiles READ HasProfiles NOTIFY ProfilesChanged)
  /// True when at least one stored profile has a saved credential.
  Q_PROPERTY(bool hasSavedCredentials READ HasSavedCredentials NOTIFY ProfilesChanged)

 public:
  explicit AiProviderProfileController(QObject* parent = nullptr);
  AiProviderProfileController(std::filesystem::path               storage_file,
                              std::shared_ptr<IAiCredentialStore> credential_store,
                              QObject*                            parent = nullptr);

  QVariantList Profiles() const;
  bool         HasProfiles() const;
  bool         HasSavedCredentials() const;

  Q_INVOKABLE QVariantMap Profile(const QString& profile_id) const;
  /// Deletes the credential of the profile and clears its masked key label.
  Q_INVOKABLE void        DeleteApiKey(const QString& profile_id);
  /// Removes the profile from the stored profile file. With @p delete_credential, also deletes
  /// its credential. Returns false when no profile has the id.
  Q_INVOKABLE bool        DeleteProfile(const QString& profile_id, bool delete_credential);
  /// Deletes the credential of every stored profile. Returns the number of credentials that the
  /// credential store deleted.
  Q_INVOKABLE int         DeleteAllApiKeys();

 signals:
  void ProfilesChanged();

 private:
  auto ProfileIndex(const QString& profile_id) const -> int;
  auto ProfileAt(int index) const -> QJsonObject;
  auto ProfileToVariant(const QJsonObject& profile) const -> QVariantMap;
  auto HasSavedCredential(const QJsonObject& profile) const -> bool;
  void ClearMaskedKeyLabel(int index);
  void Load();
  void Save();

  static auto DefaultStorageFile() -> std::filesystem::path;

  std::filesystem::path               storage_file_;
  std::shared_ptr<IAiCredentialStore> credential_store_;
  /// The root object of the stored profile file, as read. `profiles` holds the profile objects.
  QJsonObject                         store_;
};

}  // namespace alcedo
