//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QAbstractListModel>
#include <QHash>
#include <QString>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "sleeve/album_query.hpp"
#include "storage/store/sleeve/element_store.hpp"
#include "type/type.hpp"

namespace alcedo::ui {

/**
 * @brief Section projection of a grouped library result: one logical row for each group
 *        header and one logical row for each line of photo cells.
 *
 * @details Owned by LibraryModule. It stores the accepted group descriptors (key, photo count,
 * first occurrence), the collapse state, the column count, and the file ids of the occurrence
 * pages that are loaded near the viewport. It holds no photo metadata: a cell reads its photo
 * from the thumbnail model by file id.
 *
 * For a group of `n` photos and `c` columns, an expanded group has `1 + ceil(n / c)` rows and a
 * collapsed group has one row. A prefix table of row starts gives the row of an occurrence and
 * the group of a row without one object per photo row. Collapsing a group changes the rows only;
 * it does not change any count.
 *
 * Thread: GUI thread only.
 */
class AlbumSectionModel final : public QAbstractListModel {
  Q_OBJECT
  Q_PROPERTY(int groupCount READ GroupCount NOTIFY SectionsChanged)
  Q_PROPERTY(int columnCount READ ColumnCount NOTIFY SectionsChanged)
  Q_PROPERTY(qint64 occurrenceCount READ OccurrenceTotal NOTIFY SectionsChanged)
  Q_PROPERTY(qint64 uniqueFileCount READ UniqueFileCount NOTIFY SectionsChanged)

 public:
  enum Roles {
    RowKind = Qt::UserRole + 1,  ///< 0 header, 1 photo row.
    GroupIndex,
    GroupTitle,  ///< Key text; empty for the unknown group.
    GroupUnknown,
    PhotoCount,
    Collapsed,
    FirstOccurrence,     ///< First occurrence in a photo row; the group's first for a header.
    RowOccurrenceCount,  ///< Cells in a photo row; 0 for a header.
    FileIds,             ///< File id per cell of a photo row; 0 for a cell not loaded yet.
  };
  enum class RowType : int { kHeader = 0, kPhotos = 1 };

  /// One logical row: the header of a group, or one line of photo cells of that group.
  struct Row {
    RowType type_             = RowType::kHeader;
    int     group_index_      = 0;
    int64_t first_occurrence_ = 0;
    int64_t occurrence_count_ = 0;
  };

  explicit AlbumSectionModel(QObject* parent = nullptr);

  // QAbstractListModel
  int                    rowCount(const QModelIndex& parent = QModelIndex()) const override;
  QVariant               data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
  QHash<int, QByteArray> roleNames() const override;

  /**
   * @brief Replace the groups with the groups of a new accepted result and drop every loaded
   *        page. Collapse state is kept for keys that still exist when @p keep_collapse_state.
   *
   * Emits a model reset. The caller wraps this with the other owners of the same accepted
   * result so no owner shows a different result between the notifications.
   */
  void ResetGroups(std::vector<AlbumGroupDescriptor> groups, int64_t unique_file_count,
                   int64_t occurrence_count, bool keep_collapse_state);
  /// Remove every group (flat mode or no result).
  void Clear();
  /// Start a reset whose groups are installed by InstallGroups and shown by EndReplace. Views
  /// keep the old rows in between, while the other owners of the same result install theirs.
  void BeginReplace();
  /// Install groups without notifying (see ResetGroups for the arguments).
  void InstallGroups(std::vector<AlbumGroupDescriptor> groups, int64_t unique_file_count,
                     int64_t occurrence_count, bool keep_collapse_state);
  /// End the reset that BeginReplace started and emit SectionsChanged and PagesChanged.
  void EndReplace();

  /// Store the file ids of the occurrences that start at @p first_occurrence. A page that
  /// overlaps an evicted range is kept; pages beyond @p max_retained_pages are evicted from the
  /// end farthest from @p first_occurrence. Emits dataChanged for the rows of the stored page.
  void StorePage(int64_t first_occurrence, const std::vector<sl_element_id_t>& file_ids,
                 size_t max_retained_pages);

  [[nodiscard]] auto GroupCount() const -> int { return static_cast<int>(groups_.size()); }
  [[nodiscard]] auto ColumnCount() const -> int { return column_count_; }
  [[nodiscard]] auto OccurrenceTotal() const -> qint64 { return occurrence_count_; }
  [[nodiscard]] auto UniqueFileCount() const -> qint64 { return unique_file_count_; }
  [[nodiscard]] auto groups() const -> const std::vector<AlbumGroupDescriptor>& { return groups_; }
  [[nodiscard]] auto RowAt(int row) const -> Row;
  /// Number of loaded pages and of loaded occurrence ids (bounded-retention evidence).
  [[nodiscard]] auto LoadedPageCount() const -> size_t { return pages_.size(); }
  [[nodiscard]] auto LoadedOccurrenceCount() const -> size_t;
  /// Ordered first occurrences of the loaded pages.
  [[nodiscard]] auto LoadedPageStarts() const -> std::vector<int64_t>;
  /// Unique file ids of the loaded pages, in occurrence order of each file's first occurrence.
  [[nodiscard]] auto LoadedUniqueFileIds() const -> std::vector<sl_element_id_t>;

