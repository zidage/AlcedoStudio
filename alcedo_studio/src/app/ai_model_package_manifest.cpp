//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/ai_model_package_manifest.hpp"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QPair>
#include <QRegularExpression>
#include <QSet>
#include <cmath>

#include "app/detached_signature.hpp"
#include "app/package_archive.hpp"

namespace alcedo {
namespace {

constexpr quint64 kMaximumArtifactSize     = 8ULL * 1024ULL * 1024ULL * 1024ULL;
constexpr quint64 kMaximumUnpackedBytes    = 32ULL * 1024ULL * 1024ULL * 1024ULL;
constexpr quint64 kMaximumFileCount        = 10000;
constexpr quint64 kMaximumModelSetVersion  = 1000000;
constexpr int     kMaximumPackages         = 64;
constexpr int     kMaximumLicenses         = 32;
constexpr int     kMaximumArtifacts        = 16;
constexpr int     kMaximumNameChars        = 128;
constexpr int     kMaximumLicensePathChars = 256;

auto              Failure(QString message) -> AiModelPackageManifestResult {
  return AiModelPackageManifestResult{std::nullopt, std::move(message)};
}

auto ReadPositiveInteger(const QJsonObject& object, const QString& name, quint64 maximum,
                         quint64* output) -> bool {
  const QJsonValue value = object.value(name);
  if (!value.isDouble()) return false;
  const double number = value.toDouble();
  if (!std::isfinite(number) || number < 1.0 || std::floor(number) != number ||
      number > static_cast<double>(maximum)) {
    return false;
  }
  *output = static_cast<quint64>(number);
  return true;
}

auto ReadSha256(const QJsonObject& object, const QString& name, QByteArray* output) -> bool {
  const QString                   text = object.value(name).toString();
  static const QRegularExpression kHex(QStringLiteral("^[0-9a-f]{64}$"));
  if (!kHex.match(text).hasMatch()) return false;
  *output = QByteArray::fromHex(text.toLatin1());
  return output->size() == 32;
}

auto IsDisplayText(const QString& text) -> bool {
  static const QRegularExpression kControl(QStringLiteral("[\\x00-\\x1f\\x7f]"));
  return !text.trimmed().isEmpty() && text.size() <= kMaximumNameChars &&
         !kControl.match(text).hasMatch();
}

auto ParseLicenses(const QJsonValue& value, AiModelPackageDescriptor* package) -> bool {
  if (!value.isArray() || value.toArray().isEmpty() || value.toArray().size() > kMaximumLicenses) {
    return false;
  }
  for (const QJsonValue& item : value.toArray()) {
    if (!item.isObject()) return false;
    const QJsonObject     object = item.toObject();
    AiModelPackageLicense license{object.value(QStringLiteral("name")).toString(),
                                  object.value(QStringLiteral("file")).toString()};
    if (!object.value(QStringLiteral("name")).isString() || !IsDisplayText(license.name) ||
        !object.value(QStringLiteral("file")).isString() ||
        license.file.size() > kMaximumLicensePathChars ||
        !IsSafePackageRelativePath(license.file.toStdString())) {
      return false;
    }
    package->licenses.push_back(std::move(license));
  }
  return true;
}

auto ParseArtifact(const QString& platform, const QJsonValue& value, const QUrl& feed_url,
                   const QString& package_label, AiModelPackageArtifact* artifact) -> QString {
  static const QRegularExpression kPlatform(QStringLiteral("^[a-z0-9]+(?:[-_][a-z0-9]+)*$"));
  if (platform.size() > 64 || !kPlatform.match(platform).hasMatch() || !value.isObject()) {
    return QStringLiteral("An artifact of AI model package %1 is not valid.").arg(package_label);
  }
  const QJsonObject object = value.toObject();
  artifact->platform       = platform;
  artifact->url            = QUrl(object.value(QStringLiteral("url")).toString());
  const QUrl& url          = artifact->url;
  if (!url.isValid() || url.scheme() != QStringLiteral("https") ||
      url.host().compare(feed_url.host(), Qt::CaseInsensitive) != 0 || !url.userInfo().isEmpty() ||
      !url.path().endsWith(QStringLiteral(".7z"))) {
    return QStringLiteral("The %1 archive URL of AI model package %2 is not allowed.")
        .arg(platform, package_label);
  }
  if (!ReadSha256(object, QStringLiteral("sha256"), &artifact->sha256)) {
    return QStringLiteral("The %1 archive SHA-256 of AI model package %2 is not valid.")
        .arg(platform, package_label);
  }
  quint64 size = 0;
  if (!ReadPositiveInteger(object, QStringLiteral("size"), kMaximumArtifactSize, &size)) {
    return QStringLiteral("The %1 archive size of AI model package %2 is not valid.")
        .arg(platform, package_label);
  }
  artifact->size = static_cast<qint64>(size);
  if (!ReadPositiveInteger(object, QStringLiteral("file_count"), kMaximumFileCount,
                           &artifact->file_count) ||
      !ReadPositiveInteger(object, QStringLiteral("unpacked_bytes"), kMaximumUnpackedBytes,
                           &artifact->unpacked_bytes) ||
      !ReadSha256(object, QStringLiteral("inventory_sha256"), &artifact->inventory_sha256)) {
    return QStringLiteral("The %1 inventory of AI model package %2 is not valid.")
        .arg(platform, package_label);
  }
  return {};
}

auto ParsePackage(const QJsonValue& value, const QUrl& feed_url, AiModelPackageDescriptor* package)
    -> QString {
  if (!value.isObject()) return QStringLiteral("An AI model package entry is not an object.");
  const QJsonObject               object = value.toObject();
  static const QRegularExpression kSlug(QStringLiteral("^[a-z0-9]+(?:[._-][a-z0-9]+)*$"));
  package->id = object.value(QStringLiteral("id")).toString();
  if (package->id.size() > 96 || !kSlug.match(package->id).hasMatch()) {
    return QStringLiteral("An AI model package ID is not valid.");
  }
  if (!ReadPositiveInteger(object, QStringLiteral("model_set_version"), kMaximumModelSetVersion,
                           &package->model_set_version)) {
    return QStringLiteral("The model set version of AI model package %1 is not valid.")
        .arg(package->id);
  }
  const QString label = QStringLiteral("%1 v%2").arg(package->id).arg(package->model_set_version);
  if (const QJsonValue name = object.value(QStringLiteral("name")); !name.isUndefined()) {
    package->name = name.toString();
    if (!name.isString() || !IsDisplayText(package->name)) {
      return QStringLiteral("The name of AI model package %1 is not valid.").arg(label);
    }
  }
  static const QRegularExpression kRevision(QStringLiteral("^r[1-9][0-9]{0,8}$"));
  package->revision = object.value(QStringLiteral("revision")).toString();
  if (!kRevision.match(package->revision).hasMatch()) {
    return QStringLiteral("The revision of AI model package %1 is not valid.").arg(label);
  }
  if (!ParseLicenses(object.value(QStringLiteral("licenses")), package)) {
    return QStringLiteral("The license list of AI model package %1 is not valid.").arg(label);
  }
  const QJsonValue artifacts = object.value(QStringLiteral("artifacts"));
  if (!artifacts.isObject() || artifacts.toObject().isEmpty() ||
      artifacts.toObject().size() > kMaximumArtifacts) {
    return QStringLiteral("AI model package %1 has no artifact.").arg(label);
  }
  const QJsonObject artifact_map = artifacts.toObject();
  for (auto it = artifact_map.begin(); it != artifact_map.end(); ++it) {
    AiModelPackageArtifact artifact;
    if (QString error = ParseArtifact(it.key(), it.value(), feed_url, label, &artifact);
        !error.isEmpty()) {
      return error;
    }
    package->artifacts.push_back(std::move(artifact));
  }
  return {};
}

}  // namespace

auto AiModelPackageDescriptor::Artifact(const QString& platform) const
    -> const AiModelPackageArtifact* {
  for (const AiModelPackageArtifact& artifact : artifacts) {
    if (artifact.platform == platform) return &artifact;
  }
  return nullptr;
}

auto VerifyAiModelPackageManifest(const QByteArray& manifest_bytes,
                                  const QByteArray& signature_text, const QByteArray& public_key,
                                  const QUrl& feed_url, quint64 minimum_sequence)
    -> AiModelPackageManifestResult {
  switch (VerifyDetachedSignature(manifest_bytes, signature_text, public_key)) {
    case DetachedSignatureCheck::kValid:
      break;
    case DetachedSignatureCheck::kInvalidPublicKey:
      return Failure(QStringLiteral("The AI model package public key is not valid."));
    case DetachedSignatureCheck::kInvalidSignatureFormat:
      return Failure(QStringLiteral("The AI model package signature has an invalid format."));
    case DetachedSignatureCheck::kSignatureMismatch:
      return Failure(QStringLiteral("The AI model package signature is not valid."));
  }

  QJsonParseError     parse_error;
  const QJsonDocument document = QJsonDocument::fromJson(manifest_bytes, &parse_error);
  if (parse_error.error != QJsonParseError::NoError || !document.isObject()) {
    return Failure(QStringLiteral("The signed AI model package manifest is not valid JSON."));
  }
  const QJsonObject root = document.object();
  if (root.value(QStringLiteral("schema")).toDouble() != 1.0 ||
      root.value(QStringLiteral("kind")).toString() != QStringLiteral("alcedo-ai-model-packages")) {
    return Failure(QStringLiteral("The AI model package manifest schema is not supported."));
  }

  AiModelPackageManifest manifest;
  if (!ReadPositiveInteger(root, QStringLiteral("sequence"), 9007199254740991ULL,
                           &manifest.sequence) ||
      manifest.sequence < minimum_sequence) {
    return Failure(
        QStringLiteral("The AI model package manifest sequence is older than a trusted manifest."));
  }

  const QJsonValue packages_value = root.value(QStringLiteral("packages"));
  if (!packages_value.isArray() || packages_value.toArray().isEmpty() ||
      packages_value.toArray().size() > kMaximumPackages) {
    return Failure(QStringLiteral("The AI model package manifest has no valid package list."));
  }
  QSet<QPair<QString, quint64>> seen;
  for (const QJsonValue& value : packages_value.toArray()) {
    AiModelPackageDescriptor package;
    if (const QString error = ParsePackage(value, feed_url, &package); !error.isEmpty()) {
      return Failure(error);
    }
    const QPair<QString, quint64> key{package.id, package.model_set_version};
    if (seen.contains(key)) {
      return Failure(QStringLiteral("The AI model package manifest lists %1 v%2 twice.")
                         .arg(package.id)
                         .arg(package.model_set_version));
    }
    seen.insert(key);
    manifest.packages.push_back(std::move(package));
  }
  return AiModelPackageManifestResult{std::move(manifest), {}};
}

auto SelectAiModelPackage(const AiModelPackageManifest& manifest, const QString& package_id,
                          quint64 minimum_version, quint64 maximum_version, const QString& platform)
    -> const AiModelPackageDescriptor* {
  const AiModelPackageDescriptor* selected = nullptr;
  for (const AiModelPackageDescriptor& package : manifest.packages) {
    if (package.id != package_id || package.model_set_version < minimum_version ||
        package.model_set_version > maximum_version || package.Artifact(platform) == nullptr) {
      continue;
    }
    if (selected == nullptr || package.model_set_version > selected->model_set_version) {
      selected = &package;
    }
  }
  return selected;
}

auto ListsAiModelPackageAbove(const AiModelPackageManifest& manifest, const QString& package_id,
                              quint64 maximum_version, const QString& platform) -> bool {
  for (const AiModelPackageDescriptor& package : manifest.packages) {
    if (package.id == package_id && package.model_set_version > maximum_version &&
        package.Artifact(platform) != nullptr) {
      return true;
    }
  }
  return false;
}

}  // namespace alcedo
