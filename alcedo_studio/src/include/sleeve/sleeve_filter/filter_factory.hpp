//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <string>

#include "image/color_label.hpp"
#include "sleeve/sleeve_filter/filter_combo.hpp"

// Builders of the stats bar bucket filters. Each returns a StatsBucket node, a domain value;
// storage compiles it with the bucket predicate of its stats dimension
// (storage/query/stats_dimension.hpp), the definition that the stats read counts with.

namespace alcedo::sleeve_filter {

/**
 * @brief Build a filter node for a camera-model stats-bar bucket label.
 *
 * @param label Bucket label as the stats read returns it (may be the unknown label
 *        "(unknown)").
 * @return StatsBucket node of kCameraModel.
 */
[[nodiscard]] auto BuildCameraModelBucketFilter(const std::wstring& label) -> FilterNode;

/**
 * @brief Build a filter node for a lens stats-bar bucket label.
 *
 * @param label Bucket label as the stats read returns it (may be the unknown label
 *        "(unknown)").
 * @return StatsBucket node of kLens.
 */
[[nodiscard]] auto BuildLensBucketFilter(const std::wstring& label) -> FilterNode;

/**
 * @brief Build a filter node for a capture-date bucket label.
 *
 * @param date_yyyy_mm_dd Bucket label in `YYYY-MM-DD` form.
 * @return StatsBucket node of kCaptureDate.
 */
[[nodiscard]] auto BuildCaptureDateBucketFilter(const std::wstring& date_yyyy_mm_dd) -> FilterNode;

/**
 * @brief Build a filter node that matches files with no usable capture date.
 *
 * @return StatsBucket node of the kCaptureDate bucket without a value: the Image
 *         `capture_date` column is NULL (no date, or a date text that does not parse).
 */
[[nodiscard]] auto BuildCaptureDateUnknownFilter() -> FilterNode;

/**
 * @brief Build a filter node for one local import day.
 *
 * @param date_yyyy_mm_dd Local calendar day in `YYYY-MM-DD` form.
 * @param time_zone IANA time zone of that day, the same zone as the import-day groups.
 * @return StatsBucket node of kImportDay. Storage compiles it to a half-open range on the
 *         stored UTC import time between the local midnights of the day and the next day.
 * @throws std::invalid_argument when the date is not a valid `YYYY-MM-DD` day or the zone is
 *         empty. DuckDB reports an unknown zone when the predicate runs.
 */
[[nodiscard]] auto BuildImportDateBucketFilter(const std::wstring& date_yyyy_mm_dd,
                                               const std::string&  time_zone) -> FilterNode;

/**
 * @brief Build a filter node that matches files with no import time (`e.added_time` NULL).
 */
[[nodiscard]] auto BuildImportDateUnknownFilter() -> FilterNode;

/**
 * @brief Build a filter node for a rating stats-bar bucket label.
 *
 * @param label Bucket label. A numeric label selects that rating; any other label (for example
 *        "(unknown)") selects the photos without a rating.
 * @return StatsBucket node of kRating.
 */
[[nodiscard]] auto BuildRatingBucketFilter(const std::wstring& label) -> FilterNode;

/**
 * @brief Build a filter node that matches files whose photo has the color label @p code
 *        (`Image.color_labels` holds its bit).
 */
[[nodiscard]] auto BuildColorLabelFilter(ColorLabel code) -> FilterNode;

/**
 * @brief Build a filter node that matches files whose photo has the Delete mark
 *        (`Image.delete_mark`).
 */
[[nodiscard]] auto BuildDeleteMarkFilter() -> FilterNode;

/**
 * @brief Build a filter node for one semantic label bucket.
 *
 * @param model_key Active semantic model key. An empty key selects no photo (no active model
 *        can produce labels).
 * @param label_key Semantic label key of the bucket, as the stats read returns it. Storage
 *        compares it after the LOWER and TRIM of the stored label, so a key in another case
 *        selects the same bucket.
 * @return StatsBucket node of kSemanticLabel.
 */
[[nodiscard]] auto BuildSemanticLabelBucketFilter(const std::string& model_key,
                                                  const std::string& label_key) -> FilterNode;

}  // namespace alcedo::sleeve_filter
