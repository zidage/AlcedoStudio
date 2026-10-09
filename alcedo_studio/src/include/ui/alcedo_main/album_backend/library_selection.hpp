//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QObject>
#include <QString>
#include <QVariantList>
#include <QVariantMap>
#include <map>
#include <vector>

#include "type/type.hpp"
#include "ui/alcedo_main/album_backend/album_catalog.hpp"

namespace alcedo::ui {

/// The library image selection, keyed by element id. Owned by LibraryModule. GUI thread only.
///
/// QML reads `selectedImagesById` (element id text to `{elementId, fileId, imageId, fileName,
/// isHdr}`) and `selectedCount`, and changes the selection through the invokables. Every change
/// that leaves the selection as it was emits no SelectionChanged.
class LibrarySelection final : public QObject {
  Q_OBJECT
  Q_PROPERTY(QVariantMap selectedImagesById READ SelectedImagesById NOTIFY SelectionChanged)
  Q_PROPERTY(int selectedCount READ SelectedCount NOTIFY SelectionChanged)

 public:
  /// One selected image.
  struct SelectedImage {
    sl_element_id_t element_id = 0;
    image_id_t      image_id   = 0;
    QString         file_name;
    bool            is_hdr = false;
  };

  /// @p catalog supplies the current library row of a selected element; it must outlive this
  /// object.
  explicit LibrarySelection(const IAlbumCatalog* catalog, QObject* parent = nullptr);

  [[nodiscard]] auto SelectedImagesById() const -> QVariantMap;
  [[nodiscard]] auto SelectedCount() const -> int { return static_cast<int>(selected_.size()); }
  [[nodiscard]] auto SelectedElementIds() const -> std::vector<sl_element_id_t>;

  /// Adds or removes one image.
  Q_INVOKABLE void   SetImageSelected(uint elementId, uint imageId, const QString& fileName,
                                      bool isHdr, bool selected);
  Q_INVOKABLE void   Clear();
  /// Replaces the selection with @p items (maps with elementId, imageId, fileName, isHdr).
  Q_INVOKABLE void   Replace(const QVariantList& items);
  /// The selected images as target maps (elementId, fileId, imageId, fileName, isHdr). The
  /// image id, file name, and HDR flag come from the current library row when it exists.
  Q_INVOKABLE QVariantList SelectedItems() const;
  /// Removes the deleted elements @p elementIds from the selection.
  Q_INVOKABLE void         PruneElements(const QVariantList& elementIds);

  void                     Replace(std::vector<SelectedImage> images);
  void                     Prune(const std::vector<sl_element_id_t>& element_ids);

 signals:
  void SelectionChanged();

 private:
  [[nodiscard]] auto                       ItemMap(const SelectedImage& image) const -> QVariantMap;
  [[nodiscard]] auto                       Normalized(SelectedImage image) const -> SelectedImage;
  void                                     Assign(std::map<sl_element_id_t, SelectedImage> next);

  const IAlbumCatalog*                     catalog_ = nullptr;
  std::map<sl_element_id_t, SelectedImage> selected_;
};

}  // namespace alcedo::ui
