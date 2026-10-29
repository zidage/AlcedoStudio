//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/lut_preview_image_provider.hpp"

#include <QSize>
#include <utility>

namespace alcedo::ui {

auto LutPreviewImageStore::MakeUrl(std::uint64_t display_id) -> QString {
  return QStringLiteral("image://%1/%2")
      .arg(QLatin1String(kLutPreviewImageProviderId))
      .arg(display_id);
}

auto LutPreviewImageStore::ParseProviderId(const QString& id, std::uint64_t* display_id) -> bool {
  bool       ok     = false;
  const auto parsed = id.toULongLong(&ok);
  if (!ok || parsed == 0) {
    return false;
  }
  if (display_id != nullptr) {
    *display_id = parsed;
  }
  return true;
}

auto LutPreviewImageStore::Put(std::uint64_t display_id, QImage image) -> QString {
  if (display_id == 0 || image.isNull()) {
    return {};
  }
  {
    std::scoped_lock lock(mutex_);
    images_[display_id] = std::move(image);
  }
  return MakeUrl(display_id);
}

void LutPreviewImageStore::Remove(std::uint64_t display_id) {
  std::scoped_lock lock(mutex_);
  images_.erase(display_id);
}

auto LutPreviewImageStore::Get(std::uint64_t display_id) const -> QImage {
  std::scoped_lock lock(mutex_);
  const auto       found = images_.find(display_id);
  return found == images_.end() ? QImage{} : found->second;
}

auto LutPreviewImageStore::size() const -> std::size_t {
  std::scoped_lock lock(mutex_);
  return images_.size();
}

LutPreviewImageProvider::LutPreviewImageProvider(std::shared_ptr<LutPreviewImageStore> store)
    : QQuickImageProvider(QQuickImageProvider::Image), store_(std::move(store)) {}

QImage LutPreviewImageProvider::requestImage(const QString& id, QSize* size,
                                             const QSize& requestedSize) {
  std::uint64_t display_id = 0;
  QImage        image;
  if (store_ && LutPreviewImageStore::ParseProviderId(id, &display_id)) {
    image = store_->Get(display_id);
  }
  if (size != nullptr) {
    *size = image.size();
  }
  if (!requestedSize.isEmpty() && !image.isNull()) {
    return image.scaled(requestedSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);
  }
  return image;
}

auto SharedLutPreviewImageStore() -> std::shared_ptr<LutPreviewImageStore> {
  static auto store = std::make_shared<LutPreviewImageStore>();
  return store;
}

}  // namespace alcedo::ui
