//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QAbstractListModel>
#include <QHash>
#include <QPointer>
#include <QRectF>
#include <QVariant>
#include <QVariantList>
#include <vector>

#include "ui/alcedo_main/album_backend/album_thumbnail_model.hpp"
#include "ui/alcedo_main/album_backend/justified_row_layout.hpp"

namespace alcedo::ui {

/**
 * @brief The justified-rows Library layout of the loaded photos of an AlbumThumbnailModel, and
 *        the window of the photos whose tiles are near the viewport.
 *
 * @details The model lays out the photos of `source` with JustifiedRowLayout (row height
 * `rowHeight`, view width `viewWidth`). The window is the photos of the layout rows that
 * intersect `[viewportY - overscan, viewportY + viewportHeight + overscan]`: source photos
 * `[firstIndex, firstIndex + count)`.
 *
 * The rows of this model are tile slots, so that a view keeps its delegates while the window
 * moves (slotOfIndex). A photo keeps its slot while it stays in the window; a photo that enters
 * the window takes an empty slot, the lowest first, and the model appends slots only when no
 * slot is empty. A slot has every role of its source photo, plus `photoIndex` and the tile
 * rectangle (`tileX`, `tileY`, `tileWidth`, `tileHeight`) in layout coordinates. An empty slot
 * has `photoIndex` -1, a null tile rectangle, and the role values of a default AlbumItem.
 *
 * A slot whose photo changes gets every role (dataChanged); a photo that stays gets nothing on a
 * scroll and the tile roles on a layout change. A source insert or remove changes the photo of
 * every index after it, so every slot with a photo gets every role. After a layout or source
 * change that leaves fewer than half of the slots in use, the model removes the slots from
 * `count` on; a photo of a removed slot moves to an empty kept slot. A source reset resets this
 * model and moves the viewport to the top (viewportReset), so the new result shows from its
 * first photo, as a GridView shows a reset model.
 *
 * The layout reads the `aspectRatio` role of each source photo. A change of that role lays out
 * the rows from the row of the photo. Appended pages lay out the last row and the rows after it.
 *
 * Thread: GUI thread only.
 */
class JustifiedLayoutModel : public QAbstractListModel {
  Q_OBJECT
  Q_PROPERTY(QObject* source READ source WRITE setSource NOTIFY sourceChanged)
  Q_PROPERTY(qreal viewWidth READ viewWidth WRITE setViewWidth NOTIFY geometryChanged)
  Q_PROPERTY(qreal rowHeight READ rowHeight WRITE setRowHeight NOTIFY geometryChanged)
  Q_PROPERTY(qreal spacing READ spacing WRITE setSpacing NOTIFY geometryChanged)
  Q_PROPERTY(qreal captionHeight READ captionHeight WRITE setCaptionHeight NOTIFY geometryChanged)
  Q_PROPERTY(qreal viewportY READ viewportY WRITE setViewportY NOTIFY viewportChanged)
  Q_PROPERTY(
      qreal viewportHeight READ viewportHeight WRITE setViewportHeight NOTIFY viewportChanged)
  Q_PROPERTY(qreal overscan READ overscan WRITE setOverscan NOTIFY viewportChanged)
  /// Height of all photos of the result: the loaded rows and the estimated rows of the photos
  /// that are not loaded.
  Q_PROPERTY(qreal contentHeight READ contentHeight NOTIFY tilesChanged)
  /// Bottom of the last loaded tile.
  Q_PROPERTY(qreal loadedHeight READ loadedHeight NOTIFY tilesChanged)
  /// Source index of the first photo of the window; 0 when the window is empty.
  Q_PROPERTY(int firstIndex READ firstIndex NOTIFY windowChanged)
  /// Photos in the window.
  Q_PROPERTY(int count READ count NOTIFY windowChanged)
  /// Laid-out rows.
  Q_PROPERTY(int layoutRowCount READ layoutRowCount NOTIFY tilesChanged)
  /// Increments after each change of the tiles.
  Q_PROPERTY(int tilesRevision READ tilesRevision NOTIFY tilesChanged)

 public:
  enum Roles {
    PhotoIndex = Qt::UserRole + 1000,
    TileX,
    TileY,
    TileWidth,
    TileHeight,
  };

  explicit JustifiedLayoutModel(QObject* parent = nullptr);

  // QAbstractListModel
  int                    rowCount(const QModelIndex& parent = QModelIndex()) const override;
  QVariant               data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
  QHash<int, QByteArray> roleNames() const override;

  [[nodiscard]] auto     source() const -> QObject* { return source_.data(); }
  /// The AlbumThumbnailModel to lay out; any other object reads as no source.
  void                   setSource(QObject* source);
  [[nodiscard]] auto     viewWidth() const -> qreal { return geometry_.view_width; }
  void                   setViewWidth(qreal value);
  [[nodiscard]] auto     rowHeight() const -> qreal { return geometry_.row_height; }
  void                   setRowHeight(qreal value);
  [[nodiscard]] auto     spacing() const -> qreal { return geometry_.spacing; }
  void                   setSpacing(qreal value);
  [[nodiscard]] auto     captionHeight() const -> qreal { return geometry_.caption_height; }
  void                   setCaptionHeight(qreal value);
  [[nodiscard]] auto     viewportY() const -> qreal { return viewport_y_; }
  void                   setViewportY(qreal value);
  [[nodiscard]] auto     viewportHeight() const -> qreal { return viewport_height_; }
  void                   setViewportHeight(qreal value);
  [[nodiscard]] auto     overscan() const -> qreal { return overscan_; }
  void                   setOverscan(qreal value);

