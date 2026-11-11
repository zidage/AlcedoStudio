//  Copyright 2025-2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "storage/query/filter_compiler.hpp"

#include <ctime>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "sleeve/sleeve_filter/filter_combo.hpp"
#include "storage/mapper/duckorm/duckdb_expr.hpp"
#include "storage/query/library_search_predicates.hpp"
#include "storage/query/stats_dimension.hpp"
#include "utils/string/convert.hpp"

namespace alcedo {
namespace {

using duckorm::SqlFragment;
namespace expr = duckorm::expr;

auto FormatTimestampLiteral(const std::tm& tm_value) -> SqlFragment {
  char buffer[32];
  // TIMESTAMP 'YYYY-MM-DD HH:MM:SS' — trusted format string, values from std::tm fields.
  if (std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &tm_value) == 0) {
    return expr::lit_null();
  }
  return expr::raw(std::string("TIMESTAMP ") + expr::lit(std::string_view(buffer)).sql_);
}

/// `column LIKE ? ESCAPE '~'` with the bound pattern @p prefix + @p text + @p suffix. The text
/// is escaped, so `%`, `_`, and `~` in it match only themselves (#812).
auto LiteralTextLike(SqlFragment column, std::string_view prefix, const std::wstring& text,
                     std::string_view suffix) -> SqlFragment {
  std::string pattern(prefix);
  pattern += expr::escape_like_pattern(conv::ToBytes(text));
  pattern += suffix;
  return expr::like_escape(std::move(column), expr::param(std::move(pattern)));
}

}  // namespace

/**
 * @brief Map a domain field onto a scoped album-query column expression.
 *
 * Alias rule (must match ElementStore::BuildScopedFileQuery):
 * - `i` is the Image row (`i.file_name`, `i.image_path`, and the typed search columns that
 *   ImageMapper writes; the metadata JSON is never read)
 * - `e` is the Element row (`e.element_name`, `e.added_time`)
 */
auto FilterSQLCompiler::FieldToColumn(FilterField field) -> SqlFragment {
  switch (field) {
    case FilterField::ExifCameraModel:
      return expr::col("i.camera_model");
    case FilterField::ExifFocalLength:
      return expr::col("i.focal_mm");
    case FilterField::ExifAperture:
      return expr::col("i.aperture");
    case FilterField::ExifISO:
      return expr::col("i.iso");
    case FilterField::CaptureDate:
      return expr::col("i.capture_at");
    case FilterField::ImportDate:
      return expr::col("e.added_time");
    case FilterField::FileName:
      return expr::col("e.element_name");
    case FilterField::FileExtension:
      return expr::col("UPPER(i.file_name)");
    case FilterField::ImageSize:
      return expr::col("i.pixel_count");
    case FilterField::Rating:
      return expr::col("i.rating");
    case FilterField::ImagePath:
      return expr::col("i.image_path");
    case FilterField::SemanticTags:
      // Semantic label filters are StatsBucket nodes (kSemanticLabel). Placeholder only.
      return expr::col("i.embedding");
    case FilterField::FileId:
      return expr::col("e.id");
  }
  return expr::raw("1=0");
}

auto FilterSQLCompiler::ValueToFragment(const FilterValue& value) -> SqlFragment {
  if (std::holds_alternative<std::monostate>(value)) {
    return expr::lit_null();
  }
  if (std::holds_alternative<int64_t>(value)) {
    return expr::param(std::get<int64_t>(value));
  }
  if (std::holds_alternative<double>(value)) {
    return expr::param(std::get<double>(value));
  }
  if (std::holds_alternative<bool>(value)) {
    return expr::param(std::get<bool>(value));
  }
  if (std::holds_alternative<std::wstring>(value)) {
    return expr::param(conv::ToBytes(std::get<std::wstring>(value)));
  }
  if (std::holds_alternative<std::tm>(value)) {
    // Timestamp literals keep a fixed SQL shape; values come from trusted tm fields.
    return FormatTimestampLiteral(std::get<std::tm>(value));
  }
  return expr::lit_null();
}

