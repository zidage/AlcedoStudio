//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include "sleeve/sleeve_filter/library_search_request.hpp"
#include "storage/mapper/duckorm/duckdb_expr.hpp"

namespace alcedo {

/**
 * @brief The WHERE predicate of one library search over the album scope.
 *
 * @details Every term of @p request must match (Decision D4 of
 * library_search_and_project_size_plan.md). A Date, FileKind, or CaptureParameter term is
 * `(typed predicate OR text alternative)`; a Text term matches the folded search text columns
 * that `request.fields` enables, a literal Text term matches the name, camera, and lens columns
 * as typed, and with the AiTags bit a Text term also matches the CLIP semantic labels of
 * `capabilities.semantic_model_key`. When `capabilities.has_ai_fts_index` is true and both AI
 * field groups are in the mask, the whole query text is also matched against the BM25 index
 * of the AI documents: `(all terms) OR (BM25 match)`.
 *
 * Aliases follow BuildScopedFileQuery: `e` Element, `i` Image. Predicates on other tables
 * (AiImageSearchText, SemanticImageLabel, AiImageFtsDocument) are subqueries on `e.id`.
 * Every user text value is a bound parameter.
 *
 * @param request The parsed query. With `fields` 0 the predicate is `FALSE`.
 * @param capabilities The project indexes, read once by the caller.
 * @return Predicate text with its binds.
 * @throws std::invalid_argument when `fields` is not 0 and `terms` is empty.
 */
[[nodiscard]] auto BuildLibrarySearchPredicate(const LibrarySearchRequest& request,
                                               const SearchCapabilities&   capabilities)
    -> duckorm::SqlFragment;

}  // namespace alcedo
