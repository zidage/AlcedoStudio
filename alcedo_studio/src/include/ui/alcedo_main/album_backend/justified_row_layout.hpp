//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstddef>
#include <optional>
#include <vector>

namespace alcedo::ui {

/// View values of a justified-rows layout. All values are in view pixels.
struct JustifiedLayoutGeometry {
  double view_width     = 0.0;  ///< Width that each complete row fills.
  double row_height     = 0.0;  ///< Target photo height of a row (the zoom level).
  double spacing        = 0.0;  ///< Gap between two tiles of a row and between two rows.
  double caption_height = 0.0;  ///< Height below the photo of each tile (the file name).

  auto   operator==(const JustifiedLayoutGeometry&) const -> bool = default;
};

/// Tile rectangle in layout coordinates: 0 is the top of the first row.
struct JustifiedTileRect {
  double x      = 0.0;
  double y      = 0.0;
  double width  = 0.0;
  double height = 0.0;  ///< Photo height plus the caption height.
};

/**
 * @brief Justified-rows geometry of the loaded photos of a Library result.
 *
 * @details All tiles of one row have the same photo height. The width of a tile is the photo
 * height times the aspect ratio of its photo. A complete row fills the view width: the sum of
 * its tile widths and gaps is `view_width`. The photos are put into rows in order. A row ends
 * at the photo count whose row height is nearest to `row_height`; a row of one photo that is
 * wider than the view gets the view width.
 *
 * The last row is incomplete: it keeps the target height and starts at the left edge, because
 * the next page of photos continues it. Its photos are laid out again when photos are appended.
 *
 * The photo height of a row stays in [kMinRowHeightRatio, kMaxRowHeightRatio] times the target
 * height. Only a row of one photo that is much wider than the view reaches the lower limit; that
 * tile then shows the middle of its photo. Aspect ratios are clamped to
 * [kMinAspectRatio, kMaxAspectRatio]; an unknown aspect ratio (0 or less) is kUnknownAspectRatio,
 * the 3:2 box of the classic grid.
 *
 * A change of the geometry lays out every row again. Appending photos lays out the last row and
 * the rows after it. An insert, a remove, or an aspect ratio change lays out the rows from the
 * row of the first changed photo; the rows above it stay.
 *
 * The layout also estimates the height of the photos that are not loaded yet, from the mean
 * aspect ratio of the loaded photos, so that a scroll bar can cover the complete result.
 *
 * Thread: not synchronized; the owner uses it from one thread.
 */
class JustifiedRowLayout {
 public:
  static constexpr double kUnknownAspectRatio = 1.5;
  static constexpr double kMinAspectRatio     = 0.2;
  static constexpr double kMaxAspectRatio     = 5.0;
  static constexpr double kMinRowHeightRatio  = 0.5;
  static constexpr double kMaxRowHeightRatio  = 1.5;

  /// One row: the photos [first, first + count) at photo height photo_height.
  struct Row {
    int    first        = 0;
    int    count        = 0;
    double top          = 0.0;
    double photo_height = 0.0;
    bool   complete     = false;  ///< True when the row fills the view width.
  };

  /// Set the view values. Returns true when they changed; then every row is laid out again.
  auto               SetGeometry(const JustifiedLayoutGeometry& geometry) -> bool;
  [[nodiscard]] auto geometry() const -> const JustifiedLayoutGeometry& { return geometry_; }

  /// Replace all photos with @p aspect_ratios (width / height of each photo, in order).
  void               Reset(std::vector<double> aspect_ratios);
  /// Append photos after the last photo.
  void               Append(const std::vector<double>& aspect_ratios);
  /// Insert photos before photo @p index.
  void               Insert(int index, const std::vector<double>& aspect_ratios);
  /// Remove @p count photos from photo @p first.
  void               Remove(int first, int count);
  /// Set the aspect ratio of photo @p index. Returns true when the tiles changed.
  auto               SetAspectRatio(int index, double aspect_ratio) -> bool;

  /// First row whose layout changed in the last operation; -1 when no row changed.
  [[nodiscard]] auto first_changed_row() const -> int { return first_changed_row_; }

  [[nodiscard]] auto ItemCount() const -> int { return static_cast<int>(aspects_.size()); }
  [[nodiscard]] auto rows() const -> const std::vector<Row>& { return rows_; }
  /// Tile height of a row of photo height @p photo_height.
  [[nodiscard]] auto TileHeight(double photo_height) const -> double {
    return photo_height + geometry_.caption_height;
  }

  /// Tile of photo @p index; nullopt when the photo is not laid out.
  [[nodiscard]] auto TileRect(int index) const -> std::optional<JustifiedTileRect>;
  /// Row that holds photo @p index; -1 when the photo is not laid out.
  [[nodiscard]] auto RowOfIndex(int index) const -> int;
  /// Row whose span (tile height plus the gap below it) holds @p y; the first row for y above
  /// it, the last row for y below it; -1 without rows.
  [[nodiscard]] auto RowAtY(double y) const -> int;
  /// Photo whose tile holds the point; -1 in a gap or outside the tiles.
  [[nodiscard]] auto IndexAt(double x, double y) const -> int;
  /// Photo whose tile is nearest to the point: the row at @p y, then the tile nearest to @p x.
  [[nodiscard]] auto NearestIndex(double x, double y) const -> int;
  /// Photos whose tiles intersect the rectangle [x0, x1] x [y0, y1], in order.
  [[nodiscard]] auto IndicesInRect(double x0, double y0, double x1, double y1) const
      -> std::vector<int>;
  /// Photos of the rows that intersect [y0, y1]; empty when no row does.
  struct IndexRange {
    int first = 0;
    int last  = -1;
  };
  [[nodiscard]] auto IndexRangeForY(double y0, double y1) const -> IndexRange;

  /// Bottom of the last tile; 0 without rows.
  [[nodiscard]] auto LoadedHeight() const -> double;
  /// LoadedHeight plus the estimated rows of the photos after the loaded photos, for a result
  /// of @p total_count photos.
  [[nodiscard]] auto EstimatedHeight(int total_count) const -> double;
  /// Top of the row of photo @p index: the laid-out row, or the estimated row of a photo that is
  /// not loaded yet.
  [[nodiscard]] auto EstimatedRowTop(int index) const -> double;
  /// Photo at the start of the row at @p y: a laid-out photo, or the estimated photo index of a
  /// position below the loaded rows (not clamped to the result).
  [[nodiscard]] auto EstimatedIndexAtY(double y) const -> int;

 private:
  [[nodiscard]] auto        UsableGeometry() const -> bool;
  [[nodiscard]] static auto ClampAspect(double aspect_ratio) -> double;
  /// Photos per estimated row and the height of one estimated row with its gap.
  [[nodiscard]] auto        EstimatedPhotosPerRow() const -> int;
  [[nodiscard]] auto        EstimatedRowPitch() const -> double;
  /// Lay out the rows from row @p row_index to the end; the rows above it stay.
  void                      LayoutFromRow(int row_index);
  /// Lay out the rows from the row that holds photo @p index (the last row for an index after
  /// the laid-out photos).
  void                      LayoutFromIndex(int index);

  JustifiedLayoutGeometry   geometry_{};
  std::vector<double>       aspects_{};  ///< Clamped aspect ratio of each photo.
  std::vector<Row>          rows_{};
  std::vector<double>       tile_x_{};      ///< Left edge of each laid-out tile.
  std::vector<double>       tile_width_{};  ///< Width of each laid-out tile.
  double                    aspect_sum_        = 0.0;
  int                       first_changed_row_ = -1;
};

}  // namespace alcedo::ui
