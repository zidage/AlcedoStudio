//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QString>
#include <QUrl>
#include <optional>

namespace alcedo {

struct UpdateArtifact {
  QUrl       url;
  QByteArray sha256;
  qint64     size = 0;
};

struct UpdateManifest {
  quint64        sequence = 0;
  QString        version;
  quint64        build = 0;
  QDateTime      published_at;
  QDateTime      expires_at;
  QUrl           notes_url;
  QString        changelog;
  UpdateArtifact artifact;
};

struct UpdateManifestResult {
  std::optional<UpdateManifest> manifest;
  QString                       error;

  [[nodiscard]] explicit        operator bool() const { return manifest.has_value(); }
};

/// Verify a detached Ed25519 signature and parse a platform artifact.
///
/// The signature covers the exact manifest bytes. The manifest must use the
/// schema described in docs/update-system.md. Artifact URLs must use HTTPS and
/// the same host as @p feed_url.
[[nodiscard]] UpdateManifestResult VerifyUpdateManifest(
    const QByteArray& manifest_bytes, const QByteArray& signature_text,
    const QByteArray& public_key, const QString& platform_key, const QUrl& feed_url,
    quint64 minimum_sequence, const QDateTime& now_utc = QDateTime::currentDateTimeUtc());

}  // namespace alcedo
