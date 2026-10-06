//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// SQL meaning of the library presentation options. Used by ElementStore only; the application
// layer passes AlbumQueryOptions and never sees these fragments.

#pragma once

#include <string>
#include <vector>

#include "sleeve/album_query.hpp"
#include "storage/mapper/duckorm/duckdb_expr.hpp"
#include "storage/mapper/duckorm/duckdb_select.hpp"

namespace alcedo::album_query_sql {

/// Temporary table that holds the matching files of one read (see ElementStore). Alias `s`.
inline constexpr const char* kMatchSetTable = "SearchMatchSet";

/**
 * @brief Match set column that holds the local import day.
 *
 * @param time_zone IANA zone. Empty yields a NULL DATE, so no import-day value exists.
 * @return `CAST(timezone(?, timezone('UTC', e.added_time)) AS DATE)` with the zone bound:
 *         `e.added_time` is UTC by application convention.
 */
[[nodiscard]] auto           ImportDayColumn(const std::string& time_zone) -> duckorm::SqlFragment;

/// True when the options read the canonical label relation (label group or label sort).
[[nodiscard]] auto           UsesLabelRelation(const AlbumQueryOptions& options) -> bool;

/**
 * @brief `label_membership AS (...), label_sort AS (...)` for the active model: definitions
 *        for a common table expression list (see RelationsPrefix).
 *
 * @details label_membership holds one row per distinct canonical `(file_id, label_key)` pair of
 * the match set files. Alias-equivalent labels map to one canonical key with the taxonomy
 * alias rules (SemanticLabelCanonicalLookup); a label outside the taxonomy keeps its lowercase
 * trimmed text as key. Labels of other models do not appear. label_sort holds the minimum key
 * of each labelled file.
 */
[[nodiscard]] auto           LabelRelations(const std::string& active_semantic_model_key)
    -> duckorm::SqlFragment;

/// True when the options read the edit state relation (edit-day group or edit-time sort).
[[nodiscard]] auto UsesEditStateRelation(const AlbumQueryOptions& options) -> bool;

/**
 * @brief `WITH <relations> ` that the options read: the label relations (LabelRelations) and
 *        the edit state relation (album_edit_state_sql::EditStateRelation, local day zone of
 *        the options). Empty when the options read neither.
 */
[[nodiscard]] auto RelationsPrefix(const AlbumQueryOptions& options,
                                   const std::string&       active_semantic_model_key)
    -> duckorm::SqlFragment;

/**
 * @brief FROM clause of the ordered occurrence stream.
 *
 * @details One row per matching file, except for the label group, which has one row per
 * canonical label of a file and one unlabelled row for a file without a label (alias `m`).
 * The label sort joins label_sort (alias `ls`). The edit-day group and the edit-time sort
 * left-join edit_state (alias `es`), so an unedited file keeps its row with NULL edit values.
 */
[[nodiscard]] auto OccurrenceSource(const AlbumQueryOptions& options) -> duckorm::SqlFragment;

/// Group key expression over the occurrence source; NULL is the unknown group. Empty for kNone.
[[nodiscard]] auto GroupKeyExpression(AlbumGroupField field) -> duckorm::SqlFragment;

/// Fixed order of the groups of @p field. Unknown (NULL) is always last.
[[nodiscard]] auto GroupOrderTerm(AlbumGroupField field) -> duckorm::OrderTerm;

/**
 * @brief The one comparison definition of page, position, and id reads: group key, selected
 *        photo sort, the date group's full timestamp when it is not already the sort, file id.
 */
[[nodiscard]] auto OccurrenceOrderTerms(const AlbumQueryOptions& options)
    -> std::vector<duckorm::OrderTerm>;

}  // namespace alcedo::album_query_sql
