//  Copyright 2025-2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <ctime>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "storage/mapper/duckorm/duckdb_expr.hpp"
#include "type/type.hpp"

namespace alcedo {
enum class FilterOp { AND, OR, NOT };

/**
 * @brief Filterable fields in Image metadata
 *
 */
enum class FilterField {
  ExifCameraModel,
  ExifFocalLength,
  ExifAperture,
  ExifISO,
  CaptureDate,
  ImportDate,
  FileName,
  FileExtension,
  ImageSize,
  Rating,
  ImagePath,
  // Phase 2: stats-bar bucket columns. Each column must equal the bucket
  // expression used by ElementController::BuildFolderStats GROUP BY so a
  // clicked bucket label selects exactly the rows that produced that bucket.
  CameraModelLabel,
  LensLabel,
  CaptureDateLabel,
  RatingLabel,
  SemanticTags
};

enum class CompareOp {
  EQUALS,
  NOT_EQUALS,
  CONTAINS,
  NOT_CONTAINS,
  GREATER_THAN,
  LESS_THAN,
  GREATER_EQUAL,
  LESS_EQUAL,
  STARTS_WITH,
  ENDS_WITH,
  BETWEEN,
  REGEX
};

using FilterValue = std::variant<std::monostate, int64_t, double, bool, std::wstring, std::tm>;

struct FieldCondition {
  FilterField                field_;
  CompareOp                  op_;
  FilterValue                value_;
  std::optional<FilterValue> second_value_ = std::nullopt;  // Used for BETWEEN condition
};

struct FilterNode {
  enum class Type { Logical, Condition, RawSQL } type_;

  // For Logical nodes
  FilterOp                op_;
  std::vector<FilterNode> children_;

  // For Condition nodes
  std::optional<FieldCondition> condition_;

  // For RawSQL nodes
  std::optional<std::wstring> raw_sql_;
};

/**
 * @brief Compiles a FilterNode tree into a duckorm WHERE fragment.
 *
 * @details Maps domain FilterField values onto scoped album-query columns.
 * Alias rule (must match BuildScopedFileQuery):
 * - Image columns use `i.` (`i.metadata`, `i.file_name`, `i.image_path`)
 * - Element columns use `e.` (`e.element_name`, `e.added_time`)
 *
 * This compiler builds only a WHERE predicate. Stores own FROM/JOIN scope.
 */
class FilterSQLCompiler {
 public:
  /**
   * @brief Compile a filter tree into a SqlFragment WHERE predicate.
   *
   * @param node Root of the filter tree.
   * @return Fragment with escaped/embedded literals. Empty when the tree is empty.
   *
   * @note String values are escaped. RawSQL nodes are a temporary bridge only.
   */
  static auto Compile(const FilterNode& node) -> duckorm::SqlFragment;

 private:
  static auto CompileNode(const FilterNode& node) -> duckorm::SqlFragment;
  static auto GenerateCondition(const FieldCondition& cond) -> duckorm::SqlFragment;
  static auto FieldToColumn(FilterField field) -> duckorm::SqlFragment;
  static auto ValueToFragment(const FilterValue& value) -> duckorm::SqlFragment;
};

/**
 * @brief Merge two optional filter trees under one AND root.
 *
 * @param left Left filter (for example the stats-bar filter).
 * @param right Right filter (for example the active search filter).
 * @return The merged tree, or the single present side, or std::nullopt when
 * both sides are absent.
 */
[[nodiscard]] auto MergeFilterNodes(const std::optional<FilterNode>& left,
                                    const std::optional<FilterNode>& right)
    -> std::optional<FilterNode>;

/**
 * @brief Compile an optional filter tree into scoped-query WHERE text.
 *
 * @param node Filter tree (already merged by the caller).
 * @return UTF-8 WHERE predicate text, or std::nullopt when the tree is absent
 * or compiles to an empty fragment.
 */
[[nodiscard]] auto CompileFilterWhere(const std::optional<FilterNode>& node)
    -> std::optional<std::wstring>;

class FilterCombo {
 public:
  filter_id_t filter_id_;

 private:
  FilterNode root_;

 public:
  FilterCombo() = default;
  FilterCombo(const filter_id_t id, const FilterNode& root) : filter_id_(id), root_(root) {}

  const FilterNode& GetRoot() const { return root_; }

  void SetRoot(const FilterNode& root) { root_ = root; }
};
};  // namespace alcedo
