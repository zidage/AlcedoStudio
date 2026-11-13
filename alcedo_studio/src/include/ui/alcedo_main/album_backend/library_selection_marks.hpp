//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QObject>
#include <QVariantList>
#include <array>
#include <cstdint>
#include <span>
#include <unordered_map>
#include <vector>

#include "image/color_label.hpp"
#include "storage/store/sleeve/element_store.hpp"
#include "type/type.hpp"

namespace alcedo::ui {

/// The rating, the color labels, and the Delete mark of the selected photos, as one summary for
/// the marks control of the Library inspector (#772). Owned by LibraryModule. GUI thread only.
///
/// LibraryModule reads the marks of every selected photo from the project database
/// (Replace) after each selection change and after each rating or mark write. A batch write
/// shows its change here before the write ends (ApplyRating, ApplyColorLabel, ApplyDeleteMark),
/// the same way the library rows show it. The summary keeps the marks of each photo, so a change
/// of some selected photos gives the exact summary.
class LibrarySelectionMarks final : public QObject {
  Q_OBJECT
  /// Selected photos that the summary holds.
  Q_PROPERTY(int count READ Count NOTIFY Changed)
  /// The rating of all photos (0 to 5), or -1 when the ratings differ or nothing is selected.
  Q_PROPERTY(int rating READ Rating NOTIFY Changed)
  /// True when the selected photos have different ratings.
  Q_PROPERTY(bool ratingMixed READ RatingMixed NOTIFY Changed)
  /// Photos with each color label code: kColorLabelCount ints, index = code.
  Q_PROPERTY(QVariantList colorLabelCounts READ ColorLabelCounts NOTIFY Changed)
  /// Photos with the Delete mark.
  Q_PROPERTY(int deleteMarkCount READ DeleteMarkCount NOTIFY Changed)

 public:
  /// The marks of one photo.
  struct PhotoMarks {
    int            rating_       = 0;
    ColorLabelMask color_labels_ = 0;
    bool           delete_mark_  = false;
  };

  explicit LibrarySelectionMarks(QObject* parent = nullptr);

  [[nodiscard]] auto Count() const -> int { return count_; }
  [[nodiscard]] auto Rating() const -> int;
  [[nodiscard]] auto RatingMixed() const -> bool;
  [[nodiscard]] auto ColorLabelCounts() const -> QVariantList;
  [[nodiscard]] auto DeleteMarkCount() const -> int { return delete_mark_count_; }
  [[nodiscard]] auto ColorLabelCount(ColorLabel code) const -> int {
    return color_label_counts_[static_cast<size_t>(code)];
  }

  /// Holds the marks of @p rows (one row per selected photo) in place of the current ones.
  void Replace(const std::vector<SearchResultRow>& rows);
  /// Holds no photo.
  void Clear();
  /// The photos of @p element_ids that the summary holds show @p rating.
  void ApplyRating(std::span<const sl_element_id_t> element_ids, int rating);
  /// The photos of @p element_ids that the summary holds get (@p on) or lose code @p code.
  void ApplyColorLabel(std::span<const sl_element_id_t> element_ids, ColorLabel code, bool on);
  /// The photos of @p element_ids that the summary holds get or lose the Delete mark.
  void ApplyDeleteMark(std::span<const sl_element_id_t> element_ids, bool on);

  /// Increments on each Apply call. A database read that started before an Apply call holds
  /// marks older than the summary; LibraryModule drops such a read.
  [[nodiscard]] auto LocalChangeRevision() const -> std::uint64_t { return local_change_revision_; }

 signals:
  void Changed();

 private:
  /// Counts the summary again from marks_ and emits Changed when it changed.
  void                                            Recount();

  std::unordered_map<sl_element_id_t, PhotoMarks> marks_{};
  int                                             count_      = 0;
  int                                             min_rating_ = 0;
  int                                             max_rating_ = 0;
  std::array<int, kColorLabelCount>               color_label_counts_{};
  int                                             delete_mark_count_     = 0;
  std::uint64_t                                   local_change_revision_ = 0;
};

}  // namespace alcedo::ui
