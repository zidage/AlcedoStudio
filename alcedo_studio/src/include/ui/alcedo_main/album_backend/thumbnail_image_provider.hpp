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
#include <optional>
#include <unordered_map>

#include "type/type.hpp"
#include "ui/edit_viewer/frame_sink.hpp"
#include "ui/edit_viewer/window_display_destination.hpp"

namespace alcedo::ui {

inline constexpr const char* kThumbnailImageProviderId = "alcedo-thumb";

/**
 * @brief Thread-safe in-memory store of the album, filmstrip, and search thumbnails.
 *
 * QML loads pixels via `image://alcedo-thumb/<elementId>/<maxEdge>/<revision>`. The revision is
 * for the QML cache only: a request with an older revision of a key gets the current pixels.
 *
 * An entry holds the source pixels (`QImage::Format_RGBA8888`) and their output encoding.
 * @ref GetForDisplay converts them to the display destination of the main window (Windows display
 * ICC plan, W6), so a new destination needs no second render. @ref SetDestination gives each
 * entry a new revision; the owner of the rows publishes the new URLs.
 *
 * Thread: any thread. The image provider reads on the QML image threads; the producers put on
 * their conversion threads.
 */
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

  /**
   * @brief Insert or replace the image and return an `image://` URL with a fresh revision.
   * @param image The pixels of @p encoding in any 8-bit RGB format; the store keeps them as
   *        `QImage::Format_RGBA8888`.
   * @param encoding The output encoding of the pixels (ThumbnailGuard::encoding).
   * @return Empty for element id 0, edge 0, or a null image.
   */
  [[nodiscard]] auto Put(sl_element_id_t element_id, uint32_t max_edge, QImage image,
                         const ViewerDisplayConfig& encoding) -> QString;
  void               Remove(sl_element_id_t element_id, uint32_t max_edge);
  void               RemoveElement(sl_element_id_t element_id);
  void               Clear();

  /// The source pixels of the key, or null.
  [[nodiscard]] auto Get(sl_element_id_t element_id, uint32_t max_edge) const -> QImage;
  /// The output encoding of the key; std::nullopt when the key has no image.
  [[nodiscard]] auto Encoding(sl_element_id_t element_id, uint32_t max_edge) const
      -> std::optional<ViewerDisplayConfig>;
  /// Get, converted to the destination (ConvertImageForDestination); the source pixels for an
  /// HDR encoding, without a destination, and when the conversion fails.
  [[nodiscard]] auto GetForDisplay(sl_element_id_t element_id, uint32_t max_edge) const -> QImage;
  /// URL of the stored image at its current revision, or empty when the key has no image.
  [[nodiscard]] auto CurrentUrl(sl_element_id_t element_id, uint32_t max_edge) const -> QString;
  /// The source pixels of a provider id.
  [[nodiscard]] auto GetByProviderId(const QString& id) const -> QImage;
  /// The display pixels (@ref GetForDisplay) of a provider id.
  [[nodiscard]] auto GetForDisplayByProviderId(const QString& id) const -> QImage;

  /**
   * @brief Convert the images to @p destination from now on, and give each entry a new revision.
   *
   * Renders nothing: the entries keep their source pixels. Null converts nothing.
   * @thread The GUI thread of the owner of the rows.
   */
  void               SetDestination(std::shared_ptr<const DisplayDestination> destination);
  [[nodiscard]] auto Destination() const -> std::shared_ptr<const DisplayDestination>;

  [[nodiscard]] static auto MakeUrl(sl_element_id_t element_id, uint32_t max_edge,
                                    quint64 revision) -> QString;
  [[nodiscard]] static auto ParseProviderId(const QString& id, sl_element_id_t* element_id,
                                            uint32_t* max_edge, quint64* revision) -> bool;
  /// ParseProviderId of an `image://alcedo-thumb/...` URL; false for other schemes.
  [[nodiscard]] static auto ParseUrl(const QString& url, sl_element_id_t* element_id,
                                     uint32_t* max_edge, quint64* revision) -> bool;
  /// Resolve an `image://alcedo-thumb/...` URL to its source pixels. Other schemes return null.
  [[nodiscard]] auto ResolveUrl(const QString& url) const -> QImage;

 private:
  struct Entry {
    QImage              image;
    ViewerDisplayConfig encoding{};
    quint64             revision = 0;
  };

  [[nodiscard]] auto FindEntry(sl_element_id_t element_id, uint32_t max_edge) const
      -> std::optional<Entry>;

  mutable std::mutex                      mutex_;
  std::unordered_map<Key, Entry, KeyHash> images_;
  std::atomic<quint64>                    next_revision_{1};
  DisplayImageConverter                   converter_;
};

class ThumbnailImageProvider final : public QQuickImageProvider {
 public:
  explicit ThumbnailImageProvider(std::shared_ptr<ThumbnailImageStore> store);

  /// The display pixels of the thumbnail (ThumbnailImageStore::GetForDisplay).
  QImage requestImage(const QString& id, QSize* size, const QSize& requestedSize) override;

 private:
  std::shared_ptr<ThumbnailImageStore> store_;
};

}  // namespace alcedo::ui
