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

/// Provider of the LUT browser previews (issue #598).
inline constexpr const char* kLutPreviewImageProviderId    = "alcedo-lut-preview";
/// Provider of the preset browser previews (edit presets plan, PS3).
inline constexpr const char* kPresetPreviewImageProviderId = "alcedo-preset-preview";
/// Provider of the preset previews of the Apply Preset dialog (edit presets plan, PS4).
inline constexpr const char* kPresetDialogPreviewImageProviderId = "alcedo-preset-dialog-preview";

/**
 * @brief Display table of the browser tile previews that tiles show now: one table per image
 *        provider (the LUT previews and the preset previews).
 *
 * Keys are display ids that the BrowserPreviewController of the provider assigns when a tile
 * starts to show a preview; they are not content keys. The LRU of rendered previews is in the
 * controller. QImage implicit sharing keeps the pixels alive after LRU eviction.
 *
 * An entry holds the source pixels and their encoding. GetForDisplay converts them to the display
 * destination of the main window (Windows display ICC plan, W5); the owner of the display ids
 * gives the shown previews new ids after SetDestination.
 *
 * Thread: any thread. The image provider reads on the QML image thread.
 */
class BrowserPreviewImageStore {
 public:
  /// @p provider_id is the host part of the URLs that @ref Put returns, such as
  /// kLutPreviewImageProviderId.
  explicit BrowserPreviewImageStore(QString provider_id);

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
  [[nodiscard]] auto        provider_id() const -> const QString& { return provider_id_; }
  /// URL of @p display_id: `image://<provider id>/<display id>`.
  [[nodiscard]] auto        MakeUrl(std::uint64_t display_id) const -> QString;
  /// Parse the provider id of a URL (the part after `image://<provider id>/`).
  [[nodiscard]] static auto ParseProviderId(const QString& id, std::uint64_t* display_id) -> bool;

 private:
  struct Entry {
    QImage              image;
    ViewerDisplayConfig encoding{};
  };

  const QString                            provider_id_;
  mutable std::mutex                       mutex_;
  std::unordered_map<std::uint64_t, Entry> images_;
  DisplayImageConverter                    converter_;
};

/// `image://<provider id>/<display id>`: the browser preview that a tile shows.
class BrowserPreviewImageProvider final : public QQuickImageProvider {
 public:
  explicit BrowserPreviewImageProvider(std::shared_ptr<BrowserPreviewImageStore> store);

  QImage requestImage(const QString& id, QSize* size, const QSize& requestedSize) override;

 private:
  std::shared_ptr<BrowserPreviewImageStore> store_;
};

/// The store that the application LUT preview provider reads.
[[nodiscard]] auto SharedLutPreviewImageStore() -> std::shared_ptr<BrowserPreviewImageStore>;
/// The store that the application preset preview provider reads.
[[nodiscard]] auto SharedPresetPreviewImageStore() -> std::shared_ptr<BrowserPreviewImageStore>;
/// The store that the application preset dialog preview provider reads.
[[nodiscard]] auto SharedPresetDialogPreviewImageStore()
    -> std::shared_ptr<BrowserPreviewImageStore>;

}  // namespace alcedo::ui
