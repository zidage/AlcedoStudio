//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/mask_thumbnail_image_provider.hpp"

#include <QSize>

namespace alcedo::ui {

auto MaskThumbnailImageStore::MakeUrl(std::uint64_t request_id) -> QString {
  return QStringLiteral("image://%1/%2")
      .arg(QLatin1String(kMaskThumbnailImageProviderId))
      .arg(request_id);
}

auto MaskThumbnailImageStore::ParseProviderId(const QString& id, std::uint64_t* request_id)
    -> bool {
  bool ok = false;
  const auto parsed = id.toULongLong(&ok);
  if (!ok || parsed == 0) {
    return false;
  }
  if (request_id != nullptr) {
    *request_id = parsed;
  }
  return true;
}

auto MaskThumbnailImageStore::Put(std::uint64_t request_id, QImage image) -> QString {
  if (request_id == 0 || image.isNull()) {
    return {};
  }
  {
    std::lock_guard lock(mutex_);
    images_[request_id] = std::move(image);
  }
  return MakeUrl(request_id);
}

void MaskThumbnailImageStore::Remove(std::uint64_t request_id) {
  std::lock_guard lock(mutex_);
  images_.erase(request_id);
}

auto MaskThumbnailImageStore::Get(std::uint64_t request_id) const -> QImage {
  std::lock_guard lock(mutex_);
  const auto found = images_.find(request_id);
  if (found == images_.end()) {
    return {};
  }
  return found->second;
}

MaskThumbnailImageProvider::MaskThumbnailImageProvider(
    std::shared_ptr<MaskThumbnailImageStore> store)
    : QQuickImageProvider(QQuickImageProvider::Image), store_(std::move(store)) {}

QImage MaskThumbnailImageProvider::requestImage(const QString& id, QSize* size,
                                                const QSize& requestedSize) {
  std::uint64_t request_id = 0;
  QImage        image;
  if (store_ && MaskThumbnailImageStore::ParseProviderId(id, &request_id)) {
    image = store_->Get(request_id);
  }
  if (size != nullptr) {
    *size = image.size();
  }
  if (!requestedSize.isEmpty() && !image.isNull()) {
    return image.scaled(requestedSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);
  }
  return image;
}

auto SharedMaskThumbnailImageStore() -> std::shared_ptr<MaskThumbnailImageStore> {
  static auto store = std::make_shared<MaskThumbnailImageStore>();
  return store;
}

}  // namespace alcedo::ui