auto FilterSQLCompiler::GenerateCondition(const FieldCondition& cond) -> SqlFragment {
  auto column = FieldToColumn(cond.field_);

  switch (cond.op_) {
    case CompareOp::BETWEEN:
      if (!cond.second_value_.has_value()) {
        return expr::raw("1=0");
      }
      return expr::between(std::move(column), ValueToFragment(cond.value_),
                           ValueToFragment(*cond.second_value_));
    case CompareOp::CONTAINS:
      return LiteralTextLike(std::move(column), "%", std::get<std::wstring>(cond.value_), "%");
    case CompareOp::NOT_CONTAINS:
      return expr::not_(
          LiteralTextLike(std::move(column), "%", std::get<std::wstring>(cond.value_), "%"));
    case CompareOp::STARTS_WITH:
      return LiteralTextLike(std::move(column), "", std::get<std::wstring>(cond.value_), "%");
    case CompareOp::ENDS_WITH:
      return LiteralTextLike(std::move(column), "%", std::get<std::wstring>(cond.value_), "");
    case CompareOp::EQUALS:
      return expr::eq(std::move(column), ValueToFragment(cond.value_));
    case CompareOp::NOT_EQUALS:
      return expr::ne(std::move(column), ValueToFragment(cond.value_));
    case CompareOp::GREATER_THAN:
      return expr::gt(std::move(column), ValueToFragment(cond.value_));
    case CompareOp::LESS_THAN:
      return expr::lt(std::move(column), ValueToFragment(cond.value_));
    case CompareOp::GREATER_EQUAL:
      return expr::ge(std::move(column), ValueToFragment(cond.value_));
    case CompareOp::LESS_EQUAL:
      return expr::le(std::move(column), ValueToFragment(cond.value_));
    case CompareOp::REGEX: {
      const auto& text = std::get<std::wstring>(cond.value_);
      // DuckDB REGEXP has no typed helper; bind the pattern as a parameter.
      auto pattern = expr::param(conv::ToBytes(text));
      SqlFragment out;
      out.sql_.append("(");
      out.append(std::move(column));
      out.sql_.append(" REGEXP ");
      out.append(std::move(pattern));
      out.sql_.append(")");
      return out;
    }
  }
  return expr::raw("1=0");
}

auto FilterSQLCompiler::CompileNode(const FilterNode& node) -> SqlFragment {
  if (node.type_ == FilterNode::Type::Condition && node.condition_.has_value()) {
    return GenerateCondition(*node.condition_);
  }

  if (node.type_ == FilterNode::Type::Logical) {
    std::vector<SqlFragment> children;
    children.reserve(node.children_.size());
    for (const auto& child : node.children_) {
      auto frag = CompileNode(child);
      if (!frag.empty()) {
        children.push_back(std::move(frag));
      }
    }
    if (children.empty()) {
      return {};
    }
    if (node.op_ == FilterOp::NOT) {
      if (children.size() == 1) {
        return expr::not_(std::move(children.front()));
      }
      return expr::not_(expr::and_(children));
    }
    if (node.op_ == FilterOp::OR) {
      return expr::or_(children);
    }
    return expr::and_(children);
  }

  if (node.type_ == FilterNode::Type::StatsBucket && node.stats_bucket_.has_value()) {
    return stats_dimension::BucketPredicate(*node.stats_bucket_,
                                            stats_dimension::ColumnSource::kLiveTables);
  }

  if (node.type_ == FilterNode::Type::LibrarySearch && node.library_search_.has_value()) {
    return BuildLibrarySearchPredicate(node.library_search_->request,
                                       node.library_search_->capabilities);
  }

  return {};
}

auto FilterSQLCompiler::Compile(const FilterNode& node) -> SqlFragment {
  return CompileNode(node);
}

auto CompileFilterPredicate(const std::optional<FilterNode>& node)
    -> std::optional<duckorm::SqlFragment> {
  if (!node.has_value()) {
    return std::nullopt;
  }
  auto fragment = FilterSQLCompiler::Compile(*node);
  if (fragment.empty()) {
    return std::nullopt;
  }
  return fragment;
}

}  // namespace alcedo
