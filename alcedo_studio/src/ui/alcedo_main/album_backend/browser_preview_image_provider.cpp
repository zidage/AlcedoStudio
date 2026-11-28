//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/browser_preview_image_provider.hpp"

#include <QSize>
#include <utility>

namespace alcedo::ui {

BrowserPreviewImageStore::BrowserPreviewImageStore(QString provider_id)
    : provider_id_(std::move(provider_id)) {}

auto BrowserPreviewImageStore::MakeUrl(std::uint64_t display_id) const -> QString {
  return QStringLiteral("image://%1/%2").arg(provider_id_).arg(display_id);
}

auto BrowserPreviewImageStore::ParseProviderId(const QString& id, std::uint64_t* display_id)
    -> bool {
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

auto BrowserPreviewImageStore::Put(std::uint64_t display_id, QImage image,
                                   const ViewerDisplayConfig& encoding) -> QString {
  if (display_id == 0 || image.isNull()) {
    return {};
  }
  {
    std::scoped_lock lock(mutex_);
    images_[display_id] = Entry{std::move(image), encoding};
  }
  return MakeUrl(display_id);
}

void BrowserPreviewImageStore::Remove(std::uint64_t display_id) {
  std::scoped_lock lock(mutex_);
  images_.erase(display_id);
}

auto BrowserPreviewImageStore::Get(std::uint64_t display_id) const -> QImage {
  std::scoped_lock lock(mutex_);
  const auto       found = images_.find(display_id);
  return found == images_.end() ? QImage{} : found->second.image;
}

auto BrowserPreviewImageStore::GetForDisplay(std::uint64_t display_id) const -> QImage {
  Entry entry;
  {
    std::scoped_lock lock(mutex_);
    const auto       found = images_.find(display_id);
    if (found == images_.end()) {
      return {};
    }
    entry = found->second;
  }
  // The conversion runs outside the lock: the GUI thread can put and remove meanwhile.
  return converter_.Convert(entry.image, entry.encoding);
}

void BrowserPreviewImageStore::SetDestination(
    std::shared_ptr<const DisplayDestination> destination) {
  converter_.SetDestination(std::move(destination));
}

auto BrowserPreviewImageStore::Destination() const -> std::shared_ptr<const DisplayDestination> {
  return converter_.Destination();
}

auto BrowserPreviewImageStore::size() const -> std::size_t {
  std::scoped_lock lock(mutex_);
  return images_.size();
}

BrowserPreviewImageProvider::BrowserPreviewImageProvider(
    std::shared_ptr<BrowserPreviewImageStore> store)
    : QQuickImageProvider(QQuickImageProvider::Image), store_(std::move(store)) {}

QImage BrowserPreviewImageProvider::requestImage(const QString& id, QSize* size,
                                                 const QSize& requestedSize) {
  std::uint64_t display_id = 0;
  QImage        image;
  if (store_ && BrowserPreviewImageStore::ParseProviderId(id, &display_id)) {
    image = store_->GetForDisplay(display_id);
  }
  if (size != nullptr) {
    *size = image.size();
  }
  if (!requestedSize.isEmpty() && !image.isNull()) {
    return image.scaled(requestedSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);
  }
  return image;
}

auto SharedLutPreviewImageStore() -> std::shared_ptr<BrowserPreviewImageStore> {
  static auto store =
      std::make_shared<BrowserPreviewImageStore>(QString::fromUtf8(kLutPreviewImageProviderId));
  return store;
}

auto SharedPresetPreviewImageStore() -> std::shared_ptr<BrowserPreviewImageStore> {
  static auto store =
      std::make_shared<BrowserPreviewImageStore>(QString::fromUtf8(kPresetPreviewImageProviderId));
  return store;
}

auto SharedPresetDialogPreviewImageStore() -> std::shared_ptr<BrowserPreviewImageStore> {
  static auto store = std::make_shared<BrowserPreviewImageStore>(
      QString::fromUtf8(kPresetDialogPreviewImageProviderId));
  return store;
}

}  // namespace alcedo::ui
