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

#include "ui/edit_viewer/window_display_destination.hpp"

namespace alcedo::ui {

inline constexpr const char* kLutPreviewImageProviderId = "alcedo-lut-preview";

/**
 * @brief Display table of the LUT previews that tiles show now (issue #598).
 *
 * Keys are display ids that LutPreviewController assigns when a tile starts to show a preview;
 * they are not content keys. The LRU of rendered previews is in LutPreviewController. QImage
 * implicit sharing keeps the pixels alive after LRU eviction.
 *
 * An entry holds the source pixels and their encoding. GetForDisplay converts them to the display
 * destination of the main window (Windows display ICC plan, W5); the owner of the display ids
 * gives the shown previews new ids after SetDestination.
 *
 * Thread: any thread. The image provider reads on the QML image thread.
 */
class LutPreviewImageStore {
 public:
  /// Store @p image with the pixels of @p encoding under @p display_id and return its URL; empty
  /// for id 0 or a null image.
  [[nodiscard]] auto        Put(std::uint64_t display_id, QImage image,
                                const ViewerDisplayConfig& encoding) -> QString;
  void                      Remove(std::uint64_t display_id);
  /// The source image of @p display_id, or null.
  [[nodiscard]] auto        Get(std::uint64_t display_id) const -> QImage;
  /// Get, converted to the destination (ConvertImageForDestination); the source pixels when the
  /// conversion fails.
  [[nodiscard]] auto        GetForDisplay(std::uint64_t display_id) const -> QImage;
  /// Convert the images to @p destination from now on. Null converts nothing.
  void                      SetDestination(std::shared_ptr<const DisplayDestination> destination);
  [[nodiscard]] auto        Destination() const -> std::shared_ptr<const DisplayDestination>;
  [[nodiscard]] auto        size() const -> std::size_t;
  [[nodiscard]] static auto MakeUrl(std::uint64_t display_id) -> QString;
  /// Parse the provider id of a URL (the part after `image://alcedo-lut-preview/`).
  [[nodiscard]] static auto ParseProviderId(const QString& id, std::uint64_t* display_id) -> bool;

 private:
  struct Entry {
    QImage              image;
    ViewerDisplayConfig encoding{};
  };

  mutable std::mutex                       mutex_;
  std::unordered_map<std::uint64_t, Entry> images_;
  DisplayImageConverter                    converter_;
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