  [[nodiscard]] auto     contentHeight() const -> qreal;
  [[nodiscard]] auto     loadedHeight() const -> qreal { return layout_.LoadedHeight(); }
  [[nodiscard]] auto     firstIndex() const -> int { return window_count_ > 0 ? first_ : 0; }
  [[nodiscard]] auto     count() const -> int { return window_count_; }
  [[nodiscard]] auto     layoutRowCount() const -> int {
    return static_cast<int>(layout_.rows().size());
  }
  [[nodiscard]] auto tilesRevision() const -> int { return tiles_revision_; }
  [[nodiscard]] auto layout() const -> const JustifiedRowLayout& { return layout_; }

  /// Set the view width, row height, spacing, and caption height in one layout pass.
  Q_INVOKABLE void   setGeometry(qreal viewWidth, qreal rowHeight, qreal spacing,
                                 qreal captionHeight);
  /**
   * @brief Set the view values and move the viewport so that a point of one tile stays at the
   *        same viewport height, with one layout pass and one window update.
   *
   * @param anchorIndex Source photo that keeps its place; -1 keeps viewportY.
   * @param anchorFraction Point of the anchor tile as a fraction of its height, in [0, 1].
   * @param anchorViewportY Viewport height of that point.
   * @return The new viewportY, clamped to [0, contentHeight - viewportHeight].
   */
  Q_INVOKABLE qreal  setGeometryAnchored(qreal viewWidth, qreal rowHeight, qreal spacing,
                                         qreal captionHeight, int anchorIndex, qreal anchorFraction,
                                         qreal anchorViewportY);
  /// Tile of source photo @p index in layout coordinates; a null rectangle when the photo is not
  /// laid out.
  [[nodiscard]] Q_INVOKABLE QRectF       tileRect(int index) const;
  /// Source photo whose tile holds the layout point; -1 in a gap or outside the tiles.
  [[nodiscard]] Q_INVOKABLE int          indexAt(qreal x, qreal y) const;
  /// Source photo whose tile is nearest to the layout point; -1 without tiles.
  [[nodiscard]] Q_INVOKABLE int          nearestIndex(qreal x, qreal y) const;
  /// Slot (row of this model) of source photo @p index; -1 when the photo is not in the window.
  [[nodiscard]] Q_INVOKABLE int          slotOfIndex(int index) const;
  /// Source photos whose tiles intersect the layout rectangle, in order.
  [[nodiscard]] Q_INVOKABLE QVariantList indicesInRect(qreal x, qreal y, qreal width,
                                                       qreal height) const;
  /// Top of the row of source photo @p index; an estimated row for a photo not loaded yet.
  [[nodiscard]] Q_INVOKABLE qreal        rowTopOfIndex(int index) const;
  /// Photo at the start of the row at @p y; an estimated index below the loaded rows.
  [[nodiscard]] Q_INVOKABLE int          indexAtY(qreal y) const;
  /// `{first, last}` source photos of the rows that intersect [@p y0, @p y1]; last is -1 when
  /// no row does.
  [[nodiscard]] Q_INVOKABLE QVariantMap  indexRangeForY(qreal y0, qreal y1) const;

 signals:
  void sourceChanged();
  void geometryChanged();
  void viewportChanged();
  void tilesChanged();
  void windowChanged();
  /// A source reset moved viewportY to 0; the view moves its content position there.
  void viewportReset();

 private:
  /// What changed before a window update.
  enum class WindowChange {
    kViewport,  ///< The viewport moved; the tiles and the photo of each index are the same.
    kTiles,   ///< The tiles moved (geometry or aspect ratio); the photo of each index is the same.
    kPhotos,  ///< Source rows were inserted or removed: an index can name another photo.
  };

  [[nodiscard]] auto thumbnailModel() const -> AlbumThumbnailModel*;
  [[nodiscard]] auto AspectRatioAt(int source_row) const -> double;
  [[nodiscard]] auto AspectRatios(int first, int last) const -> std::vector<double>;
  void               ApplyGeometry(const JustifiedLayoutGeometry& geometry);
  /// Rows of the window for the current layout and viewport.
  [[nodiscard]] auto WantedWindow() const -> JustifiedRowLayout::IndexRange;
  /// Move the window to WantedWindow, give the photos that enter it a slot, and notify the slots
  /// whose photo or tile changed.
  void               UpdateWindow(WindowChange change);
  /// Put the photos of WantedWindow in slots 0 to window_count_ - 1, in order, with no other
  /// slot (a new source or a source reset).
  void               PlaceWindowInOrder();
  void               OnSourceReset();
  void               OnSourceRowsInserted(const QModelIndex& parent, int first, int last);
  void               OnSourceRowsRemoved(const QModelIndex& parent, int first, int last);
  void OnSourceDataChanged(const QModelIndex& top_left, const QModelIndex& bottom_right,
                           const QList<int>& roles);

  QPointer<AlbumThumbnailModel>        source_;
  std::vector<QMetaObject::Connection> source_connections_;
  JustifiedLayoutGeometry              geometry_{};
  JustifiedRowLayout                   layout_{};
  qreal                                viewport_y_      = 0.0;
  qreal                                viewport_height_ = 0.0;
  qreal                                overscan_        = 0.0;
  /// Window: source rows [first_, first_ + window_count_).
  int                                  first_           = 0;
  int                                  window_count_    = 0;
  /// Photo of each slot (row of this model); -1 for an empty slot. At least window_count_ slots.
  std::vector<int>                     slot_photo_;
  /// Slot of each photo of the window: window_slot_[p - first_] for photo p.
  std::vector<int>                     window_slot_;
  int                                  tiles_revision_  = 0;
};

/// Register JustifiedLayoutModel as `Alcedo.Main 1.0 JustifiedLayoutModel`.
void RegisterJustifiedLayoutQmlType();

}  // namespace alcedo::ui
