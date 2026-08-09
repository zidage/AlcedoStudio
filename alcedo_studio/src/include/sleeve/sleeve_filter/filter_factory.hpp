//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <span>
#include <string>
#include <vector>

#include "sleeve/sleeve_filter/filter_combo.hpp"

namespace alcedo::sleeve_filter {

/**
 * @brief Build a filter node for a camera-model stats-bar bucket label.
 *
 * @param label Bucket label as shown by the stats bar (may be "(unknown)").
 * @return Typed condition against the camera bucket column.
 */
[[nodiscard]] auto BuildCameraModelBucketFilter(const std::wstring& label) -> FilterNode;

/**
 * @brief Build a filter node for a lens stats-bar bucket label.
 *
 * @param label Bucket label as shown by the stats bar (may be "(unknown)").
 * @return Typed condition against the lens bucket column.
 */
[[nodiscard]] auto BuildLensBucketFilter(const std::wstring& label) -> FilterNode;

/**
 * @brief Build a filter node for a capture-date bucket label.
 *
 * @param date_yyyy_mm_dd Bucket label in `YYYY-MM-DD` form.
 * @return Typed condition against the date bucket column.
 */
[[nodiscard]] auto BuildCaptureDateBucketFilter(const std::wstring& date_yyyy_mm_dd) -> FilterNode;

/**
 * @brief Build a filter node that matches files with no usable capture date.
 *
 * @return Predicate: the raw date string is NULL or empty.
 */
[[nodiscard]] auto BuildCaptureDateUnknownFilter() -> FilterNode;

/**
 * @brief Build a filter node for a rating stats-bar bucket label.
 *
 * @param label Bucket label. A numeric label becomes an integer equality;
 * any other label (for example "(unknown)") becomes a NULL check.
 * @return Typed condition or a NULL-check predicate.
 */
[[nodiscard]] auto BuildRatingBucketFilter(const std::wstring& label) -> FilterNode;

/**
 * @brief Build a filter node that matches files with a semantic label.
 *
 * @param model_key Active semantic model key. An empty key yields a false
 * predicate (no active model can produce labels).
 * @param aliases Label aliases to match with a case-insensitive comparison.
 * @return EXISTS subquery over SemanticImageLabel scoped to the file row.
 */
[[nodiscard]] auto BuildSemanticLabelExistsFilter(const std::string&           model_key,
                                                  std::span<const std::string> aliases)
    -> FilterNode;

}  // namespace alcedo::sleeve_filter
