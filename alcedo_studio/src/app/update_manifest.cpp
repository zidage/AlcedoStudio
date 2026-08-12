//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/update_manifest.hpp"

#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <cmath>
#include <limits>

extern "C" {
#include <ed25519.h>
}

namespace alcedo {
namespace {

constexpr qint64 kMaximumArtifactSize  = 8LL * 1024LL * 1024LL * 1024LL;
constexpr int    kMaximumChangelogChars = 16 * 1024;

auto             Failure(QString message) -> UpdateManifestResult {
  return UpdateManifestResult{std::nullopt, std::move(message)};
}

auto ReadPositiveInteger(const QJsonObject& object, const QString& name, quint64* output) -> bool {
  const QJsonValue value = object.value(name);
  if (!value.isDouble()) {
    return false;
  }
  const double number = value.toDouble();
  if (!std::isfinite(number) || number < 1.0 || std::floor(number) != number ||
      number > 9007199254740991.0) {
    return false;
  }
  *output = static_cast<quint64>(number);
  return true;
}

auto ParseUtcTimestamp(const QJsonObject& object, const QString& name) -> QDateTime {
  const QJsonValue value = object.value(name);
  if (!value.isString()) {
    return {};
  }
  QDateTime timestamp = QDateTime::fromString(value.toString(), Qt::ISODateWithMs);
  if (!timestamp.isValid()) {
    timestamp = QDateTime::fromString(value.toString(), Qt::ISODate);
  }
  return timestamp.isValid() ? timestamp.toUTC() : QDateTime{};
}

}  // namespace

UpdateManifestResult VerifyUpdateManifest(const QByteArray& manifest_bytes,
                                          const QByteArray& signature_text,
                                          const QByteArray& public_key, const QString& platform_key,
                                          const QUrl& feed_url, quint64 minimum_sequence,
                                          const QDateTime& now_utc) {
  if (public_key.size() != 32) {
    return Failure(QStringLiteral("The update public key is not valid."));
  }

  const QByteArray signature =
      QByteArray::fromBase64(signature_text.trimmed(), QByteArray::AbortOnBase64DecodingErrors);
  if (signature.size() != 64) {
    return Failure(QStringLiteral("The update signature has an invalid format."));
  }
  if (ed25519_verify(reinterpret_cast<const unsigned char*>(signature.constData()),
                     reinterpret_cast<const unsigned char*>(manifest_bytes.constData()),
                     static_cast<size_t>(manifest_bytes.size()),
                     reinterpret_cast<const unsigned char*>(public_key.constData())) != 1) {
    return Failure(QStringLiteral("The update signature is not valid."));
  }

  QJsonParseError     parse_error;
  const QJsonDocument document = QJsonDocument::fromJson(manifest_bytes, &parse_error);
  if (parse_error.error != QJsonParseError::NoError || !document.isObject()) {
    return Failure(QStringLiteral("The signed update manifest is not valid JSON."));
  }
  const QJsonObject root   = document.object();
  const QJsonValue  schema = root.value(QStringLiteral("schema"));
  if (!schema.isDouble() || schema.toDouble() != 1.0) {
    return Failure(QStringLiteral("The update manifest schema is not supported."));
  }

  UpdateManifest manifest;
  if (!ReadPositiveInteger(root, QStringLiteral("sequence"), &manifest.sequence) ||
      manifest.sequence < minimum_sequence) {
    return Failure(
        QStringLiteral("The update manifest sequence is older than a trusted manifest."));
  }
  if (!ReadPositiveInteger(root, QStringLiteral("build"), &manifest.build)) {
    return Failure(QStringLiteral("The update build number is not valid."));
  }
  manifest.version = root.value(QStringLiteral("version")).toString().trimmed();
  if (manifest.version.isEmpty() || manifest.version.size() > 64) {
    return Failure(QStringLiteral("The update version is not valid."));
  }

  manifest.published_at = ParseUtcTimestamp(root, QStringLiteral("publishedAt"));
  manifest.expires_at   = ParseUtcTimestamp(root, QStringLiteral("expiresAt"));
  if (!manifest.published_at.isValid() || !manifest.expires_at.isValid() ||
      manifest.expires_at <= manifest.published_at) {
    return Failure(QStringLiteral("The update manifest timestamps are not valid."));
  }
  if (now_utc.toUTC() > manifest.expires_at) {
    return Failure(QStringLiteral("The update manifest has expired."));
  }
  if (manifest.published_at > now_utc.toUTC().addSecs(24 * 60 * 60)) {
    return Failure(QStringLiteral("The update manifest has a future publication time."));
  }

  const QJsonValue artifacts_value = root.value(QStringLiteral("artifacts"));
  if (!artifacts_value.isObject()) {
    return Failure(QStringLiteral("The update manifest has no artifacts."));
  }
  const QJsonValue artifact_value = artifacts_value.toObject().value(platform_key);
  if (!artifact_value.isObject()) {
    return Failure(QStringLiteral("This update does not support the current platform."));
  }
  const QJsonObject artifact_object = artifact_value.toObject();

  manifest.artifact.url             = QUrl(artifact_object.value(QStringLiteral("url")).toString());
  if (!manifest.artifact.url.isValid() ||
      manifest.artifact.url.scheme() != QStringLiteral("https") ||
      manifest.artifact.url.host().compare(feed_url.host(), Qt::CaseInsensitive) != 0 ||
      !manifest.artifact.url.userInfo().isEmpty()) {
    return Failure(QStringLiteral("The update package URL is not allowed."));
  }
  const QString artifact_suffix = QFileInfo(manifest.artifact.url.path()).suffix().toLower();
  if ((platform_key == QStringLiteral("windows-x86_64") &&
       artifact_suffix != QStringLiteral("exe")) ||
      (platform_key.startsWith(QStringLiteral("macos-")) &&
       artifact_suffix != QStringLiteral("zip"))) {
    return Failure(QStringLiteral("The update package type is not allowed for this platform."));
  }

  const QString sha_text   = artifact_object.value(QStringLiteral("sha256")).toString().trimmed();
  manifest.artifact.sha256 = QByteArray::fromHex(sha_text.toLatin1());
  if (sha_text.size() != 64 || manifest.artifact.sha256.size() != 32 ||
      manifest.artifact.sha256.toHex() != sha_text.toLatin1().toLower()) {
    return Failure(QStringLiteral("The update package SHA-256 value is not valid."));
  }

  quint64 artifact_size = 0;
  if (!ReadPositiveInteger(artifact_object, QStringLiteral("size"), &artifact_size) ||
      artifact_size > static_cast<quint64>(kMaximumArtifactSize)) {
    return Failure(QStringLiteral("The update package size is not valid."));
  }
  manifest.artifact.size   = static_cast<qint64>(artifact_size);

  const QString notes_text = root.value(QStringLiteral("notesUrl")).toString().trimmed();
  if (!notes_text.isEmpty()) {
    manifest.notes_url = QUrl(notes_text);
    if (!manifest.notes_url.isValid() || manifest.notes_url.scheme() != QStringLiteral("https") ||
        !manifest.notes_url.userInfo().isEmpty()) {
      return Failure(QStringLiteral("The release notes URL is not valid."));
    }
  }

  if (root.contains(QStringLiteral("changelog"))) {
    const QJsonValue changelog_value = root.value(QStringLiteral("changelog"));
    if (!changelog_value.isString()) {
      return Failure(QStringLiteral("The update changelog is not valid."));
    }
    manifest.changelog = changelog_value.toString();
    if (manifest.changelog.size() > kMaximumChangelogChars) {
      return Failure(QStringLiteral("The update changelog is too large."));
    }
    if (manifest.changelog.trimmed().isEmpty()) {
      manifest.changelog.clear();
    }
  }

  return UpdateManifestResult{std::move(manifest), {}};
}

}  // namespace alcedo
