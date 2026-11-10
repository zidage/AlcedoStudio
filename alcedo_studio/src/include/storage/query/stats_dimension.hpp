//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// The one SQL definition of each library stats dimension. The stats read (ElementStore) groups
// and counts with these expressions, and the filter compiler selects a clicked bucket with the
// bucket predicate of the same dimension, so a clicked bucket selects the photos it counts.

#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "sleeve/sleeve_filter/filter_combo.hpp"
#include "storage/mapper/duckorm/duckdb_expr.hpp"

namespace alcedo::stats_dimension {

/**
 * @brief The rows that an expression reads.
 *
 * - kLiveTables: the album scope of ElementStore (`e` Element, `i` Image).
 * - kMatchSet: the match set table of one stats read (alias `s`, see album_query_sql).
 */
enum class ColumnSource : bool { kLiveTables, kMatchSet };

/**
 * @brief Label of the bucket of the photos without a value.
 *
 * @return `(unknown)` for the camera model and the lens; std::nullopt for the dimensions whose
 *         unknown bucket has a NULL label (capture date, rating, import day). The color label,
 *         Delete mark, and semantic label dimensions have no unknown bucket (std::nullopt).
 * @throws std::invalid_argument for an unknown id.
 */
[[nodiscard]] auto UnknownLabel(StatsDimensionId dimension) -> std::optional<std::string_view>;

/**
 * @brief Bucket label expression, a VARCHAR that is the bucket label or NULL.
 *
 * @details Capture date, camera model, lens, rating, and import day read one match set row
 * (alias `s`; the import day is the `s.import_day` column that ImportDayValue fills). The
 * semantic label reads one SemanticImageLabel row (alias `sl`) that SemanticLabelRowCondition
 * admits. The stats read groups by this expression.
 * @throws std::invalid_argument for kColorLabel and kDeleteMark (a photo can be in several or no
 *         bucket of them; the stats read counts each bucket with BucketPredicate) and for an
 *         unknown id.
 */
[[nodiscard]] auto GroupExpression(StatsDimensionId dimension) -> duckorm::SqlFragment;

/**
 * @brief Predicate of the photos of one bucket.
 *
 * @details For each bucket the predicate admits exactly the rows that GroupExpression puts in
 * that bucket (for color labels and the Delete mark, the rows that the stats read counts). The
 * import day over the live tables is a half-open range on the stored UTC import time between
 * the local midnights of the day and the next day in `condition.time_zone`.
 * @param condition Dimension and key; see StatsBucketKey for the key of each dimension.
 * @param source Rows of the predicate. A semantic label predicate is an EXISTS subquery on the
 *        file id of the source.
 * @throws std::invalid_argument for an unknown id ("Stats dimension: unknown id"), a key type
 *         that the dimension does not use, an invalid day or color label code, and an import day
 *         over the live tables without a time zone.
 */
[[nodiscard]] auto BucketPredicate(const StatsBucketCondition& condition, ColumnSource source)
    -> duckorm::SqlFragment;

/**
 * @brief Local import day of one live Element row (`e.added_time`), a DATE.
 *
 * @param time_zone IANA zone. Empty yields a NULL DATE, so no import-day value exists.
 * @return `CAST(timezone(?, timezone('UTC', e.added_time)) AS DATE)` with the zone bound:
 *         `e.added_time` is UTC by application convention.
 */
[[nodiscard]] auto ImportDayValue(const std::string& time_zone) -> duckorm::SqlFragment;

/**
 * @brief The SemanticImageLabel rows (alias `sl`) of the semantic label buckets.
 *
 * @return `((sl.model_key = ?) AND TRIM(sl.label) <> '')`: rows of the active model with a
 *         label.
 */
[[nodiscard]] auto SemanticLabelRowCondition(const std::string& semantic_model_key)
    -> duckorm::SqlFragment;

}  // namespace alcedo::stats_dimension
