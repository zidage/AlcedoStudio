//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "semantic_label_assignment.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <format>
#include <iomanip>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <variant>

#include "semantic_score_elbow.hpp"
#include "semantic_tables.hpp"
#include "storage/mapper/duckorm/duckdb_expr.hpp"
#include "storage/mapper/duckorm/duckdb_orm.hpp"

namespace alcedo::semantic_label_assignment {
namespace {
namespace expr = duckorm::expr;
using semantic_tables::Column;
using semantic_tables::ImageEmbeddingTable;
using semantic_tables::LabelPrototypeTable;
using semantic_tables::SetError;

using RankedLabels = std::vector<std::pair<std::string, double>>;

auto JsonString(const std::string& value) -> std::string {
  std::string out;
  out.reserve(value.size() + 2);
  out.push_back('"');
  for (const char ch : value) {
    switch (ch) {
      case '\\':
        out += "\\\\";
        break;
      case '"':
        out += "\\\"";
        break;
      default:
        out.push_back(ch);
        break;
    }
  }
  out.push_back('"');
  return out;
}

// Decides how many label candidates to keep for one image from its ranked (label, score)
// list, which is sorted by score descending. Mirrors the small-N short-circuit of the
// elbow cutoff (keep everything when there are at most two candidates), then keeps every
// candidate at or above the elbow cutoff, clamped to ``ceiling`` (the display cap).
auto ElbowLabelKeepCount(const RankedLabels& scores, size_t ceiling) -> size_t {
  if (scores.empty()) {
    return 0;
  }
  const size_t effective_ceiling = std::max<size_t>(1, ceiling);
  if (scores.size() <= 2) {
    return std::min(scores.size(), effective_ceiling);
  }

  std::vector<double> score_values;
  score_values.reserve(scores.size());
  for (const auto& [label, score] : scores) {
    (void)label;
    score_values.push_back(score);
  }
  const double cutoff = semantic_score_elbow::CutoffScore(score_values);

  size_t       keep   = 0;
  for (const auto& [label, score] : scores) {
    (void)label;
    if (score >= cutoff) {
      ++keep;
    } else {
      break;  // scores are sorted descending; nothing past the first miss can qualify
    }
  }
  return std::clamp(keep, size_t{1}, effective_ceiling);
}

auto MakeTopScoresJson(const RankedLabels& scores, size_t limit) -> std::string {
  std::ostringstream out;
  out << "[";
  const auto count = std::min({limit, kMaxSemanticImageLabelCount, scores.size()});
  for (size_t i = 0; i < count; ++i) {
    if (i > 0) {
      out << ",";
    }
    out << "{\"label\":" << JsonString(scores[i].first) << ",\"score\":" << std::setprecision(9)
        << scores[i].second << "}";
  }
  out << "]";
  return out.str();
}

auto BuildLabel(sl_element_id_t file_id, const std::string& model_key,
                const SemanticLabelAssignmentOptions& options, const RankedLabels& scores)
    -> SemanticImageLabelRecord {
  // The elbow decides how many of the ranked candidates are genuinely relevant for this
  // image; the rest are noise and are dropped before they reach the stored JSON. The
  // display ceiling (top_score_count_, capped at kMaxSemanticImageLabelCount) bounds k.
  const size_t ceiling = std::min(options.top_score_count_, kMaxSemanticImageLabelCount);
  const size_t keep    = ElbowLabelKeepCount(scores, ceiling);
  // margin_ always measures the gap to the true nearest competitor (scores[1]), independent
  // of how many labels the elbow kept, so confident_ still reflects whether top-1 dominates.
  const double second_score = scores.size() > 1 ? scores[1].second : 0.0;

  SemanticImageLabelRecord label;
  label.file_id_   = file_id;
  label.model_key_ = model_key;
  label.label_     = scores[0].first;
  label.score_     = scores[0].second;
  if (keep >= 2 && scores.size() > 1) {
    label.second_label_ = scores[1].first;
    label.second_score_ = std::optional<double>(scores[1].second);
  } else {
    label.second_label_ = std::string{};
    label.second_score_ = std::nullopt;
  }
  label.margin_          = scores[0].second - second_score;
  label.confident_       = label.score_ >= options.confidence_score_threshold_ &&
                     label.margin_ >= options.confidence_margin_threshold_;
  label.top_scores_json_ = MakeTopScoresJson(scores, keep);
  return label;
}
}  // namespace

void ValidateOptions(const SemanticLabelAssignmentOptions& options) {
  if (options.prompt_config_hash_.empty()) {
    throw std::runtime_error("Semantic label assignment prompt config hash is empty.");
  }
  if (!std::isfinite(options.confidence_score_threshold_) ||
      !std::isfinite(options.confidence_margin_threshold_)) {
    throw std::runtime_error("Semantic label assignment threshold contains NaN or infinity.");
  }
}

auto ValidateLabel(const SemanticImageEmbeddingRecord& record,
                   const SemanticImageLabelRecord* label, std::string* error) -> bool {
  if (!label) {
    return true;
  }
  if (label->file_id_ != record.file_id_) {
    SetError(error, "Semantic label file id does not match embedding file id.");
    return false;
  }
  if (label->model_key_ != record.model_key_) {
    SetError(error, "Semantic label model key does not match embedding model key.");
    return false;
  }
  if (label->label_.empty()) {
    SetError(error, "Semantic label is empty.");
    return false;
  }
  if (!std::isfinite(label->score_) ||
      (label->second_score_.has_value() && !std::isfinite(*label->second_score_)) ||
      !std::isfinite(label->margin_)) {
    SetError(error, "Semantic label score contains NaN or infinity.");
    return false;
  }
  return true;
}

void InsertLabel(duckdb_connection conn, const SemanticImageLabelRecord& label) {
  static constexpr std::array<duckorm::DuckFieldDesc, 9> fields = {
      FIELD_AS(SemanticImageLabelRecord, file_id_, "file_id", UINT32),
      FIELD_AS(SemanticImageLabelRecord, model_key_, "model_key", STRING),
      FIELD_AS(SemanticImageLabelRecord, label_, "label", STRING),
      FIELD_AS(SemanticImageLabelRecord, score_, "score", DOUBLE),
      FIELD_AS(SemanticImageLabelRecord, second_label_, "second_label", NULLABLE_STRING),
      FIELD_AS(SemanticImageLabelRecord, second_score_, "second_score", NULLABLE_DOUBLE),
      FIELD_AS(SemanticImageLabelRecord, margin_, "margin", DOUBLE),
      FIELD_AS(SemanticImageLabelRecord, confident_, "confident", BOOLEAN),
      FIELD_AS(SemanticImageLabelRecord, top_scores_json_, "top_scores", NULLABLE_STRING)};
  duckorm::insert_by_query(
      conn,
      "INSERT INTO SemanticImageLabel "
      "(file_id, model_key, label, score, second_label, second_score, margin, confident, "
      "top_scores) "
      "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?);",
      &label, fields, fields.size());
}

void AppendLabels(duckdb_connection conn, std::span<const SemanticImageLabelRecord> labels) {
  static constexpr std::array<const char*, 9> kColumns = {
      "file_id",      "model_key", "label",     "score",     "second_label",
      "second_score", "margin",    "confident", "top_scores"};
  duckorm::Appender appender(conn, "SemanticImageLabel", kColumns);
  const auto        append_nullable_string = [&appender](const std::string& value) {
    if (value.empty()) {
      appender.append_null();
    } else {
      appender.append_varchar(value);
    }
  };
  for (const auto& label : labels) {
    appender.append_uint32(static_cast<uint32_t>(label.file_id_));
    appender.append_varchar(label.model_key_);
    appender.append_varchar(label.label_);
    appender.append_double(label.score_);
    append_nullable_string(label.second_label_);
    if (label.second_score_.has_value()) {
      appender.append_double(*label.second_score_);
    } else {
      appender.append_null();
    }
    appender.append_double(label.margin_);
    appender.append_bool(label.confident_);
    append_nullable_string(label.top_scores_json_);
    appender.end_row();
  }
  appender.flush();
}

auto AssignLabel(duckdb_connection conn, const SemanticImageEmbeddingRecord& record, int model_dim,
                 const SemanticLabelAssignmentOptions& options) -> SemanticImageLabelRecord {
  ValidateOptions(options);

  // Rank enough prototypes for the elbow to find a knee; the display cap is applied
  // later in BuildLabel, not here.
  auto query = expr::raw(std::format(
      "SELECT lp.label, array_inner_product(lp.embedding, se.embedding) AS score "
      "FROM {} lp "
      "JOIN {} se ON se.model_key = lp.model_key AND se.file_id = ",
      LabelPrototypeTable(model_dim), ImageEmbeddingTable(model_dim)));
  query.append(expr::param(static_cast<int64_t>(record.file_id_)));
  query.append(expr::raw(" AND se.image_id = "));
  query.append(expr::param(static_cast<int64_t>(record.image_id_)));
  query.append(expr::raw(" WHERE lp.model_key = "));
  query.append(expr::param(record.model_key_));
  query.append(expr::raw(" AND lp.prompt_config_hash = "));
  query.append(expr::param(options.prompt_config_hash_));
  query.append(expr::raw(std::format(" AND se.status = 'ready' AND se.error IS NULL "
                                     "ORDER BY score DESC, lp.label LIMIT {};",
                                     kSemanticLabelCandidatePoolSize)));

  static const std::array<duckorm::DuckFieldDesc, 2> fields = {
      Column("label", duckorm::DuckDBType::VARCHAR), Column("score", duckorm::DuckDBType::DOUBLE)};
  const auto rows = duckorm::select_by_query(conn, fields, fields.size(), query);

  RankedLabels scores;
  scores.reserve(rows.size());
  for (const auto& row : rows) {
    auto label = duckorm::cell_text(row[0]);
    if (label.empty()) {
      throw std::runtime_error("Semantic label assignment returned an empty label.");
    }
    scores.emplace_back(std::move(label), std::get<double>(row[1]));
  }
  if (scores.empty()) {
    throw std::runtime_error("Semantic label prototype cache is empty.");
  }
  return BuildLabel(record.file_id_, record.model_key_, options, scores);
}

auto AssignLabels(duckdb_connection conn, std::span<const SemanticImageEmbeddingRecord> records,
                  std::span<const sl_element_id_t> file_ids, const std::string& model_key,
                  int model_dim, const SemanticLabelAssignmentOptions& options)
    -> std::vector<SemanticImageLabelRecord> {
  std::vector<SemanticImageLabelRecord>       out_labels(records.size());
  std::unordered_map<sl_element_id_t, size_t> index_by_file;
  index_by_file.reserve(records.size() * 2);
  for (size_t i = 0; i < records.size(); ++i) {
    index_by_file.emplace(records[i].file_id_, i);
  }

  // Ranks every prototype against each embedding and keeps the top-N per file with a window
  // function. Rank enough prototypes for the elbow to find a knee; the display cap is applied
  // later in BuildLabel, not here.
  auto query = expr::raw(std::format(
      "WITH scored AS ("
      "SELECT se.file_id AS file_id, lp.label AS label, "
      "array_inner_product(lp.embedding, se.embedding) AS score "
      "FROM {} se "
      "JOIN {} lp ON lp.model_key = se.model_key AND lp.prompt_config_hash = ",
      ImageEmbeddingTable(model_dim), LabelPrototypeTable(model_dim)));
  query.append(expr::param(options.prompt_config_hash_));
  query.append(expr::raw(" WHERE se.model_key = "));
  query.append(expr::param(model_key));
  query.append(expr::raw(" AND se.status = 'ready' AND se.error IS NULL AND "));
  query.append(expr::in_list(expr::col("se.file_id"), file_ids));
  query.append(expr::raw(std::format(
      ") SELECT file_id, label, score FROM ("
      "SELECT file_id, label, score, "
      "ROW_NUMBER() OVER (PARTITION BY file_id ORDER BY score DESC, label) AS rn "
      "FROM scored) ranked WHERE rn <= {} ORDER BY file_id, rn;",
      kSemanticLabelCandidatePoolSize)));

  static const std::array<duckorm::DuckFieldDesc, 3> fields = {
      Column("file_id", duckorm::DuckDBType::INT64), Column("label", duckorm::DuckDBType::VARCHAR),
      Column("score", duckorm::DuckDBType::DOUBLE)};
  const auto rows = duckorm::select_by_query(conn, fields, fields.size(), query);

  // Rows are ordered by file, so the ranked scores of a file are one run of rows.
  RankedLabels scores;
  for (size_t r = 0; r < rows.size(); ++r) {
    const auto file_id = static_cast<sl_element_id_t>(std::get<int64_t>(rows[r][0]));
    auto       label   = duckorm::cell_text(rows[r][1]);
    if (label.empty()) {
      throw std::runtime_error(
          "Semantic label assignment returned incomplete results for the batch.");
    }
    scores.emplace_back(std::move(label), std::get<double>(rows[r][2]));
    const bool last_row_of_file =
        r + 1 == rows.size() ||
        static_cast<sl_element_id_t>(std::get<int64_t>(rows[r + 1][0])) != file_id;
    if (!last_row_of_file) {
      continue;
    }
    const auto it = index_by_file.find(file_id);
    if (it == index_by_file.end()) {
      throw std::runtime_error(
          "Semantic label assignment returned incomplete results for the batch.");
    }
    out_labels[it->second] = BuildLabel(file_id, model_key, options, scores);
    scores.clear();
  }

  for (const auto& label : out_labels) {
    if (label.label_.empty()) {
      throw std::runtime_error(
          "Semantic label assignment produced no label for a file in the batch.");
    }
  }
  return out_labels;
}

}  // namespace alcedo::semantic_label_assignment
