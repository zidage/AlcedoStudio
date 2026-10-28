//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QByteArray>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QObject>
#include <QString>
#include <QUrl>
#include <functional>

namespace alcedo {

/// Upper bound of a signed feed manifest (software update, LUT, and AI model feeds).
inline constexpr qint64 kMaximumSignedFeedManifestBytes  = 256 * 1024;
/// Upper bound of the detached base64 signature written by `alcedo_update_signer sign`.
inline constexpr qint64 kMaximumSignedFeedSignatureBytes = 1024;

/// Reason a small feed file was not read. Each caller words the error for its own feed.
enum class FeedFileError {
  kNone,
  /// The URL is not HTTPS, has user info, or is on another host than the feed.
  kUrlNotAllowed,
  /// The response is larger than the requested maximum.
  kTooLarge,
  /// The transfer failed; FeedFileResult::network_error has the reply's error text.
  kNetwork,
  /// The server answered with a status other than 200 (FeedFileResult::http_status).
  kHttpStatus,
  /// A redirect left HTTPS or the feed host.
  kUntrustedRedirect,
};

struct FeedFileResult {
  QByteArray    bytes;
  FeedFileError error = FeedFileError::kNone;
  QString       network_error;
  int           http_status = 0;
};

/// HTTPS GET of at most @p maximum_bytes from @p url on @p allowed_host, without a cache
/// and with a 15 s transfer timeout. A redirect may not leave HTTPS or the host.
///
/// Returns the reply (deleted after @p done runs) so that the caller can abort it, or nullptr
/// when the URL is not allowed; @p done then runs before the function returns. Otherwise
/// @p done runs on the thread of @p context once the reply finishes; it does not run when
/// @p context is destroyed first. Owner-thread function.
auto FetchFeedFile(QNetworkAccessManager& network, QObject* context, const QUrl& url,
                   const QString& allowed_host, qint64 maximum_bytes,
                   std::function<void(FeedFileResult)> done) -> QNetworkReply*;

/// Read at most @p maximum_bytes from @p url and call @p done with the bytes or an error text.
/// Services take this form so that tests can serve signed feeds without a network.
using FeedFileFetch =
    std::function<void(const QUrl& url, qint64 maximum_bytes,
                       std::function<void(QByteArray bytes, QString error)> done)>;

/// Read the signed manifest @p manifest_url and then its detached signature
/// (`<manifest path>.sig`) through @p fetch, and call @p done once with both byte arrays or
/// with the first error text. The signature is requested only after the manifest arrived.
/// Neither file is verified here; the caller verifies the exact bytes.
void FetchSignedFeed(
    const FeedFileFetch& fetch, const QUrl& manifest_url,
    std::function<void(QByteArray manifest, QByteArray signature, QString error)> done);

}  // namespace alcedo
