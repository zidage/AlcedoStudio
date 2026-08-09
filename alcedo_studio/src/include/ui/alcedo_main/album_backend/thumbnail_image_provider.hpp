//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QImage>
#include <QQuickImageProvider>
#include <QString>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>

#include "type/type.hpp"

namespace alcedo::ui {

inline constexpr const char* kThumbnailImageProviderId = "alcedo-thumb";

/// Thread-safe in-memory QImage store for album / filmstrip thumbnails.
/// QML loads pixels via `image://alcedo-thumb/<elementId>/<maxEdge>/<revision>`.
class ThumbnailImageStore {
 public:
  struct Key {
    sl_element_id_t element_id = 0;
    uint32_t        max_edge   = 0;

    bool operator==(const Key& other) const = default;
  };

  struct KeyHash {
    size_t operator()(const Key& key) const noexcept {
      const auto h1 = std::hash<uint32_t>{}(key.element_id);
      const auto h2 = std::hash<uint32_t>{}(key.max_edge);
      return h1 ^ (h2 + 0x9e3779b9 + (h1 << 6) + (h1 >> 2));
    }
  };

  /// Insert or replace the image and return an `image://` URL with a fresh revision.
  [[nodiscard]] auto Put(sl_element_id_t element_id, uint32_t max_edge, QImage image) -> QString;
  void               Remove(sl_element_id_t element_id, uint32_t max_edge);
  void               RemoveElement(sl_element_id_t element_id);
  void               Clear();

  [[nodiscard]] auto Get(sl_element_id_t element_id, uint32_t max_edge) const -> QImage;
  [[nodiscard]] auto GetByProviderId(const QString& id) const -> QImage;

  [[nodiscard]] static auto MakeUrl(sl_element_id_t element_id, uint32_t max_edge,
                                    quint64 revision) -> QString;
  [[nodiscard]] static auto ParseProviderId(const QString& id, sl_element_id_t* element_id,
                                            uint32_t* max_edge, quint64* revision) -> bool;
  /// Resolve an `image://alcedo-thumb/...` URL against this store. Other schemes return null.
  [[nodiscard]] auto ResolveUrl(const QString& url) const -> QImage;

 private:
  struct Entry {
    QImage  image;
    quint64 revision = 0;
  };

  mutable std::mutex                 mutex_;
  std::unordered_map<Key, Entry, KeyHash> images_;
  std::atomic<quint64>               next_revision_{1};
};

class ThumbnailImageProvider final : public QQuickImageProvider {
 public:
  explicit ThumbnailImageProvider(std::shared_ptr<ThumbnailImageStore> store);

  QImage requestImage(const QString& id, QSize* size, const QSize& requestedSize) override;

 private:
  std::shared_ptr<ThumbnailImageStore> store_;
};

}  // namespace alcedo::ui
