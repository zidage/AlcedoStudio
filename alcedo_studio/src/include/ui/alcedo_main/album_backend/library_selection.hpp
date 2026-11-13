//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QObject>
#include <QString>
#include <QVariantList>
#include <QVariantMap>
#include <map>
#include <set>
#include <vector>

#include "type/type.hpp"
#include "ui/alcedo_main/album_backend/album_catalog.hpp"

namespace alcedo::ui {

/// The library image selection, keyed by element id. Owned by LibraryModule. GUI thread only.
///
/// The selection is backend state: a delegate asks IsSelected for its own photo and binds to
/// `revision` so that it reads again after a change. QML never receives the whole selection as
/// one value, so a selection of tens of thousands of photos costs one counter update per change.
/// Actions read the complete set through SelectedItems. Every change that leaves the selection
/// as it was emits no SelectionChanged.
class LibrarySelection final : public QObject {
  Q_OBJECT
  Q_PROPERTY(int selectedCount READ SelectedCount NOTIFY SelectionChanged)
  /// Increments on each change of the selection. Bindings that call IsSelected read it.
  Q_PROPERTY(int revision READ Revision NOTIFY SelectionChanged)

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

  [[nodiscard]] auto SelectedCount() const -> int { return static_cast<int>(selected_.size()); }
  [[nodiscard]] auto Revision() const -> int { return revision_; }
  [[nodiscard]] auto SelectedElementIds() const -> std::vector<sl_element_id_t>;

  /// True when @p elementId is selected.
  Q_INVOKABLE bool   IsSelected(uint elementId) const;
  /// Adds or removes one image.
  Q_INVOKABLE void   SetImageSelected(uint elementId, uint imageId, const QString& fileName,
                                      bool isHdr, bool selected);
  Q_INVOKABLE void   Clear();
  /// Replaces the selection with @p items (maps with elementId, imageId, fileName, isHdr).
  Q_INVOKABLE void   Replace(const QVariantList& items);
  /// Adds @p items (same maps as Replace) to the selection (Ctrl+Shift range).
  Q_INVOKABLE void   AddItems(const QVariantList& items);
  /**
   * @brief Additive range that the user still changes (Ctrl + rubber band drag). The selection
   *        becomes the selection from before the range plus @p items.
   *
   * @details Each call removes the photos that the previous call of the same range added and
   *        that @p items no longer holds. A photo that was selected before the range started
   *        stays selected. EndRange, and every other change, ends the range.
   */
  Q_INVOKABLE void   SetRangeItems(const QVariantList& items);
  Q_INVOKABLE void   EndRange();
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
  [[nodiscard]] auto ItemMap(const SelectedImage& image) const -> QVariantMap;
  [[nodiscard]] auto Normalized(SelectedImage image) const -> SelectedImage;
  [[nodiscard]] auto ImageFromItem(const QVariantMap& item) const -> SelectedImage;
  void               Assign(std::map<sl_element_id_t, SelectedImage> next);
  void               NotifyChanged();

  const IAlbumCatalog*                     catalog_ = nullptr;
  std::map<sl_element_id_t, SelectedImage> selected_;
  /// Photos that the open additive range added (see SetRangeItems).
  std::set<sl_element_id_t>                range_added_;
  int                                      revision_ = 0;
};

}  // namespace alcedo::ui
