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
 * the match set files. The label column holds canonical keys (label assignment writes them,
 * and SemanticLabelStore::CanonicalizeImageLabels rewrites older alias texts on project open),
 * so the key is the lowercase trimmed label. The SQL holds no label literal. Labels of other
 * models do not appear. label_sort holds the minimum key of each labelled file.
 */
[[nodiscard]] auto           LabelRelations(const std::string& active_semantic_model_key)
    -> duckorm::SqlFragment;

/// True when the options read the edit state relation (edit-day group or edit-time sort).
[[nodiscard]] auto UsesEditStateRelation(const AlbumQueryOptions& options) -> bool;

/// Whether a read projects the edit state of each photo. A page read does: every photo row
/// carries its edit time and active Version name. Reads that return only positions, ids, or
/// counts do not, so they join the edit state relation only when the options read it.
enum class EditStateProjection : bool { kNotRead = false, kRead = true };

/**
 * @brief `WITH <relations> ` that the read uses: the label relations (LabelRelations) and the
 *        edit state relation (album_edit_state_sql::EditStateRelation, local day zone of the
 *        options) when the options read it or @p projection is kRead. Empty when the read uses
 *        neither.
 */
[[nodiscard]] auto RelationsPrefix(const AlbumQueryOptions& options,
                                   const std::string&       active_semantic_model_key,
                                   EditStateProjection      projection) -> duckorm::SqlFragment;

/**
 * @brief FROM clause of the ordered occurrence stream.
 *
 * @details One row per matching file, except for the label group, which has one row per
 * canonical label of a file and one unlabelled row for a file without a label (alias `m`).
 * The label sort joins label_sort (alias `ls`). The edit-day group, the edit-time sort, and a
 * read with @p projection kRead left-join edit_state (alias `es`), so an unedited file keeps
 * its row with NULL edit values.
 */
[[nodiscard]] auto OccurrenceSource(const AlbumQueryOptions& options,
                                    EditStateProjection      projection) -> duckorm::SqlFragment;

/// Group key expression over the occurrence source; NULL is the unknown group. Empty for kNone.
[[nodiscard]] auto GroupKeyExpression(AlbumGroupField field) -> duckorm::SqlFragment;

/**
 * @brief Order of the groups of the options' group field. Unknown (NULL) is always last.
 *
 * @details When the photo sort is the same Inspector field as the group field (for example the
 * capture-time sort with capture-day groups), the groups follow the sort direction, so the
 * groups and the photos inside them have one order. Otherwise the group field's fixed order
 * applies (see AlbumQueryOptions).
 */
[[nodiscard]] auto GroupOrderTerm(const AlbumQueryOptions& options) -> duckorm::OrderTerm;

/**
 * @brief The one comparison definition of page, position, and id reads: group key, selected
 *        photo sort, the date group's full timestamp when it is not already the sort, file id.
 *
 * @param has_relevance The match set holds the relevance of a natural-language search (column
 *        `s.relevance`). Without a selected photo sort the photo order is then the relevance
 *        descending and the file id ascending, also inside a date group.
 */
[[nodiscard]] auto OccurrenceOrderTerms(const AlbumQueryOptions& options, bool has_relevance)
    -> std::vector<duckorm::OrderTerm>;

}  // namespace alcedo::album_query_sql
