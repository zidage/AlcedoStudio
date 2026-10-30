//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/album_section_model.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <iterator>
#include <limits>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
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
  return RowsOfGroup(CollapseKey(group.key_), group.photo_count_);
}

auto AlbumSectionModel::RowsOfGroup(const std::string& collapse_key, int64_t photo_count) const
    -> int64_t {
  if (collapsed_keys_.contains(collapse_key)) {
    return 1;
  }
  return 1 + CeilDiv(photo_count, column_count_);
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
  // Loaded pages that overlap the stored range join it, so no two pages overlap.
  int64_t       page_begin = first_occurrence;
  const int64_t store_end  = first_occurrence + static_cast<int64_t>(file_ids.size());
  int64_t       page_end   = store_end;
  auto          it         = pages_.upper_bound(first_occurrence);
  if (it != pages_.begin() &&
      std::prev(it)->first + static_cast<int64_t>(std::prev(it)->second.size()) >
          first_occurrence) {
    --it;
  }
  std::vector<std::pair<int64_t, std::vector<sl_element_id_t>>> joined;
  while (it != pages_.end() && it->first < store_end) {
    page_begin = std::min(page_begin, it->first);
    page_end   = std::max(page_end, it->first + static_cast<int64_t>(it->second.size()));
    joined.emplace_back(it->first, std::move(it->second));
    it = pages_.erase(it);
  }
  std::vector<sl_element_id_t> page(static_cast<size_t>(page_end - page_begin), 0);
  for (const auto& [start, ids] : joined) {
    std::copy(ids.begin(), ids.end(), page.begin() + (start - page_begin));
  }
  std::copy(file_ids.begin(), file_ids.end(), page.begin() + (first_occurrence - page_begin));
  pages_[page_begin] = std::move(page);
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

auto AlbumSectionModel::FirstUnloadedOccurrence(int64_t begin, int64_t end) const -> int64_t {
  begin = std::max<int64_t>(0, begin);
  end   = std::min(end, occurrence_count_);
  for (int64_t occurrence = begin; occurrence < end;) {
    auto it = pages_.upper_bound(occurrence);
    if (it != pages_.begin()) {
      const auto page     = std::prev(it);
      const auto page_end = page->first + static_cast<int64_t>(page->second.size());
      if (occurrence < page_end) {
        occurrence = page_end;
        continue;
      }
    }
    return occurrence;
  }
  return -1;
}

auto AlbumSectionModel::NextLoadedPageStart(int64_t occurrence) const -> int64_t {
  const auto it = pages_.upper_bound(occurrence);
  return it == pages_.end() ? occurrence_count_ : it->first;
}

auto AlbumSectionModel::LoadedEnd() const -> int64_t {
  if (pages_.empty()) {
    return 0;
  }
  const auto& last = *pages_.rbegin();
  return last.first + static_cast<int64_t>(last.second.size());
}

auto AlbumSectionModel::RetainedPagesInResult(
    const std::vector<AlbumGroupDescriptor>& groups,
    const std::vector<sl_element_id_t>&      occurrence_file_ids) const -> PageMap {
  // New occurrences of the loaded files, with the group that holds each one. One pass over the
  // new result; only the loaded files are recorded.
  std::unordered_map<sl_element_id_t, std::vector<std::pair<int64_t, int>>> new_occurrences;
  for (const auto& [start, ids] : pages_) {
    for (const auto id : ids) {
      if (id != 0) {
        new_occurrences.try_emplace(id);
      }
    }
  }
  int group_index = groups.empty() ? -1 : 0;
  for (int64_t occurrence = 0; occurrence < static_cast<int64_t>(occurrence_file_ids.size());
       ++occurrence) {
    while (group_index >= 0 && group_index + 1 < static_cast<int>(groups.size()) &&
           groups[static_cast<size_t>(group_index) + 1].first_occurrence_ <= occurrence) {
      ++group_index;
    }
    const auto it = new_occurrences.find(occurrence_file_ids[static_cast<size_t>(occurrence)]);
    if (it != new_occurrences.end()) {
      it->second.emplace_back(occurrence, group_index);
    }
  }
  std::unordered_map<std::string, int> new_group_of_key;
  for (int index = 0; index < static_cast<int>(groups.size()); ++index) {
    new_group_of_key.emplace(CollapseKey(groups[static_cast<size_t>(index)].key_), index);
  }

  // The new occurrence range of each page: from its first to its last remaining photo.
  std::vector<std::pair<int64_t, int64_t>> ranges;
  for (const auto& [start, ids] : pages_) {
    int64_t begin = -1;
    int64_t end   = -1;
    for (size_t offset = 0; offset < ids.size(); ++offset) {
      const auto found = new_occurrences.find(ids[offset]);
      if (ids[offset] == 0 || found == new_occurrences.end() || found->second.empty()) {
        continue;
      }
      // The same group key: a label group keeps the occurrence of its own label.
      int wanted_group = -1;
      if (!groups_.empty()) {
        const int old_group = GroupOfOccurrence(start + static_cast<int64_t>(offset));
        if (old_group < 0) {
          continue;
        }
        const auto key =
            new_group_of_key.find(CollapseKey(groups_[static_cast<size_t>(old_group)].key_));
        if (key == new_group_of_key.end()) {
          continue;
        }
        wanted_group = key->second;
      }
      for (const auto& [occurrence, group] : found->second) {
        if (wanted_group < 0 || group == wanted_group) {
          begin = begin < 0 ? occurrence : std::min(begin, occurrence);
          end   = std::max(end, occurrence + 1);
          break;
        }
      }
    }
    if (begin >= 0) {
      ranges.emplace_back(begin, end);
    }
  }
  std::sort(ranges.begin(), ranges.end());

  PageMap pages;
  for (size_t index = 0; index < ranges.size();) {
    auto [begin, end] = ranges[index++];
    while (index < ranges.size() && ranges[index].first < end) {
      end = std::max(end, ranges[index++].second);
    }
    pages.emplace(begin, std::vector<sl_element_id_t>(occurrence_file_ids.begin() + begin,
                                                      occurrence_file_ids.begin() + end));
  }
  return pages;
}

void AlbumSectionModel::ApplyResultChange(std::vector<AlbumGroupDescriptor> groups,
                                          int64_t unique_file_count, int64_t occurrence_count,
                                          PageMap pages) {
  std::set<std::string> new_keys;
  for (const auto& group : groups) {
    new_keys.insert(CollapseKey(group.key_));
  }
  std::set<std::string> old_keys;
  for (const auto& group : groups_) {
    old_keys.insert(CollapseKey(group.key_));
  }

  // Walk both group lists in their common order. Each step keeps groups_ and the row table
  // consistent with the rows that the view was told about; group positions and pages are
  // installed after the last structural change.
  bool   changed  = false;
  size_t position = 0;
  size_t next     = 0;
  while (position < groups_.size() || next < groups.size()) {
    const auto old_key =
        position < groups_.size() ? CollapseKey(groups_[position].key_) : std::string{};
    const auto new_key     = next < groups.size() ? CollapseKey(groups[next].key_) : std::string{};
    const bool old_removed = position < groups_.size() && !new_keys.contains(old_key);
    // A common key out of the common order (not expected for one query) moves the group: it is
    // removed here and inserted at its new position.
    const bool old_moved   = position < groups_.size() && next < groups.size() && !old_removed &&
                           old_key != new_key && old_keys.contains(new_key);
    if (old_removed || old_moved) {
      const int first = static_cast<int>(row_starts_[position]);
      const int rows  = static_cast<int>(GroupRowCount(static_cast<int>(position)));
      beginRemoveRows({}, first, first + rows - 1);
      groups_.erase(groups_.begin() + static_cast<std::ptrdiff_t>(position));
      RebuildRowStarts();
      endRemoveRows();
      if (old_moved) {
        old_keys.erase(old_key);
      }
      changed = true;
      continue;
    }
    if (position >= groups_.size() || !old_keys.contains(new_key)) {
      const int first = position < groups_.size() ? static_cast<int>(row_starts_[position])
                                                  : static_cast<int>(row_count_);
      const int rows  = static_cast<int>(RowsOfGroup(new_key, groups[next].photo_count_));
      beginInsertRows({}, first, first + rows - 1);
      groups_.insert(groups_.begin() + static_cast<std::ptrdiff_t>(position), groups[next]);
      RebuildRowStarts();
      endInsertRows();
      ++position;
      ++next;
      changed = true;
      continue;
    }
    // The same group: photo rows are inserted or removed at its end.
    const int  start    = static_cast<int>(row_starts_[position]);
    const auto old_rows = GroupRowCount(static_cast<int>(position));
    const auto new_rows = RowsOfGroup(new_key, groups[next].photo_count_);
    if (new_rows > old_rows) {
      beginInsertRows({}, start + static_cast<int>(old_rows),
                      start + static_cast<int>(new_rows) - 1);
      groups_[position] = groups[next];
      RebuildRowStarts();
      endInsertRows();
    } else if (new_rows < old_rows) {
      beginRemoveRows({}, start + static_cast<int>(new_rows),
                      start + static_cast<int>(old_rows) - 1);
      groups_[position] = groups[next];
      RebuildRowStarts();
      endRemoveRows();
    }
    changed = changed || groups_[position].photo_count_ != groups[next].photo_count_ ||
              groups_[position].first_occurrence_ != groups[next].first_occurrence_ ||
              new_rows != old_rows;
    groups_[position] = groups[next];
    ++position;
    ++next;
  }

  // Collapse state of keys that no longer exist is dropped, as after a replacement.
  std::erase_if(collapsed_keys_, [&](const std::string& key) { return !new_keys.contains(key); });
  groups_            = std::move(groups);
  unique_file_count_ = unique_file_count;
  occurrence_count_  = occurrence_count;
  changed            = changed || pages != pages_;
  pages_             = std::move(pages);
  RebuildRowStarts();
  if (changed && row_count_ > 0) {
    emit dataChanged(index(0), index(static_cast<int>(row_count_) - 1));
  }
  emit SectionsChanged();
  emit PagesChanged();
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

int AlbumSectionModel::GroupIndexForTitle(const QString& title, bool unknown) const {
  const auto text = title.toStdString();
  for (int index = 0; index < GroupCount(); ++index) {
    const auto& key = groups_[static_cast<size_t>(index)].key_;
    if (unknown ? std::holds_alternative<std::monostate>(key)
                : !std::holds_alternative<std::monostate>(key) && KeyText(key) == text) {
      return index;
    }
  }
  return -1;
}

qint64 AlbumSectionModel::LoadedOccurrenceOf(uint file_id, int group_index) const {
  if (file_id == 0 || group_index < 0 || group_index >= GroupCount()) {
    return -1;
  }
  const auto& group = groups_[static_cast<size_t>(group_index)];
  const auto  begin = group.first_occurrence_;
  const auto  end   = group.first_occurrence_ + group.photo_count_;
  auto        it    = pages_.upper_bound(begin);
  if (it != pages_.begin()) {
    --it;
  }
  for (; it != pages_.end() && it->first < end; ++it) {
    for (size_t offset = 0; offset < it->second.size(); ++offset) {
      const auto occurrence = it->first + static_cast<int64_t>(offset);
      if (occurrence >= begin && occurrence < end && it->second[offset] == file_id) {
        return occurrence;
      }
    }
  }
  return -1;
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
