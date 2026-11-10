//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/library_selection_marks.hpp"

#include <algorithm>

namespace alcedo::ui {

LibrarySelectionMarks::LibrarySelectionMarks(QObject* parent) : QObject(parent) {}

auto LibrarySelectionMarks::Rating() const -> int {
  return !marks_.empty() && min_rating_ == max_rating_ ? min_rating_ : -1;
}

auto LibrarySelectionMarks::RatingMixed() const -> bool {
  return !marks_.empty() && min_rating_ != max_rating_;
}

auto LibrarySelectionMarks::ColorLabelCounts() const -> QVariantList {
  QVariantList counts;
  counts.reserve(kColorLabelCount);
  for (const int count : color_label_counts_) {
    counts.push_back(count);
  }
  return counts;
}

void LibrarySelectionMarks::Replace(const std::vector<SearchResultRow>& rows) {
  marks_.clear();
  marks_.reserve(rows.size());
  for (const auto& row : rows) {
    marks_.insert_or_assign(row.file_id_, PhotoMarks{.rating_       = row.rating_,
                                                     .color_labels_ = row.color_labels_,
                                                     .delete_mark_  = row.delete_mark_});
  }
  Recount();
}

void LibrarySelectionMarks::Clear() {
  marks_.clear();
  Recount();
}

void LibrarySelectionMarks::ApplyRating(std::span<const sl_element_id_t> element_ids, int rating) {
  ++local_change_revision_;
  for (const auto element_id : element_ids) {
    if (const auto it = marks_.find(element_id); it != marks_.end()) {
      it->second.rating_ = rating;
    }
  }
  Recount();
}

void LibrarySelectionMarks::ApplyColorLabel(std::span<const sl_element_id_t> element_ids,
                                            ColorLabel code, bool on) {
  ++local_change_revision_;
  for (const auto element_id : element_ids) {
    if (const auto it = marks_.find(element_id); it != marks_.end()) {
      it->second.color_labels_ = WithColorLabel(it->second.color_labels_, code, on);
    }
  }
  Recount();
}

void LibrarySelectionMarks::ApplyDeleteMark(std::span<const sl_element_id_t> element_ids, bool on) {
  ++local_change_revision_;
  for (const auto element_id : element_ids) {
    if (const auto it = marks_.find(element_id); it != marks_.end()) {
      it->second.delete_mark_ = on;
    }
  }
  Recount();
}

void LibrarySelectionMarks::Recount() {
  int                               min_rating = 0;
  int                               max_rating = 0;
  std::array<int, kColorLabelCount> color_counts{};
  int                               delete_count = 0;
  bool                              first        = true;
  for (const auto& [element_id, marks] : marks_) {
    (void)element_id;
    min_rating = first ? marks.rating_ : std::min(min_rating, marks.rating_);
    max_rating = first ? marks.rating_ : std::max(max_rating, marks.rating_);
    first      = false;
    for (int code = 0; code < kColorLabelCount; ++code) {
      if (HasColorLabel(marks.color_labels_, static_cast<ColorLabel>(code))) {
        ++color_counts[static_cast<size_t>(code)];
      }
    }
    delete_count += marks.delete_mark_ ? 1 : 0;
  }
  const int previous_count = count_;
  count_                   = static_cast<int>(marks_.size());
  if (count_ == previous_count && min_rating == min_rating_ && max_rating == max_rating_ &&
      color_counts == color_label_counts_ && delete_count == delete_mark_count_) {
    return;
  }
  min_rating_         = min_rating;
  max_rating_         = max_rating;
  color_label_counts_ = color_counts;
  delete_mark_count_  = delete_count;
  emit Changed();
}

}  // namespace alcedo::ui
