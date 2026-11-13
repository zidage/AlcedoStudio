//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/signed_feed_fetch.hpp"

#include <QNetworkRequest>
#include <memory>
#include <utility>

namespace alcedo {
namespace {

auto IsAllowedFeedUrl(const QUrl& url, const QString& allowed_host) -> bool {
  return url.scheme() == QStringLiteral("https") &&
         url.host().compare(allowed_host, Qt::CaseInsensitive) == 0 && url.userInfo().isEmpty();
}

auto NewFeedRequest(const QUrl& url) -> QNetworkRequest {
  QNetworkRequest request(url);
  request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                       QNetworkRequest::NoLessSafeRedirectPolicy);
  request.setTransferTimeout(15000);
  request.setRawHeader("Accept", "application/json, text/plain;q=0.9");
  request.setRawHeader("Cache-Control", "no-cache");
  return request;
}

}  // namespace

auto FetchFeedFile(QNetworkAccessManager& network, QObject* context, const QUrl& url,
                   const QString& allowed_host, qint64 maximum_bytes,
                   std::function<void(FeedFileResult)> done) -> QNetworkReply* {
  if (allowed_host.isEmpty() || !IsAllowedFeedUrl(url, allowed_host)) {
    done(FeedFileResult{.error = FeedFileError::kUrlNotAllowed});
    return nullptr;
  }
  QNetworkReply* reply   = network.get(NewFeedRequest(url));
  auto           payload = std::make_shared<QByteArray>();
  QObject::connect(reply, &QNetworkReply::readyRead, context, [reply, payload, maximum_bytes] {
    payload->append(reply->readAll());
    if (payload->size() > maximum_bytes) reply->abort();
  });
  QObject::connect(
      reply, &QNetworkReply::finished, context,
      [reply, payload, maximum_bytes, allowed_host, done = std::move(done)] {
        payload->append(reply->readAll());
        FeedFileResult result;
        // An oversized response is aborted, so its size is checked before the reply error.
        if (payload->size() > maximum_bytes) {
          result.error = FeedFileError::kTooLarge;
        } else if (reply->error() != QNetworkReply::NoError) {
          result.error         = FeedFileError::kNetwork;
          result.network_error = reply->errorString();
        } else if (const int status =
                       reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
                   status != 200) {
          result.error       = FeedFileError::kHttpStatus;
          result.http_status = status;
        } else if (!IsAllowedFeedUrl(reply->url(), allowed_host)) {
          result.error = FeedFileError::kUntrustedRedirect;
        } else {
          result.bytes = std::move(*payload);
        }
        reply->deleteLater();
        done(std::move(result));
      });
  return reply;
}

void FetchSignedFeed(
    const FeedFileFetch& fetch, const QUrl& manifest_url,
    std::function<void(QByteArray manifest, QByteArray signature, QString error)> done) {
  QUrl signature_url = manifest_url;
  signature_url.setPath(signature_url.path() + QStringLiteral(".sig"));
  fetch(manifest_url, kMaximumSignedFeedManifestBytes,
        [fetch, signature_url, done = std::move(done)](QByteArray manifest, QString error) mutable {
          if (!error.isEmpty()) {
            done({}, {}, std::move(error));
            return;
          }
          fetch(signature_url, kMaximumSignedFeedSignatureBytes,
                [manifest = std::move(manifest), done = std::move(done)](
                    QByteArray signature, QString signature_error) mutable {
                  if (!signature_error.isEmpty()) {
                    done({}, {}, std::move(signature_error));
                    return;
                  }
                  done(std::move(manifest), std::move(signature), {});
                });
        });
}

}  // namespace alcedo
