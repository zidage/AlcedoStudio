//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/lut_package_manifest.hpp"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QRegularExpression>
#include <QSet>
#include <cmath>

#include "app/detached_signature.hpp"

namespace alcedo {
namespace {

constexpr quint64 kMaximumArtifactSize  = 4ULL * 1024ULL * 1024ULL * 1024ULL;
constexpr quint64 kMaximumUnpackedBytes = 16ULL * 1024ULL * 1024ULL * 1024ULL;
constexpr int     kMaximumPackages      = 64;
constexpr int     kMaximumRevisionChars = 64;

auto              Failure(QString message) -> LutPackageManifestResult {
  return LutPackageManifestResult{std::nullopt, std::move(message)};
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

auto ReadSha256(const QJsonObject& object, const QString& name, QByteArray* output) -> bool {
  const QString                   text = object.value(name).toString();
  static const QRegularExpression kHex(QStringLiteral("^[0-9a-f]{64}$"));
  if (!kHex.match(text).hasMatch()) {
    return false;
  }
  *output = QByteArray::fromHex(text.toLatin1());
  return output->size() == 32;
}

auto ParsePackage(const QJsonValue& value, const QUrl& feed_url, LutPackageDescriptor* package)
    -> QString {
  if (!value.isObject()) {
    return QStringLiteral("A LUT package entry is not an object.");
  }
  const QJsonObject               object = value.toObject();
  static const QRegularExpression kSlug(QStringLiteral("^[a-z0-9]+(?:[._-][a-z0-9]+)*$"));
  package->id = object.value(QStringLiteral("id")).toString();
  if (package->id.size() > 96 || !kSlug.match(package->id).hasMatch()) {
    return QStringLiteral("A LUT package ID is not valid.");
  }
  package->revision = object.value(QStringLiteral("revision")).toString().trimmed();
  if (package->revision.isEmpty() || package->revision.size() > kMaximumRevisionChars) {
    return QStringLiteral("The revision of LUT package %1 is not valid.").arg(package->id);
  }
  if (!ReadPositiveInteger(object, QStringLiteral("file_count"), &package->file_count)) {
    return QStringLiteral("The file count of LUT package %1 is not valid.").arg(package->id);
  }
  if (!ReadSha256(object, QStringLiteral("inventory_sha256"), &package->inventory_sha256)) {
    return QStringLiteral("The inventory digest of LUT package %1 is not valid.").arg(package->id);
  }
  if (!ReadPositiveInteger(object, QStringLiteral("unpacked_bytes"), &package->unpacked_bytes) ||
      package->unpacked_bytes > kMaximumUnpackedBytes) {
    return QStringLiteral("The unpacked size of LUT package %1 is not valid.").arg(package->id);
  }

  const QJsonValue artifact_value = object.value(QStringLiteral("artifact"));
  if (!artifact_value.isObject()) {
    return QStringLiteral("LUT package %1 has no artifact.").arg(package->id);
  }
  const QJsonObject artifact = artifact_value.toObject();
  package->artifact.url      = QUrl(artifact.value(QStringLiteral("url")).toString());
  const QUrl& url            = package->artifact.url;
  if (!url.isValid() || url.scheme() != QStringLiteral("https") ||
      url.host().compare(feed_url.host(), Qt::CaseInsensitive) != 0 || !url.userInfo().isEmpty() ||
      !url.path().endsWith(QStringLiteral(".7z"))) {
    return QStringLiteral("The archive URL of LUT package %1 is not allowed.").arg(package->id);
  }
  if (!ReadSha256(artifact, QStringLiteral("sha256"), &package->artifact.sha256)) {
    return QStringLiteral("The archive SHA-256 of LUT package %1 is not valid.").arg(package->id);
  }
  quint64 size = 0;
  if (!ReadPositiveInteger(artifact, QStringLiteral("size"), &size) ||
      size > kMaximumArtifactSize) {
    return QStringLiteral("The archive size of LUT package %1 is not valid.").arg(package->id);
  }
  package->artifact.size = static_cast<qint64>(size);
  return {};
}

}  // namespace

auto VerifyLutPackageManifest(const QByteArray& manifest_bytes, const QByteArray& signature_text,
                              const QByteArray& public_key, const QUrl& feed_url,
                              quint64 minimum_sequence) -> LutPackageManifestResult {
  switch (VerifyDetachedSignature(manifest_bytes, signature_text, public_key)) {
    case DetachedSignatureCheck::kValid:
      break;
    case DetachedSignatureCheck::kInvalidPublicKey:
      return Failure(QStringLiteral("The LUT package public key is not valid."));
    case DetachedSignatureCheck::kInvalidSignatureFormat:
      return Failure(QStringLiteral("The LUT package signature has an invalid format."));
    case DetachedSignatureCheck::kSignatureMismatch:
      return Failure(QStringLiteral("The LUT package signature is not valid."));
  }

  QJsonParseError     parse_error;
  const QJsonDocument document = QJsonDocument::fromJson(manifest_bytes, &parse_error);
  if (parse_error.error != QJsonParseError::NoError || !document.isObject()) {
    return Failure(QStringLiteral("The signed LUT package manifest is not valid JSON."));
  }
  const QJsonObject root = document.object();
  if (root.value(QStringLiteral("schema")).toDouble() != 1.0 ||
      root.value(QStringLiteral("kind")).toString() != QStringLiteral("alcedo-lut-packages")) {
    return Failure(QStringLiteral("The LUT package manifest schema is not supported."));
  }

  LutPackageManifest manifest;
  if (!ReadPositiveInteger(root, QStringLiteral("sequence"), &manifest.sequence) ||
      manifest.sequence < minimum_sequence) {
    return Failure(
        QStringLiteral("The LUT package manifest sequence is older than a trusted manifest."));
  }

  const QJsonValue packages_value = root.value(QStringLiteral("packages"));
  if (!packages_value.isArray() || packages_value.toArray().isEmpty() ||
      packages_value.toArray().size() > kMaximumPackages) {
    return Failure(QStringLiteral("The LUT package manifest has no valid package list."));
  }
  QSet<QString> seen_ids;
  for (const QJsonValue& value : packages_value.toArray()) {
    LutPackageDescriptor package;
    const QString        error = ParsePackage(value, feed_url, &package);
    if (!error.isEmpty()) {
      return Failure(error);
    }
    if (seen_ids.contains(package.id)) {
      return Failure(QStringLiteral("The LUT package manifest lists %1 twice.").arg(package.id));
    }
    seen_ids.insert(package.id);
    manifest.packages.push_back(std::move(package));
  }
  return LutPackageManifestResult{std::move(manifest), {}};
}

}  // namespace alcedo
