//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/thumbnail_manager.hpp"

#include "ui/alcedo_main/album_backend/library_module.hpp"
#include "ui/alcedo_main/album_backend/project_module.hpp"
#include "ui/alcedo_main/album_backend/path_utils.hpp"
#include "ui/alcedo_main/album_backend/thumbnail_image_provider.hpp"

#include <QCoreApplication>
#include <QImage>
#include <QMetaObject>
#include <QPointer>

#include <algorithm>
#include <atomic>
#include <exception>
#include <filesystem>
#include <system_error>
#include <thread>
#include <vector>

#include "app/thumbnail_service.hpp"
#include "image/image.hpp"
#include "image/image_buffer.hpp"

namespace alcedo::ui {

namespace {

// Convert a raw max-edge pixel value to the nearest fixed resolution tier.
ThumbnailResolution NearestResolutionTier(uint32_t max_edge) {
  if (max_edge <= 256)  return ThumbnailResolution::k256;
  if (max_edge <= 512)  return ThumbnailResolution::k512;
  if (max_edge <= 1024) return ThumbnailResolution::k1024;
  return ThumbnailResolution::k2048;
}

auto MakeThumbnailKey(sl_element_id_t element_id, uint32_t max_edge) -> ThumbnailCacheKey {
  return ThumbnailCacheKey{element_id, NearestResolutionTier(max_edge)};
}

auto CurrentExceptionText(const char* fallback) -> QString {
  try {
    throw;
  } catch (const std::exception& e) {
    return QString::fromUtf8(e.what());
  } catch (...) {
    return QString::fromUtf8(fallback);
  }
}

auto IsRetryableEmptyThumbnailResult(const ThumbnailRequestResult& result) -> bool {
  return result.status == ThumbnailRequestStatus::kError &&
         result.message.find("produced no thumbnail buffer") != std::string::npos;
}

}  // namespace

auto ThumbnailManager::PathExists(const std::filesystem::path& path) -> bool {
  if (path.empty()) {
    return false;
  }

  std::error_code exists_error;
  return std::filesystem::exists(path, exists_error) && !exists_error;
}

auto ThumbnailManager::ResolveThumbnailSourcePath(sl_element_id_t elementId,
                                                  image_id_t imageId) const
    -> std::filesystem::path {
  auto proj = library_.project()->handler().project();
  if (proj) {
    try {
      auto image_path = proj->GetImagePoolService()->Read<std::filesystem::path>(
          imageId, [](const std::shared_ptr<Image>& image) -> std::filesystem::path {
            if (!image) {
              return {};
            }
            return image->image_path_;
          });
      if (!image_path.empty()) {
        return image_path;
      }
    } catch (...) {
      return {};
    }
  }

  if (const auto* item = library_.FindAlbumItem(elementId);
      item != nullptr && item->file_path_.is_absolute() && !item->file_path_.empty()) {
    return item->file_path_;
  }

  return {};
}

ThumbnailManager::ThumbnailManager(LibraryModule& library) : library_(library) {}

void ThumbnailManager::SetOccurrenceThumbnailVisible(const std::string& group_key,
                                                     sl_element_id_t elementId, image_id_t imageId,
                                                     bool visible, uint32_t maxEdge) {
  if (elementId == 0 || imageId == 0) {
    return;
  }
  const auto occurrence = std::make_tuple(group_key, elementId, maxEdge);
  if (visible) {
    if (visible_occurrences_.insert(occurrence).second) {
      SetThumbnailVisible(elementId, imageId, true, maxEdge);
    }
    return;
  }
  if (visible_occurrences_.erase(occurrence) > 0) {
    SetThumbnailVisible(elementId, imageId, false, maxEdge);
  }
}

auto ThumbnailManager::VisibleOccurrenceCount(sl_element_id_t elementId) const -> size_t {
  size_t count = 0;
  for (const auto& occurrence : visible_occurrences_) {
    if (std::get<1>(occurrence) == elementId) {
      ++count;
    }
  }
  return count;
}

void ThumbnailManager::SetThumbnailVisible(sl_element_id_t elementId, image_id_t imageId,
                                           bool visible, uint32_t maxEdge) {
  if (elementId == 0 || imageId == 0) {
    return;
  }

  auto thumb_svc = library_.project()->handler().thumbnail_service();
  const auto key = MakeThumbnailKey(elementId, maxEdge);

  if (visible) {
    if (!thumb_svc) {
      return;
    }

    auto pin_it = thumbnail_pins_.find(key);
    if (pin_it != thumbnail_pins_.end()) {
      auto& pin = pin_it->second;
      if (pin.image_id_ == imageId) {
        pin.ref_count_++;
        const auto* item = library_.FindAlbumItem(elementId);
        if (item != nullptr && item->thumb_data_url.isEmpty() && !item->thumb_loading &&
            item->thumb_error_text.isEmpty()) {
          RequestThumbnail(elementId, imageId, maxEdge);
        }
        return;
      }

      ReleasePinnedThumbnailRequest(key);
      try {
        // Same element/key now points at different image content. Treat this
        // as content invalidation rather than a zoom-tier release.
        thumb_svc->InvalidateThumbnail(elementId);
      } catch (...) {
      }
      thumbnail_pins_[key] = {.ref_count_ = 1,
                              .image_id_ = imageId,
                              .resolution_ = key.resolution};
    } else {
      // Pins of other tiers belong to other consumers (the grid and the Editor filmstrip can
      // show the same element at different tiers). A consumer that changes its own tier
      // releases its old tier itself.
      thumbnail_pins_[key] = {.ref_count_ = 1,
                              .image_id_ = imageId,
                              .resolution_ = key.resolution};
    }

    const auto* item = library_.FindAlbumItem(elementId);
    const bool known_missing = item != nullptr && item->thumb_missing_source;
    const auto source_path = ResolveThumbnailSourcePath(elementId, imageId);
    if (known_missing && !source_path.empty() && !PathExists(source_path)) {
      UpdateThumbnailState(elementId, QString(), false, true,
                           QObject::tr("Source file was moved or deleted: %1")
                               .arg(album_util::PathToQString(source_path)));
      return;
    }
    RequestThumbnail(elementId, imageId, maxEdge);
    return;
  }

  const auto it = thumbnail_pins_.find(key);
  if (it == thumbnail_pins_.end()) {
    return;
  }

  if (it->second.ref_count_ > 1) {
    it->second.ref_count_--;
    return;
  }

  thumbnail_pins_.erase(it);

  // Strategy B: mark only this request key as inactive.
  DeactivateThumbnailRequest(key);
  image_store_->Remove(elementId, static_cast<uint32_t>(key.resolution));

  const auto* item = library_.FindAlbumItem(elementId);
  const bool  missing_source = item != nullptr && item->thumb_missing_source;
  if (!IsThumbnailPinned(elementId)) {
    UpdateThumbnailState(elementId, QString(), false, missing_source);
  } else {
    ShowRemainingPinnedTier(elementId);
  }
  if (thumb_svc) {
    try {
      thumb_svc->ReleaseThumbnail(key);
    } catch (...) {
    }
  }
}

void ThumbnailManager::ShowRemainingPinnedTier(sl_element_id_t elementId) {
  const ThumbnailCacheKey* remaining_key      = nullptr;
  image_id_t               remaining_image_id = 0;
  for (const auto& [key, pin] : thumbnail_pins_) {
    if (key.element_id == elementId && pin.ref_count_ > 0 &&
        (remaining_key == nullptr || key.resolution > remaining_key->resolution)) {
      remaining_key      = &key;
      remaining_image_id = pin.image_id_;
    }
  }
  if (remaining_key == nullptr) {
    return;
  }

  const ThumbnailCacheKey key = *remaining_key;

  // The row may still carry the released tier's URL, whose pixels just left the store.
  const auto    max_edge = static_cast<uint32_t>(key.resolution);
  const QString url      = image_store_->CurrentUrl(elementId, max_edge);
  if (!url.isEmpty()) {
    UpdateThumbnailState(elementId, url, false, false);
    return;
  }
  // A request in flight publishes the remaining tier. Until then the row keeps its URL, so a card
  // that already shows the released tier keeps those pixels instead of going blank.
  if (!thumbnail_active_flags_.contains(key)) {
    RequestThumbnail(elementId, remaining_image_id, max_edge);
  }
}

void ThumbnailManager::FinishThumbnailRequest(const ThumbnailCacheKey&                  key,
                                              const std::shared_ptr<std::atomic<bool>>& is_active) {
  const auto flag_it = thumbnail_active_flags_.find(key);
  if (flag_it != thumbnail_active_flags_.end() && flag_it->second == is_active) {
    thumbnail_active_flags_.erase(flag_it);
  }
}

void ThumbnailManager::RequestThumbnail(sl_element_id_t elementId, image_id_t imageId,
                                        uint32_t maxEdge, int retryAttempt) {
  auto thumb_svc = library_.project()->handler().thumbnail_service();
  if (!thumb_svc) {
    return;
  }

  const auto key = MakeThumbnailKey(elementId, maxEdge);

  const auto* item = library_.FindAlbumItem(elementId);
  const QString existing_data_url = item != nullptr ? item->thumb_data_url : QString{};
  UpdateThumbnailState(elementId, existing_data_url, true, false);

  // Strategy B: create active flag for this request key.
  // When this key is unpinned, the flag is set to false,
  // and the callback skips QImage conversion.
  auto is_active = std::make_shared<std::atomic<bool>>(true);
  {
    // Invalidate any previous flag for this exact key.
    auto old_it = thumbnail_active_flags_.find(key);
    if (old_it != thumbnail_active_flags_.end() && old_it->second) {
      old_it->second->store(false);
    }
    thumbnail_active_flags_[key] = is_active;
  }

  // The service stores this callback in its pending requests until the render answers, and the
  // dispatcher queues it on the application. A strong reference would keep the service, its
  // pipeline, and the project database alive after the project closes, so capture a weak one.
  std::weak_ptr<ThumbnailService> weak_service = thumb_svc;
  QPointer<LibraryModule>         self(&library_);

  CallbackDispatcher dispatcher = [](std::function<void()> fn) {
    auto* app = QCoreApplication::instance();
    if (!app) {
      fn();
      return;
    }
    QMetaObject::invokeMethod(app, std::move(fn), Qt::QueuedConnection);
  };

  try {
    thumb_svc->GetThumbnailDetailed(
        elementId, imageId,
        [self, weak_service, elementId, imageId, maxEdge, key, retryAttempt,
         is_active](ThumbnailRequestResult result) {
        // Strategy B: skip QImage conversion if this request was cancelled.
        if (!is_active || !is_active->load()) {
          return;
        }

        if (result.status == ThumbnailRequestStatus::kCanceled) {
          return;
        }

        auto guard = std::move(result.guard);
        if (!guard || !guard->thumbnail_buffer_) {
          if (self) {
            if (IsRetryableEmptyThumbnailResult(result) && self->thumbs().IsThumbnailPinned(key) &&
                retryAttempt < 1) {
              self->thumbs().RequestThumbnail(elementId, imageId, maxEdge, retryAttempt + 1);
              return;
            }

            const auto source_path = self->thumbs().ResolveThumbnailSourcePath(elementId, imageId);
            const bool missing_source = !source_path.empty() && !PathExists(source_path);
            const QString error_text = [&]() {
              if (missing_source) {
                return QObject::tr("Source file was moved or deleted: %1")
                    .arg(album_util::PathToQString(source_path));
              }
              if (!result.message.empty()) {
                return QString::fromUtf8(result.message);
              }
              return QObject::tr("Thumbnail render returned no image.");
            }();
            self->thumbs().FinishThumbnailRequest(key, is_active);
            self->thumbs().UpdateThumbnailState(elementId, QString(), false, missing_source,
                                              error_text);
          }
          if (self && !self->thumbs().IsThumbnailPinned(key)) {
            try {
              if (auto service = weak_service.lock()) {
                service->ReleaseThumbnail(key);
              }
            } catch (...) {
            }
          }
          return;
        }
        if (!self) {
          try {
            if (auto service = weak_service.lock()) {
              service->ReleaseThumbnail(key);
            }
          } catch (...) {
          }
          return;
        }

        auto image_store = self->thumbs().image_store();
        std::thread([self, weak_service, elementId, maxEdge, key, is_active, image_store,
                     guard = std::move(guard)]() mutable {
          // Strategy B: re-check before expensive conversion.
          if (!is_active || !is_active->load()) {
            return;
          }

          QString thumbUrl;
          QString errorText;
          try {
            auto* buffer = guard->thumbnail_buffer_.get();
            if (!buffer) {
              errorText = QObject::tr("Thumbnail render returned no image buffer.");
            } else {
              if (!buffer->cpu_data_valid_ && buffer->gpu_data_valid_) {
                buffer->SyncToCPU();
              }
              if (buffer->cpu_data_valid_) {
                QImage image = album_util::MatRgba32fToQImageCopy(buffer->GetCPUData());
                if (!image.isNull()) {
                  const int scaled_max_edge = static_cast<int>(std::max<uint32_t>(1, maxEdge));
                  QImage scaled = image.scaled(scaled_max_edge, scaled_max_edge,
                                               Qt::KeepAspectRatio, Qt::SmoothTransformation);
                  if (image_store) {
                    const uint32_t store_edge = static_cast<uint32_t>(key.resolution);
                    thumbUrl =
                        image_store->Put(elementId, store_edge, std::move(scaled));
                  }
                  if (thumbUrl.isEmpty()) {
                    errorText =
                        QObject::tr("Thumbnail conversion produced no image provider URL.");
                  }
                } else {
                  errorText =
                      QObject::tr("Thumbnail CPU buffer could not be converted to an image.");
                }
              } else {
                errorText = QObject::tr("Thumbnail render did not produce CPU image data.");
              }
            }
          } catch (...) {
            errorText = CurrentExceptionText("Unknown thumbnail conversion error.");
          }

          if (self) {
            QMetaObject::invokeMethod(
                self,
                [self, weak_service, elementId, key, thumbUrl, errorText, is_active]() {
                  if (!self) {
                    return;
                  }

                  // Strategy B: final check before emitting signal.
                  if (!is_active || !is_active->load()) {
                    return;
                  }

                  self->thumbs().FinishThumbnailRequest(key, is_active);
                  const bool pinned = self->thumbs().IsThumbnailPinned(key);
                  if (pinned) {
                    const bool render_error = thumbUrl.isEmpty();
                    self->thumbs().UpdateThumbnailState(
                        elementId, thumbUrl, false, false,
                        render_error
                            ? (errorText.isEmpty()
                                   ? QObject::tr("Thumbnail conversion produced no image.")
                                   : errorText)
                            : QString{});
                  } else if (!self->thumbs().IsThumbnailPinned(elementId)) {
                    self->thumbs().image_store()->RemoveElement(elementId);
                    self->thumbs().UpdateThumbnailState(elementId, QString(), false, false);
                  }
                  if (!pinned) {
                    try {
                      if (auto service = weak_service.lock()) {
                        service->ReleaseThumbnail(key);
                      }
                    } catch (...) {
                    }
                  }
                },
                Qt::QueuedConnection);
          }
        }).detach();
        },
        true, dispatcher, key.resolution);
  } catch (...) {
    UpdateThumbnailState(elementId, QString(), false, false,
                         CurrentExceptionText("Unknown thumbnail request error."));
  }
}

bool ThumbnailManager::RefreshCurrentThumbnail(sl_element_id_t elementId, image_id_t imageId) {
  if (elementId == 0 || imageId == 0) {
    return false;
  }

  // Every pinned tier is refreshed: the grid and the Editor filmstrip can pin the same element
  // at different tiers, and either tier can become the one the row shows.
  std::vector<ThumbnailCacheKey> pinned_keys;
  for (const auto& [key, pin] : thumbnail_pins_) {
    if (key.element_id == elementId && pin.image_id_ == imageId && pin.ref_count_ > 0) {
      pinned_keys.push_back(key);
    }
  }
  for (const auto& key : pinned_keys) {
    RequestThumbnail(elementId, imageId, static_cast<uint32_t>(key.resolution));
  }
  return !pinned_keys.empty();
}

void ThumbnailManager::UpdateThumbnailState(sl_element_id_t elementId, const QString& dataUrl,
                                            bool loading, bool missingSource,
                                            const QString& errorText) {
  auto* item = library_.FindAlbumItem(elementId);
  if (!item) {
    return;
  }

  if (item->thumb_data_url == dataUrl && item->thumb_loading == loading &&
      item->thumb_missing_source == missingSource && item->thumb_error_text == errorText) {
    return;
  }

  item->thumb_data_url        = dataUrl;
  item->thumb_loading         = loading;
  item->thumb_missing_source  = missingSource;
  item->thumb_error_text      = errorText;

  library_.model().updateThumbnailState(elementId, dataUrl, loading, missingSource,
                                                  errorText);

  library_.EmitThumbnailUpdated(static_cast<uint>(elementId), dataUrl, loading, missingSource,
                                 errorText);
}

bool ThumbnailManager::IsThumbnailPinned(sl_element_id_t elementId) const {
  for (const auto& [key, pin] : thumbnail_pins_) {
    if (key.element_id == elementId && pin.ref_count_ > 0) {
      return true;
    }
  }
  return false;
}

bool ThumbnailManager::IsThumbnailPinned(const ThumbnailCacheKey& key) const {
  const auto it = thumbnail_pins_.find(key);
  return it != thumbnail_pins_.end() && it->second.ref_count_ > 0;
}

void ThumbnailManager::ReleaseStoreImageIfUnpinned(const ThumbnailCacheKey& key) {
  if (!IsThumbnailPinned(key)) {
    image_store_->Remove(key.element_id, static_cast<uint32_t>(key.resolution));
  }
}

void ThumbnailManager::DeactivateThumbnailRequest(const ThumbnailCacheKey& key) {
  auto flag_it = thumbnail_active_flags_.find(key);
  if (flag_it != thumbnail_active_flags_.end() && flag_it->second) {
    flag_it->second->store(false);
  }
  thumbnail_active_flags_.erase(key);
}

void ThumbnailManager::ReleasePinnedThumbnailRequest(const ThumbnailCacheKey& key) {
  auto it = thumbnail_pins_.find(key);
  if (it != thumbnail_pins_.end()) {
    thumbnail_pins_.erase(it);
  }
  DeactivateThumbnailRequest(key);
  image_store_->Remove(key.element_id, static_cast<uint32_t>(key.resolution));

  auto thumb_svc = library_.project()->handler().thumbnail_service();
  if (thumb_svc) {
    try {
      thumb_svc->ReleaseThumbnail(key);
    } catch (...) {
    }
  }
}

void ThumbnailManager::RemoveThumbnailState(sl_element_id_t elementId, image_id_t imageId) {
  (void)imageId;
  if (elementId == 0) {
    return;
  }

  std::vector<ThumbnailCacheKey> keys_to_release;
  for (const auto& [key, pin] : thumbnail_pins_) {
    (void)pin;
    if (key.element_id == elementId) {
      keys_to_release.push_back(key);
    }
  }
  for (const auto& key : keys_to_release) {
    thumbnail_pins_.erase(key);
    DeactivateThumbnailRequest(key);
  }

  // Strategy B: invalidate active flags for every request key under this element.
  std::vector<ThumbnailCacheKey> flags_to_release;
  for (const auto& [key, flag] : thumbnail_active_flags_) {
    (void)flag;
    if (key.element_id == elementId) {
      flags_to_release.push_back(key);
    }
  }
  for (const auto& key : flags_to_release) {
    DeactivateThumbnailRequest(key);
  }

  image_store_->RemoveElement(elementId);
  UpdateThumbnailState(elementId, QString(), false, false);

  auto thumb_svc = library_.project()->handler().thumbnail_service();
  if (!thumb_svc) {
    return;
  }

  try {
    thumb_svc->CancelPending(elementId);
    thumb_svc->InvalidateThumbnail(elementId);
  } catch (...) {
  }
}

void ThumbnailManager::ReleaseVisibleThumbnailPins() {
  visible_occurrences_.clear();
  if (thumbnail_pins_.empty()) {
    return;
  }

  auto thumb_svc = library_.project()->handler().thumbnail_service();

  const auto pinned_keys = [&]() {
    std::vector<ThumbnailCacheKey> keys;
    keys.reserve(thumbnail_pins_.size());
    for (const auto& [key, pin] : thumbnail_pins_) {
      (void)pin;
      keys.push_back(key);
    }
    return keys;
  }();

  for (const auto& key : pinned_keys) {
    auto* item = library_.FindAlbumItem(key.element_id);
    if (item) {
      item->thumb_data_url.clear();
      item->thumb_loading = false;
    }

    DeactivateThumbnailRequest(key);

    if (thumb_svc) {
      try {
        thumb_svc->ReleaseThumbnail(key);
      } catch (...) {
      }
    }
  }
  thumbnail_pins_.clear();
  thumbnail_active_flags_.clear();
  image_store_->Clear();
}

}  // namespace alcedo::ui
