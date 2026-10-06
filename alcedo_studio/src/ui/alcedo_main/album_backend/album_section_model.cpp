//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/album_section_model.hpp"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>
#include <unordered_set>
#include <variant>

namespace alcedo::ui {
namespace {

auto CeilDiv(int64_t value, int64_t divisor) -> int64_t { return (value + divisor - 1) / divisor; }

auto RangeMap(int64_t begin, int64_t end) -> QVariantMap {
  return QVariantMap{{QStringLiteral("begin"), static_cast<qint64>(begin)},
                     {QStringLiteral("end"), static_cast<qint64>(end)}};
}

}  // namespace

AlbumSectionModel::AlbumSectionModel(QObject* parent) : QAbstractListModel(parent) {}

int AlbumSectionModel::rowCount(const QModelIndex& parent) const {
  if (parent.isValid()) {
    return 0;
  }
  return static_cast<int>(std::min<int64_t>(row_count_, std::numeric_limits<int>::max()));
}

auto AlbumSectionModel::KeyText(const AlbumGroupKey& key) -> std::string {
  if (const auto* number = std::get_if<int64_t>(&key)) {
    return std::to_string(*number);
  }
  if (const auto* text = std::get_if<std::string>(&key)) {
    return *text;
  }
  return {};
}

auto AlbumSectionModel::CollapseKey(const AlbumGroupKey& key) -> std::string {
  if (std::holds_alternative<std::monostate>(key)) {
    return std::string("unknown");
  }
  return (std::holds_alternative<int64_t>(key) ? "n:" : "t:") + KeyText(key);
}

QVariant AlbumSectionModel::data(const QModelIndex& index, int role) const {
  if (!index.isValid() || index.row() < 0 || index.row() >= rowCount()) {
    return {};
  }
  const auto  row   = RowAt(index.row());
  const auto& group = groups_[static_cast<size_t>(row.group_index_)];
  switch (role) {
    case RowKind:
      return static_cast<int>(row.type_);
    case GroupIndex:
      return row.group_index_;
    case GroupTitle:
      return QString::fromStdString(KeyText(group.key_));
    case GroupUnknown:
      return std::holds_alternative<std::monostate>(group.key_);
    case PhotoCount:
      return static_cast<qint64>(group.photo_count_);
    case Collapsed:
      return IsGroupCollapsed(row.group_index_);
    case FirstOccurrence:
      return static_cast<qint64>(row.first_occurrence_);
    case RowOccurrenceCount:
      return static_cast<qint64>(row.occurrence_count_);
    case FileIds: {
      QVariantList ids;
      ids.reserve(static_cast<qsizetype>(row.occurrence_count_));
      for (int64_t offset = 0; offset < row.occurrence_count_; ++offset) {
        ids.push_back(FileIdAt(row.first_occurrence_ + offset));
      }
      return ids;
    }
    default:
      return {};
  }
}

QHash<int, QByteArray> AlbumSectionModel::roleNames() const {
  return {{RowKind, "rowKind"},
          {GroupIndex, "groupIndex"},
          {GroupTitle, "groupTitle"},
          {GroupUnknown, "groupUnknown"},
          {PhotoCount, "photoCount"},
          {Collapsed, "collapsed"},
          {FirstOccurrence, "firstOccurrence"},
          {RowOccurrenceCount, "occurrenceCount"},
          {FileIds, "fileIds"}};
}

void AlbumSectionModel::ResetGroups(std::vector<AlbumGroupDescriptor> groups,
                                    int64_t unique_file_count, int64_t occurrence_count,
                                    bool keep_collapse_state) {
  BeginReplace();
  InstallGroups(std::move(groups), unique_file_count, occurrence_count, keep_collapse_state);
  EndReplace();
}

void AlbumSectionModel::BeginReplace() {
  if (replacing_) {
    return;
  }
  replacing_ = true;
  beginResetModel();
}

void AlbumSectionModel::InstallGroups(std::vector<AlbumGroupDescriptor> groups,
                                      int64_t unique_file_count, int64_t occurrence_count,
                                      bool keep_collapse_state) {
  if (keep_collapse_state) {
    std::set<std::string> kept;
    for (const auto& group : groups) {
      const auto key = CollapseKey(group.key_);
      if (collapsed_keys_.contains(key)) {
        kept.insert(key);
      }
    }
    collapsed_keys_ = std::move(kept);
  } else {
    collapsed_keys_.clear();
  }
  groups_            = std::move(groups);
  unique_file_count_ = unique_file_count;
  occurrence_count_  = occurrence_count;
  pages_.clear();
  RebuildRowStarts();
}

void AlbumSectionModel::EndReplace() {
  if (!replacing_) {
    beginResetModel();
  }
  replacing_ = false;
  endResetModel();
  emit SectionsChanged();
  emit PagesChanged();
}

void AlbumSectionModel::Clear() { ResetGroups({}, 0, 0, false); }

auto AlbumSectionModel::GroupRowCount(int group_index) const -> int64_t {
  const auto& group = groups_[static_cast<size_t>(group_index)];
  if (IsGroupCollapsed(group_index)) {
    return 1;
  }
  return 1 + CeilDiv(group.photo_count_, column_count_);
}

void AlbumSectionModel::RebuildRowStarts() {
  row_starts_.clear();
  row_starts_.reserve(groups_.size());
  int64_t next = 0;
  for (int index = 0; index < static_cast<int>(groups_.size()); ++index) {
    row_starts_.push_back(next);
    next += GroupRowCount(index);
  }
  row_count_ = next;
}

void AlbumSectionModel::RebuildRows() {
  beginResetModel();
  RebuildRowStarts();
  endResetModel();
  emit SectionsChanged();
}

auto AlbumSectionModel::GroupOfRow(int row) const -> int {
  const auto it = std::upper_bound(row_starts_.begin(), row_starts_.end(), row);
  return static_cast<int>(std::distance(row_starts_.begin(), it)) - 1;
}

auto AlbumSectionModel::GroupOfOccurrence(int64_t occurrence) const -> int {
  if (occurrence < 0 || occurrence >= occurrence_count_ || groups_.empty()) {
    return -1;
  }
  const auto it = std::upper_bound(groups_.begin(), groups_.end(), occurrence,
                                   [](int64_t value, const AlbumGroupDescriptor& group) {
                                     return value < group.first_occurrence_;
                                   });
  return static_cast<int>(std::distance(groups_.begin(), it)) - 1;
}

auto AlbumSectionModel::RowAt(int row) const -> Row {
  Row out;
  if (row < 0 || row >= row_count_) {
    return out;
  }
  const int   group_index = GroupOfRow(row);
  const auto& group       = groups_[static_cast<size_t>(group_index)];
  const auto  local_row   = row - row_starts_[static_cast<size_t>(group_index)];
  out.group_index_        = group_index;
  if (local_row == 0) {
    out.type_             = RowType::kHeader;
    out.first_occurrence_ = group.first_occurrence_;
    return out;
  }
  out.type_             = RowType::kPhotos;
  const int64_t start   = (local_row - 1) * column_count_;
  out.first_occurrence_ = group.first_occurrence_ + start;
  out.occurrence_count_ = std::min<int64_t>(column_count_, group.photo_count_ - start);
  return out;
}

auto AlbumSectionModel::LoadedOccurrenceCount() const -> size_t {
  size_t count = 0;
  for (const auto& [start, ids] : pages_) {
    count += ids.size();
  }
  return count;
}

auto AlbumSectionModel::LoadedPageStarts() const -> std::vector<int64_t> {
  std::vector<int64_t> starts;
  starts.reserve(pages_.size());
  for (const auto& [start, ids] : pages_) {
    starts.push_back(start);
  }
  return starts;
}

auto AlbumSectionModel::LoadedUniqueFileIds() const -> std::vector<sl_element_id_t> {
  std::vector<sl_element_id_t>        out;
  std::unordered_set<sl_element_id_t> seen;
  for (const auto& [start, ids] : pages_) {
    for (const auto id : ids) {
      if (seen.insert(id).second) {
        out.push_back(id);
      }
    }
  }
  return out;
}

void AlbumSectionModel::StorePage(int64_t                             first_occurrence,
                                  const std::vector<sl_element_id_t>& file_ids,
                                  size_t                              max_retained_pages) {
  if (file_ids.empty()) {
    return;
  }
  pages_[first_occurrence] = file_ids;
  // Evict the page farthest from the stored page until the retention limit holds.
  while (pages_.size() > std::max<size_t>(1, max_retained_pages)) {
    const auto first          = pages_.begin();
    const auto last           = std::prev(pages_.end());
    const auto front_distance = first_occurrence - first->first;
    const auto back_distance  = last->first - first_occurrence;
    const auto victim         = back_distance > front_distance ? last : first;
    const auto begin          = victim->first;
    const auto end            = victim->first + static_cast<int64_t>(victim->second.size());
    pages_.erase(victim);
    EmitRowsChangedForOccurrences(begin, end);
  }
  EmitRowsChangedForOccurrences(first_occurrence,
                                first_occurrence + static_cast<int64_t>(file_ids.size()));
  emit PagesChanged();
}

void AlbumSectionModel::EmitRowsChangedForOccurrences(int64_t begin, int64_t end) {
  if (row_count_ == 0 || begin >= end) {
    return;
  }
  const int first_row = RowForOccurrence(begin);
  const int last_row  = RowForOccurrence(std::min(end, occurrence_count_) - 1);
  if (first_row < 0 || last_row < 0) {
    return;
  }
  emit dataChanged(index(std::min(first_row, last_row)), index(std::max(first_row, last_row)),
                   {FileIds});
}

void AlbumSectionModel::SetColumnCount(int columns) {
  const int clamped = std::max(1, columns);
  if (clamped == column_count_) {
    return;
  }
  column_count_ = clamped;
  RebuildRows();
}

void AlbumSectionModel::ApplyGroupCollapse(int group_index, bool collapsed) {
  // The photo rows of the group are inserted or removed below its header; the other rows stay,
  // so a view keeps its scroll position (a model reset moves it to the top).
  const auto& group      = groups_[static_cast<size_t>(group_index)];
  const int   header_row = static_cast<int>(row_starts_[static_cast<size_t>(group_index)]);
  const int   photo_rows = static_cast<int>(CeilDiv(group.photo_count_, column_count_));
  if (photo_rows > 0) {
    if (collapsed) {
      beginRemoveRows({}, header_row + 1, header_row + photo_rows);
    } else {
      beginInsertRows({}, header_row + 1, header_row + photo_rows);
    }
  }
  const auto key = CollapseKey(group.key_);
  if (collapsed) {
    collapsed_keys_.insert(key);
  } else {
    collapsed_keys_.erase(key);
  }
  RebuildRowStarts();
  if (photo_rows > 0) {
    if (collapsed) {
      endRemoveRows();
    } else {
      endInsertRows();
    }
  }
  emit dataChanged(index(header_row), index(header_row), {Collapsed});
}

void AlbumSectionModel::SetGroupCollapsed(int group_index, bool collapsed) {
  if (group_index < 0 || group_index >= GroupCount() ||
      IsGroupCollapsed(group_index) == collapsed) {
    return;
  }
  ApplyGroupCollapse(group_index, collapsed);
  emit SectionsChanged();
}

void AlbumSectionModel::ExpandAll() {
  if (collapsed_keys_.empty()) {
    return;
  }
  for (int index = 0; index < GroupCount(); ++index) {
    if (IsGroupCollapsed(index)) {
      ApplyGroupCollapse(index, false);
    }
  }
  emit SectionsChanged();
}

void AlbumSectionModel::CollapseAll() {
  bool changed = false;
  for (int index = 0; index < GroupCount(); ++index) {
    if (!IsGroupCollapsed(index)) {
      ApplyGroupCollapse(index, true);
      changed = true;
    }
  }
  if (changed) {
    emit SectionsChanged();
  }
}

bool AlbumSectionModel::IsGroupCollapsed(int group_index) const {
  if (group_index < 0 || group_index >= GroupCount()) {
    return false;
  }
  return collapsed_keys_.contains(CollapseKey(groups_[static_cast<size_t>(group_index)].key_));
}

int AlbumSectionModel::RowForOccurrence(qint64 occurrence) const {
  const int group_index = GroupOfOccurrence(occurrence);
  if (group_index < 0) {
    return -1;
  }
  const auto start = row_starts_[static_cast<size_t>(group_index)];
  if (IsGroupCollapsed(group_index)) {
    return static_cast<int>(start);
  }
  const auto local = occurrence - groups_[static_cast<size_t>(group_index)].first_occurrence_;
  return static_cast<int>(start + 1 + local / column_count_);
}

QVariantMap AlbumSectionModel::RowInfo(int row) const {
  if (row < 0 || row >= row_count_) {
    return QVariantMap{{QStringLiteral("kind"), -1}};
  }
  const auto info = RowAt(row);
  return QVariantMap{
      {QStringLiteral("kind"), static_cast<int>(info.type_)},
      {QStringLiteral("groupIndex"), info.group_index_},
      {QStringLiteral("firstOccurrence"), static_cast<qint64>(info.first_occurrence_)},
      {QStringLiteral("occurrenceCount"), static_cast<qint64>(info.occurrence_count_)}};
}

int AlbumSectionModel::GroupForOccurrence(qint64 occurrence) const {
  return GroupOfOccurrence(occurrence);
}

int AlbumSectionModel::GroupHeaderRow(int group_index) const {
  if (group_index < 0 || group_index >= GroupCount()) {
    return -1;
  }
  return static_cast<int>(row_starts_[static_cast<size_t>(group_index)]);
}

QVariantMap AlbumSectionModel::OccurrenceRangeForRows(int first_row, int last_row) const {
  int64_t begin = -1;
  int64_t end   = -1;
  for (int row = std::max(0, first_row); row <= last_row && row < row_count_; ++row) {
    const auto info = RowAt(row);
    if (info.type_ != RowType::kPhotos) {
      continue;
    }
    if (begin < 0) {
      begin = info.first_occurrence_;
    }
    end = info.first_occurrence_ + info.occurrence_count_;
  }
  return begin < 0 ? RangeMap(0, 0) : RangeMap(begin, end);
}

QVariantList AlbumSectionModel::VisibleOccurrenceRanges(qint64 first, qint64 last) const {
  QVariantList ranges;
  if (first > last) {
    std::swap(first, last);
  }
  first = std::max<qint64>(0, first);
  last  = std::min<qint64>(occurrence_count_ - 1, last);
  for (int group_index = std::max(0, GroupOfOccurrence(first));
       group_index >= 0 && group_index < GroupCount(); ++group_index) {
    const auto& group = groups_[static_cast<size_t>(group_index)];
    if (group.first_occurrence_ > last) {
      break;
    }
    if (IsGroupCollapsed(group_index)) {
      continue;
    }
    const auto begin = std::max<int64_t>(first, group.first_occurrence_);
    const auto end   = std::min<int64_t>(last + 1, group.first_occurrence_ + group.photo_count_);
    if (begin < end) {
      ranges.push_back(RangeMap(begin, end));
    }
  }
  return ranges;
}

uint AlbumSectionModel::FileIdAt(qint64 occurrence) const {
  auto it = pages_.upper_bound(occurrence);
  if (it == pages_.begin()) {
    return 0;
  }
  --it;
  const auto offset = occurrence - it->first;
  if (offset < 0 || offset >= static_cast<int64_t>(it->second.size())) {
    return 0;
  }
  return static_cast<uint>(it->second[static_cast<size_t>(offset)]);
}

auto AlbumSectionModel::HeaderRowsBefore(int row) const -> int {
  if (row <= 0 || groups_.empty()) {
    return 0;
  }
  if (row >= row_count_) {
    return GroupCount();
  }
  const int group_index = GroupOfRow(row);
  return group_index + (row > row_starts_[static_cast<size_t>(group_index)] ? 1 : 0);
}

double AlbumSectionModel::RowOffset(int row, double header_height, double photo_row_height) const {
  const int clamped = std::clamp<int>(row, 0, static_cast<int>(row_count_));
  const int headers = HeaderRowsBefore(clamped);
  return headers * header_height + (clamped - headers) * photo_row_height;
}

double AlbumSectionModel::ContentHeight(double header_height, double photo_row_height) const {
  return RowOffset(static_cast<int>(row_count_), header_height, photo_row_height);
}

int AlbumSectionModel::RowAtOffset(double offset, double header_height,
                                   double photo_row_height) const {
  if (row_count_ == 0 || offset < 0 || header_height <= 0 || photo_row_height <= 0) {
    return row_count_ == 0 ? -1 : 0;
  }
  // Binary search the last group whose header starts at or before the offset.
  int low  = 0;
  int high = GroupCount() - 1;
  while (low < high) {
    const int  mid   = (low + high + 1) / 2;
    const auto start = RowOffset(static_cast<int>(row_starts_[static_cast<size_t>(mid)]),
                                 header_height, photo_row_height);
    if (start <= offset) {
      low = mid;
    } else {
      high = mid - 1;
    }
  }
  const auto first_row   = row_starts_[static_cast<size_t>(low)];
  const auto group_start = RowOffset(static_cast<int>(first_row), header_height, photo_row_height);
  if (offset < group_start + header_height) {
    return static_cast<int>(first_row);
  }
  const auto photo_rows = GroupRowCount(low) - 1;
  const auto local =
      static_cast<int64_t>(std::floor((offset - group_start - header_height) / photo_row_height));
  return static_cast<int>(first_row + 1 +
                          std::min<int64_t>(local, std::max<int64_t>(0, photo_rows - 1)));
}

}  // namespace alcedo::ui
