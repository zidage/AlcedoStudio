//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QByteArray>
#include <QString>
#include <QUrl>
#include <QVector>
#include <optional>

namespace alcedo {

/// One license of the models of a package: its name and its text file in the package.
struct AiModelPackageLicense {
  QString name;
  /// Package-relative path of the license text (for example `licenses/Apache-2.0.txt`).
  QString file;
};

/// The `.7z` archive of one package for one platform (exact archive bytes and the
/// `package-inventory.json` it contains).
struct AiModelPackageArtifact {
  /// `windows-x86_64` or `macos-arm64` (the platform keys of the update feed).
  QString    platform;
  QUrl       url;
  QByteArray sha256;
  qint64     size           = 0;
  /// Number of files that `package-inventory.json` lists.
  quint64    file_count     = 0;
  /// Sum of the listed file sizes.
  quint64    unpacked_bytes = 0;
  /// SHA-256 (32 raw bytes) of the exact `package-inventory.json` bytes in the archive.
  QByteArray inventory_sha256;
};

/// One (`id`, `model_set_version`) entry of the signed AI model feed.
struct AiModelPackageDescriptor {
  QString                         id;
  /// Optional display name; empty when the feed omits it.
  QString                         name;
  /// Changes only when the inference code must change (plan decision P1).
  quint64                         model_set_version = 0;
  /// `r1`, `r2`, ...: the weights of one model set version.
  QString                         revision;
  QVector<AiModelPackageLicense>  licenses;
  QVector<AiModelPackageArtifact> artifacts;

  /// The artifact for @p platform, or nullptr.
  [[nodiscard]] auto Artifact(const QString& platform) const -> const AiModelPackageArtifact*;
};

/// Verified `alcedo-ai-model-packages` feed. It has no expiration.
struct AiModelPackageManifest {
  quint64                           sequence = 0;
  QVector<AiModelPackageDescriptor> packages;
};

struct AiModelPackageManifestResult {
  std::optional<AiModelPackageManifest> manifest;
  QString                               error;

  [[nodiscard]] explicit                operator bool() const { return manifest.has_value(); }
};

/// Verify the detached Ed25519 signature over the exact @p manifest_bytes, then parse the AI
/// model package feed (docs/roadmap/alcedo_studio/ai/external_ai_model_download_plan.md, P2
/// and P3).
///
/// Rejects an older sequence than @p minimum_sequence, two entries with the same (`id`,
/// `model_set_version`), invalid digests, sizes, revisions, or license paths, and artifact
/// URLs that are not HTTPS `.7z` files on the host of @p feed_url. No timestamp is read, so
/// an old signed feed stays valid (LUT feed rule). Pure function.
[[nodiscard]] auto VerifyAiModelPackageManifest(const QByteArray& manifest_bytes,
                                                const QByteArray& signature_text,
                                                const QByteArray& public_key, const QUrl& feed_url,
                                                quint64 minimum_sequence)
    -> AiModelPackageManifestResult;

/// The entry of @p package_id with the highest `model_set_version` in
/// [@p minimum_version, @p maximum_version] that has an artifact for @p platform, or nullptr.
[[nodiscard]] auto SelectAiModelPackage(const AiModelPackageManifest& manifest,
                                        const QString& package_id, quint64 minimum_version,
                                        quint64 maximum_version, const QString& platform)
    -> const AiModelPackageDescriptor*;

/// True when @p manifest lists @p package_id with a `model_set_version` above
/// @p maximum_version for @p platform.
[[nodiscard]] auto ListsAiModelPackageAbove(const AiModelPackageManifest& manifest,
                                            const QString& package_id, quint64 maximum_version,
                                            const QString& platform) -> bool;

}  // namespace alcedo
