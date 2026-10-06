//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>
#include <string>
#include <variant>

namespace alcedo {

/// Field of the explicit photo sort of a library read. kNone keeps the defined presentation
/// order (see AlbumQueryOptions).
enum class AlbumSortField : uint8_t {
  kNone,
  kCaptureTime,
  kImportTime,
  kCameraModel,
  kLens,
  kRating,
  kLabels,
  /// Commit time of the active Version head; a photo whose head is the root has no value.
  kEditTime,
};

/// Field that partitions the photos of a library read into groups. kNone is the flat mode.
enum class AlbumGroupField : uint8_t {
  kNone,
  kCaptureDay,
  kImportDay,
  kCameraModel,
  kLens,
  kRating,
  kLabels,
  /// Local day of the active Version head commit; photos whose head is the root form the
  /// unknown (unedited) group.
  kEditDay,
};

/// Direction of the explicit photo sort. It has no effect when the sort field is kNone.
enum class SortDirection : uint8_t { kAscending, kDescending };

/**
 * @brief Presentation options of one library read: one optional group field and one optional
 *        photo sort field. Both can name the same Inspector field.
 *
 * @details Scalar options only. The filter predicate, the scope, and the active semantic
 * model key are separate read inputs. Without a photo sort of the same Inspector field, group
 * order is a fixed rule of each group field: capture, import, and edit days newest first,
 * ratings highest first, cameras, lenses, and labels ascending by key. When the photo sort is
 * the same field as the group field (for example capture time with capture days), the groups
 * follow the sort direction too, so groups and photos have one order. The unknown group is
 * always last.
 *
 * A photo is edited when the head of its active Version is a commit (not the image root); its
 * edit time is that commit's creation time. The commit clock increases strictly, so the head
 * is the newest commit of its first-parent chain.
 *
 * Photo order inside a group (or across all photos in the flat mode) is: the selected photo
 * sort, then for a date group the full timestamp of that group newest first (only when the
 * selected sort is not that timestamp), then the file id ascending. Without a selected sort a
 * date group orders its photos by that timestamp newest first; other groups and the flat mode
 * order by file id ascending.
 */
struct AlbumQueryOptions {
  AlbumSortField  sort_field_     = AlbumSortField::kNone;
  SortDirection   sort_direction_ = SortDirection::kAscending;
  AlbumGroupField group_field_    = AlbumGroupField::kNone;
  /// IANA time zone that turns a UTC instant into a local calendar day (import-day and
  /// edit-day groups, import-day statistics). Required for AlbumGroupField::kImportDay and
  /// AlbumGroupField::kEditDay.
  std::string     local_day_time_zone_{};
};

/**
 * @brief Typed key of one group: the day as `YYYY-MM-DD`, a camera, lens, or canonical label
 *        key as text, or a rating as a number. std::monostate is the unknown group.
 */
using AlbumGroupKey = std::variant<std::monostate, std::string, int64_t>;

/// Largest number of photo rows that one library page read returns.
inline constexpr int64_t kMaxAlbumQueryPageRows = 1000;

/**
 * @brief Reject options that name an unknown enumerator or lack a required time zone.
 *
 * @throws std::invalid_argument with the reason.
 */
void                     ValidateAlbumQueryOptions(const AlbumQueryOptions& options);

}  // namespace alcedo