  /// Set the number of photo cells in one row (at least 1). Rebuilds the rows on a change.
  Q_INVOKABLE void   SetColumnCount(int columns);
  Q_INVOKABLE void   SetGroupCollapsed(int group_index, bool collapsed);
  Q_INVOKABLE void   ExpandAll();
  Q_INVOKABLE void   CollapseAll();
  [[nodiscard]] Q_INVOKABLE bool         IsGroupCollapsed(int group_index) const;
  /// Row that shows @p occurrence: its photo row, or its header row when the group is collapsed.
  /// -1 when the occurrence is out of range.
  [[nodiscard]] Q_INVOKABLE int          RowForOccurrence(qint64 occurrence) const;
  /// `{kind, groupIndex, firstOccurrence, occurrenceCount}` of @p row (kind 0 header, 1
  /// photos; -1 out of range). Reads the prefix table, so it works for rows a view has not
  /// created (keyboard navigation).
  [[nodiscard]] Q_INVOKABLE QVariantMap  RowInfo(int row) const;
  /// Group index of @p occurrence; -1 when it is out of range.
  [[nodiscard]] Q_INVOKABLE int          GroupForOccurrence(qint64 occurrence) const;
  /// Header row of group @p group_index; -1 when it is out of range.
  [[nodiscard]] Q_INVOKABLE int          GroupHeaderRow(int group_index) const;
  /// `{begin, end}` occurrences shown by the photo rows in [@p first_row, @p last_row]; an empty
  /// range when those rows show no photo.
  [[nodiscard]] Q_INVOKABLE QVariantMap  OccurrenceRangeForRows(int first_row, int last_row) const;
  /// Occurrence ranges between two occurrences (inclusive) in visible order, without the
  /// occurrences of collapsed groups. Each entry is `{begin, end}`.
  [[nodiscard]] Q_INVOKABLE QVariantList VisibleOccurrenceRanges(qint64 first, qint64 last) const;
  /// File id of @p occurrence, or 0 when its page is not loaded.
  [[nodiscard]] Q_INVOKABLE uint         FileIdAt(qint64 occurrence) const;
  /// Vertical offset of @p row from the documented row heights: every header row is
  /// @p header_height high and every photo row is @p photo_row_height high.
  [[nodiscard]] Q_INVOKABLE double       RowOffset(int row, double header_height,
                                                   double photo_row_height) const;
  /// Total height of all rows with the same geometry as RowOffset.
  [[nodiscard]] Q_INVOKABLE double       ContentHeight(double header_height,
                                                       double photo_row_height) const;
  /// Row whose span holds @p offset with the same geometry as RowOffset; -1 when no row exists.
  [[nodiscard]] Q_INVOKABLE int          RowAtOffset(double offset, double header_height,
                                                     double photo_row_height) const;

 signals:
  void SectionsChanged();
  void PagesChanged();

 private:
  [[nodiscard]] static auto         KeyText(const AlbumGroupKey& key) -> std::string;
  /// Collapse-state key of a group; the unknown group has its own key.
  [[nodiscard]] static auto         CollapseKey(const AlbumGroupKey& key) -> std::string;
  [[nodiscard]] auto                GroupRowCount(int group_index) const -> int64_t;
  /// Rebuild the prefix table of row starts. Does not emit.
  void                              RebuildRowStarts();
  void                              RebuildRows();
  /// Insert or remove the photo rows of one group below its header (no model reset) and
  /// notify the header's Collapsed role. The group's state must differ from @p collapsed.
  void                              ApplyGroupCollapse(int group_index, bool collapsed);
  /// Group whose rows hold @p row (binary search on the prefix table).
  [[nodiscard]] auto                GroupOfRow(int row) const -> int;
  [[nodiscard]] auto                GroupOfOccurrence(int64_t occurrence) const -> int;
  [[nodiscard]] auto                HeaderRowsBefore(int row) const -> int;
  void                              EmitRowsChangedForOccurrences(int64_t begin, int64_t end);

  std::vector<AlbumGroupDescriptor> groups_{};
  std::vector<int64_t>              row_starts_{};  ///< First row of each group.
  int64_t                           row_count_         = 0;
  int64_t                           unique_file_count_ = 0;
  int64_t                           occurrence_count_  = 0;
  int                               column_count_      = 1;
  bool                              replacing_         = false;
  std::set<std::string>             collapsed_keys_{};
  std::map<int64_t, std::vector<sl_element_id_t>> pages_{};  ///< First occurrence -> file ids.
};

}  // namespace alcedo::ui
