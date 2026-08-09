//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/thumbnail_image_provider.hpp"

#include <QImage>
#include <QSize>
#include <QString>
#include <QStringList>

namespace alcedo::ui {

namespace {

auto ProviderUrlPrefix() -> QString {
  return QStringLiteral("image://%1/").arg(QLatin1String(kThumbnailImageProviderId));
}

}  // namespace

auto ThumbnailImageStore::MakeUrl(sl_element_id_t element_id, uint32_t max_edge,
                                  quint64 revision) -> QString {
  return QStringLiteral("image://%1/%2/%3/%4")
      .arg(QLatin1String(kThumbnailImageProviderId))
      .arg(element_id)
      .arg(max_edge)
      .arg(revision);
}

auto ThumbnailImageStore::ParseProviderId(const QString& id, sl_element_id_t* element_id,
                                          uint32_t* max_edge, quint64* revision) -> bool {
  const QStringList parts = id.split(QLatin1Char('/'), Qt::SkipEmptyParts);
  if (parts.size() != 3) {
    return false;
  }

  bool ok_element = false;
  bool ok_edge    = false;
  bool ok_rev     = false;
  const auto parsed_element = parts[0].toUInt(&ok_element);
  const auto parsed_edge    = parts[1].toUInt(&ok_edge);
  const auto parsed_rev     = parts[2].toULongLong(&ok_rev);
  if (!ok_element || !ok_edge || !ok_rev || parsed_element == 0 || parsed_edge == 0) {
    return false;
  }

  if (element_id != nullptr) {
    *element_id = static_cast<sl_element_id_t>(parsed_element);
  }
  if (max_edge != nullptr) {
    *max_edge = parsed_edge;
  }
  if (revision != nullptr) {
    *revision = parsed_rev;
  }
  return true;
}

auto ThumbnailImageStore::Put(sl_element_id_t element_id, uint32_t max_edge, QImage image)
    -> QString {
  if (element_id == 0 || max_edge == 0 || image.isNull()) {
    return {};
  }

  const quint64 revision = next_revision_.fetch_add(1, std::memory_order_relaxed);
  {
    std::lock_guard lock(mutex_);
    images_[Key{element_id, max_edge}] = Entry{.image = std::move(image), .revision = revision};
  }
  return MakeUrl(element_id, max_edge, revision);
}

void ThumbnailImageStore::Remove(sl_element_id_t element_id, uint32_t max_edge) {
  std::lock_guard lock(mutex_);
  images_.erase(Key{element_id, max_edge});
}

void ThumbnailImageStore::RemoveElement(sl_element_id_t element_id) {
  std::lock_guard lock(mutex_);
  for (auto it = images_.begin(); it != images_.end();) {
    if (it->first.element_id == element_id) {
      it = images_.erase(it);
    } else {
      ++it;
    }
  }
}

void ThumbnailImageStore::Clear() {
  std::lock_guard lock(mutex_);
  images_.clear();
}

auto ThumbnailImageStore::Get(sl_element_id_t element_id, uint32_t max_edge) const -> QImage {
  std::lock_guard lock(mutex_);
  const auto      it = images_.find(Key{element_id, max_edge});
  if (it == images_.end()) {
    return {};
  }
  return it->second.image;
}

auto ThumbnailImageStore::GetByProviderId(const QString& id) const -> QImage {
  sl_element_id_t element_id = 0;
  uint32_t        max_edge   = 0;
  quint64         revision   = 0;
  if (!ParseProviderId(id, &element_id, &max_edge, &revision)) {
    return {};
  }
  // Revision is for QML cache-busting only. Shared consumers (grid / filmstrip /
  // search) may hold older URLs for the same key; serve the current pixels.
  (void)revision;
  return Get(element_id, max_edge);
}

auto ThumbnailImageStore::ResolveUrl(const QString& url) const -> QImage {
  const QString prefix = ProviderUrlPrefix();
  if (!url.startsWith(prefix)) {
    return {};
  }
  return GetByProviderId(url.mid(prefix.size()));
}

ThumbnailImageProvider::ThumbnailImageProvider(std::shared_ptr<ThumbnailImageStore> store)
    : QQuickImageProvider(QQuickImageProvider::Image), store_(std::move(store)) {}

QImage ThumbnailImageProvider::requestImage(const QString& id, QSize* size,
                                            const QSize& requestedSize) {
  if (!store_) {
    return {};
  }

  QImage image = store_->GetByProviderId(id);
  if (image.isNull()) {
    return {};
  }

  if (size != nullptr) {
    *size = image.size();
  }

  if (requestedSize.isValid() && requestedSize.width() > 0 && requestedSize.height() > 0 &&
      (image.width() > requestedSize.width() || image.height() > requestedSize.height())) {
    return image.scaled(requestedSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);
  }
  return image;
}

}  // namespace alcedo::ui
