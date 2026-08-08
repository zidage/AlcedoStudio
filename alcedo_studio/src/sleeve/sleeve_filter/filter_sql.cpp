//  Copyright 2025-2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "sleeve/sleeve_filter/filter_combo.hpp"

#include <ctime>
#include <string>
#include <utility>
#include <vector>

#include "storage/mapper/duckorm/duckdb_expr.hpp"
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

}  // namespace

/**
 * @brief Map a domain field onto a scoped album-query column expression.
 *
 * Alias rule (must match ElementController::BuildScopedFileQuery):
 * - `i` is the Image row (`i.metadata`, `i.file_name`, `i.image_path`)
 * - `e` is the Element row (`e.element_name`, `e.added_time`)
 */
auto FilterSQLCompiler::FieldToColumn(FilterField field) -> SqlFragment {
  switch (field) {
    case FilterField::ExifCameraModel:
      return expr::col("json_extract(i.metadata, '$.Model')");
    case FilterField::ExifFocalLength:
      return expr::col("json_extract(i.metadata, '$.FocalLength')::DOUBLE");
    case FilterField::ExifAperture:
      return expr::col("json_extract(i.metadata, '$.Aperture')::DOUBLE");
    case FilterField::ExifISO:
      return expr::col("json_extract(i.metadata, '$.ISO')::INT");
    case FilterField::CaptureDate:
      return expr::col("json_extract(i.metadata, '$.DateTimeString')::TIMESTAMP");
    case FilterField::ImportDate:
      return expr::col("e.added_time");
    case FilterField::FileName:
      return expr::col("e.element_name");
    case FilterField::FileExtension:
      return expr::col("UPPER(i.file_name)");
    case FilterField::ImageSize:
      return expr::col("json_extract(i.metadata, '$.ImageSize')");
    case FilterField::Rating:
      return expr::col("json_extract(i.metadata, '$.Rating')");
    case FilterField::ImagePath:
      return expr::col("i.image_path");
    case FilterField::CameraModelLabel:
      return expr::col("COALESCE(NULLIF(json_extract_string(i.metadata, '$.Model'), ''), "
                       "'(unknown)')");
    case FilterField::LensLabel:
      return expr::col("COALESCE(NULLIF(json_extract_string(i.metadata, '$.Lens'), ''), "
                       "'(unknown)')");
    case FilterField::CaptureDateLabel:
      // TRY_CAST: plain CAST would fail the whole query on rows whose date
      // string is not parseable (for example an empty string).
      return expr::col(
          "TRY_CAST(json_extract(i.metadata, '$.DateTimeString') AS DATE)::VARCHAR");
    case FilterField::RatingLabel:
      return expr::col("json_extract(i.metadata, '$.Rating')::INT");
    case FilterField::SemanticTags:
      // Domain semantic-label filters use EXISTS factories (Phase 2). Placeholder only.
      return expr::col("i.embedding");
  }
  return expr::raw("1=0");
}

auto FilterSQLCompiler::ValueToFragment(const FilterValue& value) -> SqlFragment {
  if (std::holds_alternative<std::monostate>(value)) {
    return expr::lit_null();
  }
  if (std::holds_alternative<int64_t>(value)) {
    return expr::lit(std::get<int64_t>(value));
  }
  if (std::holds_alternative<double>(value)) {
    return expr::lit(std::get<double>(value));
  }
  if (std::holds_alternative<bool>(value)) {
    return expr::lit(std::get<bool>(value));
  }
  if (std::holds_alternative<std::wstring>(value)) {
    return expr::lit(conv::ToBytes(std::get<std::wstring>(value)));
  }
  if (std::holds_alternative<std::tm>(value)) {
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
    case CompareOp::CONTAINS: {
      const auto& text = std::get<std::wstring>(cond.value_);
      return expr::like(std::move(column), expr::lit("%" + conv::ToBytes(text) + "%"));
    }
    case CompareOp::NOT_CONTAINS: {
      const auto& text = std::get<std::wstring>(cond.value_);
      return expr::not_like(std::move(column), expr::lit("%" + conv::ToBytes(text) + "%"));
    }
    case CompareOp::STARTS_WITH: {
      const auto& text = std::get<std::wstring>(cond.value_);
      return expr::like(std::move(column), expr::lit(conv::ToBytes(text) + "%"));
    }
    case CompareOp::ENDS_WITH: {
      const auto& text = std::get<std::wstring>(cond.value_);
      return expr::like(std::move(column), expr::lit("%" + conv::ToBytes(text)));
    }
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
      return expr::raw("(" + column.sql_ + " REGEXP " + expr::lit(conv::ToBytes(text)).sql_ + ")");
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

  if (node.type_ == FilterNode::Type::RawSQL && node.raw_sql_.has_value()) {
    // Bridge only: trusted compiled text or temporary product RawSQL.
    return expr::raw(conv::ToBytes(*node.raw_sql_));
  }

  return {};
}

auto FilterSQLCompiler::Compile(const FilterNode& node) -> SqlFragment {
  return CompileNode(node);
}

auto MergeFilterNodes(const std::optional<FilterNode>& left,
                      const std::optional<FilterNode>& right) -> std::optional<FilterNode> {
  if (!left.has_value()) {
    return right;
  }
  if (!right.has_value()) {
    return left;
  }
  return FilterNode{FilterNode::Type::Logical, FilterOp::AND, {*left, *right}, std::nullopt,
                    std::nullopt};
}

auto CompileFilterWhere(const std::optional<FilterNode>& node)
    -> std::optional<std::wstring> {
  if (!node.has_value()) {
    return std::nullopt;
  }
  const auto fragment = FilterSQLCompiler::Compile(*node);
  if (fragment.empty()) {
    return std::nullopt;
  }
  return conv::FromBytes(fragment.sql_);
}

}  // namespace alcedo
