//  Copyright 2025-2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <optional>

#include "sleeve/sleeve_filter/filter_combo.hpp"
#include "storage/mapper/duckorm/duckdb_expr.hpp"

namespace alcedo {

/**
 * @brief Compiles a FilterNode tree into a duckorm WHERE fragment.
 *
 * @details Maps domain FilterField values onto scoped album-query columns.
 * Alias rule (must match BuildScopedFileQuery):
 * - Image columns use `i.` (`i.file_name`, `i.image_path`, and the typed search columns
 *   such as `i.camera_model`, `i.capture_date`, `i.rating`; never the metadata JSON)
 * - Element columns use `e.` (`e.id`, `e.element_name`, `e.added_time`)
 *
 * A StatsBucket node compiles to the bucket predicate of its stats dimension over the live
 * tables (stats_dimension::BucketPredicate), the definition that the stats read counts with.
 * A LibrarySearch node compiles to BuildLibrarySearchPredicate of its request and
 * capabilities.
 *
 * This compiler builds only a WHERE predicate. Stores own FROM/JOIN scope.
 */
class FilterSQLCompiler {
 public:
  /**
   * @brief Compile a filter tree into a SqlFragment WHERE predicate.
   *
   * @param node Root of the filter tree.
   * @return Fragment with `?` binds for user values. Empty when the tree is empty.
   * @throws std::invalid_argument when a StatsBucket node has an unknown dimension or a key that
   *         its dimension does not use, or when a LibrarySearch node has fields and no term.
   *
   * @note Typed string/number values use prepared binds. Callers must run the fragment
   * through a prepared statement path (ElementStore album scope queries).
   */
  static auto Compile(const FilterNode& node) -> duckorm::SqlFragment;

 private:
  static auto CompileNode(const FilterNode& node) -> duckorm::SqlFragment;
  static auto GenerateCondition(const FieldCondition& cond) -> duckorm::SqlFragment;
  static auto FieldToColumn(FilterField field) -> duckorm::SqlFragment;
  static auto ValueToFragment(const FilterValue& value) -> duckorm::SqlFragment;
};

/**
 * @brief Compile an optional filter tree into a SqlFragment WHERE predicate.
 *
 * @param node Filter tree (already merged by the caller).
 * @return Fragment with SQL text and binds, or std::nullopt when the tree is
 * absent or compiles to an empty fragment.
 */
[[nodiscard]] auto CompileFilterPredicate(const std::optional<FilterNode>& node)
    -> std::optional<duckorm::SqlFragment>;

}  // namespace alcedo
