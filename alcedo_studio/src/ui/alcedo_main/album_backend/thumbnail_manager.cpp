//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/thumbnail_manager.hpp"

#include "ui/alcedo_main/album_backend/album_backend.hpp"
#include "ui/alcedo_main/album_backend/path_utils.hpp"

#include <QCoreApplication>
#include <QImage>
#include <QMetaObject>
#include <QPointer>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <system_error>
#include <thread>

#include <opencv2/opencv.hpp>

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
  if (const auto* item = backend_.FindAlbumItem(elementId);
      item != nullptr && !item->file_path_.empty()) {
    return item->file_path_;
  }

  auto proj = backend_.project_handler_.project();
  if (!proj) {
    return {};
  }

  try {
    return proj->GetImagePoolService()->Read<std::filesystem::path>(
        imageId, [](const std::shared_ptr<Image>& image) -> std::filesystem::path {
          if (!image) {
            return {};
          }
          return image->image_path_;
        });
  } catch (...) {
    return {};
  }
}

ThumbnailManager::ThumbnailManager(AlbumBackend& backend) : backend_(backend) {}

void ThumbnailManager::SetThumbnailVisible(sl_element_id_t elementId, image_id_t imageId,
                                           bool visible, uint32_t maxEdge) {
  if (elementId == 0 || imageId == 0) {
    return;
  }

  auto thumb_svc = backend_.project_handler_.thumbnail_service();
  const auto resolution = NearestResolutionTier(maxEdge);

  if (visible) {
    if (!thumb_svc) {
      return;
    }

    auto pin_it = thumbnail_pins_.find(elementId);
    if (pin_it != thumbnail_pins_.end()) {
      auto& pin = pin_it->second;
      if (pin.image_id_ == imageId && pin.resolution_ == resolution) {
        pin.ref_count_++;
        return;
      }

      if (auto flag_it = thumbnail_active_flags_.find(elementId);
          flag_it != thumbnail_active_flags_.end() && flag_it->second) {
        flag_it->second->store(false);
      }
      thumbnail_active_flags_.erase(elementId);
      try {
        thumb_svc->CancelPending(elementId);
        thumb_svc->InvalidateThumbnail(elementId);
      } catch (...) {
      }
      pin.image_id_ = imageId;
      pin.resolution_ = resolution;
    } else {
      thumbnail_pins_[elementId] = {.ref_count_ = 1,
                                    .image_id_ = imageId,
                                    .resolution_ = resolution};
    }

    const auto* item = backend_.FindAlbumItem(elementId);
    const bool known_missing = item != nullptr && item->thumb_missing_source;
    const auto source_path = ResolveThumbnailSourcePath(elementId, imageId);
    if (known_missing && !source_path.empty() && !PathExists(source_path)) {
      UpdateThumbnailState(elementId, QString(), false, true);
      return;
    }
    RequestThumbnail(elementId, imageId, maxEdge);
    return;
  }

  const auto it = thumbnail_pins_.find(elementId);
  if (it == thumbnail_pins_.end()) {
    return;
  }

  if (it->second.ref_count_ > 1) {
    it->second.ref_count_--;
    return;
  }

  thumbnail_pins_.erase(it);

  // Strategy B: mark any in-flight request for this element as inactive.
  {
    auto flag_it = thumbnail_active_flags_.find(elementId);
    if (flag_it != thumbnail_active_flags_.end() && flag_it->second) {
      flag_it->second->store(false);
    }
    thumbnail_active_flags_.erase(elementId);
  }

  const auto* item = backend_.FindAlbumItem(elementId);
  const bool  missing_source = item != nullptr && item->thumb_missing_source;
  UpdateThumbnailState(elementId, QString(), false, missing_source);
  if (thumb_svc) {
    try {
      thumb_svc->CancelPending(elementId);
      thumb_svc->InvalidateThumbnail(elementId);
    } catch (...) {
    }
  }
}

