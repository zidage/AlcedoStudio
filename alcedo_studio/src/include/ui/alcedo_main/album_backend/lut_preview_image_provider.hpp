//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QImage>
#include <QQuickImageProvider>
#include <QString>
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace alcedo::ui {

inline constexpr const char* kLutPreviewImageProviderId = "alcedo-lut-preview";

/**
 * @brief Display table of the LUT previews that tiles show now (issue #598).
 *
 * Keys are display ids that LutPreviewController assigns when a tile starts to show a preview;
 * they are not content keys. The LRU of rendered previews is in LutPreviewController. QImage
 * implicit sharing keeps the pixels alive after LRU eviction.
 *
 * Thread: any thread. The image provider reads on the QML image thread.
 */
class LutPreviewImageStore {
 public:
  /// Store @p image under @p display_id and return its URL; empty for id 0 or a null image.
  [[nodiscard]] auto        Put(std::uint64_t display_id, QImage image) -> QString;
  void                      Remove(std::uint64_t display_id);
  [[nodiscard]] auto        Get(std::uint64_t display_id) const -> QImage;
  [[nodiscard]] auto        size() const -> std::size_t;
  [[nodiscard]] static auto MakeUrl(std::uint64_t display_id) -> QString;
  /// Parse the provider id of a URL (the part after `image://alcedo-lut-preview/`).
  [[nodiscard]] static auto ParseProviderId(const QString& id, std::uint64_t* display_id) -> bool;

 private:
  mutable std::mutex                        mutex_;
  std::unordered_map<std::uint64_t, QImage> images_;
};

/// `image://alcedo-lut-preview/<display id>`: the LUT preview that a tile shows.
class LutPreviewImageProvider final : public QQuickImageProvider {
 public:
  explicit LutPreviewImageProvider(std::shared_ptr<LutPreviewImageStore> store);

  QImage requestImage(const QString& id, QSize* size, const QSize& requestedSize) override;

 private:
  std::shared_ptr<LutPreviewImageStore> store_;
};

/// The store that the application image provider reads.
[[nodiscard]] auto SharedLutPreviewImageStore() -> std::shared_ptr<LutPreviewImageStore>;

}  // namespace alcedo::ui
