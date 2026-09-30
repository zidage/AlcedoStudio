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

/// Download descriptor of one package archive (exact 7z bytes).
struct LutPackageArtifact {
  QUrl       url;
  QByteArray sha256;
  qint64     size = 0;
};

/// One official LUT package listed by the signed feed.
struct LutPackageDescriptor {
  QString            id;
  QString            revision;
  quint64            file_count = 0;
  /// Canonical inventory digest (32 raw bytes); see ComputeLutInventoryDigest.
  QByteArray         inventory_sha256;
  quint64            unpacked_bytes = 0;
  LutPackageArtifact artifact;
};

/// Verified `alcedo-lut-packages` feed. It has no expiration.
struct LutPackageManifest {
  quint64                       sequence = 0;
  QVector<LutPackageDescriptor> packages;
};

struct LutPackageManifestResult {
  std::optional<LutPackageManifest> manifest;
  QString                           error;

  [[nodiscard]] explicit            operator bool() const { return manifest.has_value(); }
};

/// Verify the detached Ed25519 signature over the exact @p manifest_bytes, then
/// parse the LUT package feed (docs/lut-package-system.md section 3).
///
/// Rejects an older sequence than @p minimum_sequence, duplicate package IDs,
/// invalid digests or sizes, and artifact URLs that are not HTTPS `.7z` files on
/// the host of @p feed_url. No timestamp is read or enforced, so an old signed
/// feed stays valid. This is separate from VerifyUpdateManifest, whose expiry
/// rule is unchanged. Pure function.
[[nodiscard]] auto VerifyLutPackageManifest(const QByteArray& manifest_bytes,
                                            const QByteArray& signature_text,
                                            const QByteArray& public_key, const QUrl& feed_url,
                                            quint64 minimum_sequence) -> LutPackageManifestResult;

}  // namespace alcedo