void ThumbnailManager::RequestThumbnail(sl_element_id_t elementId, image_id_t imageId,
                                        uint32_t maxEdge) {
  auto thumb_svc = backend_.project_handler_.thumbnail_service();
  if (!thumb_svc) {
    return;
  }

  const auto resolution = NearestResolutionTier(maxEdge);

  UpdateThumbnailState(elementId, QString(), true, false);

  // Strategy B: create active flag for this request.
  // When the element is unpinned, the flag is set to false,
  // and the callback skips QImage conversion.
  auto is_active = std::make_shared<std::atomic<bool>>(true);
  {
    // Invalidate any previous flag for this element.
    auto old_it = thumbnail_active_flags_.find(elementId);
    if (old_it != thumbnail_active_flags_.end() && old_it->second) {
      old_it->second->store(false);
    }
    thumbnail_active_flags_[elementId] = is_active;
  }

  auto                   service = thumb_svc;
  QPointer<AlbumBackend> self(&backend_);

  CallbackDispatcher dispatcher = [](std::function<void()> fn) {
    auto* app = QCoreApplication::instance();
    if (!app) {
      fn();
      return;
    }
    QMetaObject::invokeMethod(app, std::move(fn), Qt::QueuedConnection);
  };

  service->GetThumbnail(
      elementId, imageId,
      [self, service, elementId, imageId, maxEdge,
       is_active](std::shared_ptr<ThumbnailGuard> guard) {
        // Strategy B: skip QImage conversion if this request was cancelled.
        if (!is_active || !is_active->load()) {
          return;
        }

        if (!guard || !guard->thumbnail_buffer_) {
          if (self) {
            const auto source_path = self->thumb_.ResolveThumbnailSourcePath(elementId, imageId);
            const bool missing_source = !source_path.empty() && !PathExists(source_path);
            self->thumb_.UpdateThumbnailState(elementId, QString(), false, missing_source);
          }
          if (self && !self->thumb_.IsThumbnailPinned(elementId) && service) {
            try {
              service->ReleaseThumbnail(elementId);
            } catch (...) {
            }
          }
          return;
        }
        if (!self) {
          try {
            if (service) {
              service->ReleaseThumbnail(elementId);
            }
          } catch (...) {
          }
          return;
        }

        std::thread([self, service, elementId, maxEdge, is_active,
                     guard = std::move(guard)]() mutable {
          // Strategy B: re-check before expensive conversion.
          if (!is_active || !is_active->load()) {
            return;
          }

          QString dataUrl;
          try {
            auto* buffer = guard->thumbnail_buffer_.get();
            if (buffer) {
              if (!buffer->cpu_data_valid_ && buffer->gpu_data_valid_) {
                buffer->SyncToCPU();
              }
              if (buffer->cpu_data_valid_) {
                QImage image = album_util::MatRgba32fToQImageCopy(buffer->GetCPUData());
                if (!image.isNull()) {
                  const int raw_max_edge = static_cast<int>(std::max<uint32_t>(1, maxEdge));
                  const int scaled_max_edge = std::min(raw_max_edge, 1024);
                  QImage scaled = image.scaled(scaled_max_edge, scaled_max_edge, Qt::KeepAspectRatio,
                                               Qt::SmoothTransformation);
                  dataUrl = album_util::DataUrlFromImage(scaled);
                }
              }
            }
          } catch (...) {
          }

          if (self) {
            QMetaObject::invokeMethod(
                self,
                [self, service, elementId, dataUrl, is_active]() {
                  if (!self) {
                    return;
                  }

                  // Strategy B: final check before emitting signal.
                  if (!is_active || !is_active->load()) {
                    return;
                  }

                  const bool pinned = self->thumb_.IsThumbnailPinned(elementId);
                  if (pinned) {
                    self->thumb_.UpdateThumbnailState(elementId, dataUrl, false, false);
                  } else {
                    self->thumb_.UpdateThumbnailState(elementId, QString(), false, false);
                  }
                  if (!pinned && service) {
                    try {
                      service->ReleaseThumbnail(elementId);
                    } catch (...) {
                    }
                  }
                },
                Qt::QueuedConnection);
          }
        }).detach();
      },
      true, dispatcher, resolution);
}

void ThumbnailManager::UpdateThumbnailState(sl_element_id_t elementId, const QString& dataUrl,
                                            bool loading, bool missingSource) {
  auto* item = backend_.FindAlbumItem(elementId);
  if (!item) {
    return;
  }

  if (item->thumb_data_url == dataUrl && item->thumb_loading == loading &&
      item->thumb_missing_source == missingSource) {
    return;
  }

  item->thumb_data_url        = dataUrl;
  item->thumb_loading         = loading;
  item->thumb_missing_source  = missingSource;

  for (qsizetype i = 0; i < backend_.view_state_.visible_thumbnails_.size(); ++i) {
    QVariantMap row = backend_.view_state_.visible_thumbnails_.at(i).toMap();
    if (static_cast<sl_element_id_t>(row.value("elementId").toUInt()) != elementId) {
      continue;
    }
    row.insert("thumbUrl", dataUrl);
    row.insert("thumbLoading", loading);
    row.insert("thumbMissingSource", missingSource);
    backend_.view_state_.visible_thumbnails_[i] = row;
    break;
  }

  emit backend_.ThumbnailUpdated(static_cast<uint>(elementId), dataUrl, loading, missingSource);
  emit backend_.thumbnailUpdated(static_cast<uint>(elementId), dataUrl, loading, missingSource);
}

bool ThumbnailManager::IsThumbnailPinned(sl_element_id_t elementId) const {
  const auto it = thumbnail_pins_.find(elementId);
  return it != thumbnail_pins_.end() && it->second.ref_count_ > 0;
}

uint32_t ThumbnailManager::GetPinnedMaxEdge(sl_element_id_t elementId) const {
  const auto it = thumbnail_pins_.find(elementId);
  if (it == thumbnail_pins_.end()) {
    return 1024;
  }
  return static_cast<uint32_t>(it->second.resolution_);
}

void ThumbnailManager::RemoveThumbnailState(sl_element_id_t elementId, image_id_t imageId) {
  (void)imageId;
  if (elementId == 0) {
    return;
  }

  thumbnail_pins_.erase(elementId);

  // Strategy B: invalidate active flag.
  {
    auto flag_it = thumbnail_active_flags_.find(elementId);
    if (flag_it != thumbnail_active_flags_.end() && flag_it->second) {
      flag_it->second->store(false);
    }
    thumbnail_active_flags_.erase(elementId);
  }

  UpdateThumbnailState(elementId, QString(), false, false);

  auto thumb_svc = backend_.project_handler_.thumbnail_service();
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
  if (thumbnail_pins_.empty()) {
    return;
  }

  auto thumb_svc = backend_.project_handler_.thumbnail_service();

  for (const auto& [id, _] : thumbnail_pins_) {
    auto* item = backend_.FindAlbumItem(id);
    if (item) {
      item->thumb_data_url.clear();
      item->thumb_loading = false;
    }

    // Strategy B: invalidate active flag.
    {
      auto flag_it = thumbnail_active_flags_.find(id);
      if (flag_it != thumbnail_active_flags_.end() && flag_it->second) {
        flag_it->second->store(false);
      }
    }

    if (thumb_svc) {
      try {
        thumb_svc->CancelPending(id);
        thumb_svc->InvalidateThumbnail(id);
      } catch (...) {
      }
    }
  }
  thumbnail_pins_.clear();
  thumbnail_active_flags_.clear();
}

}  // namespace alcedo::ui
