//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "storage/store/semantic/semantic_store.hpp"

#include <duckdb.h>

#include <QStringList>
#include <algorithm>
#include <array>
#include <cmath>
#include <exception>
#include <format>
#include <iomanip>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

#include "storage/mapper/duckorm/duckdb_expr.hpp"
#include "storage/mapper/duckorm/duckdb_orm.hpp"
#include "storage/store/duckdb_extension.hpp"
#include "storage/store/sleeve/element_store.hpp"
#include "utils/diagnostics/app_logging.hpp"

namespace alcedo {
namespace {
namespace expr = duckorm::expr;

constexpr size_t kSemanticSearchMinCandidatePool     = 512;
constexpr size_t kSemanticSearchCutoffSampleLimit    = 256;
constexpr double kSemanticFlatScoreSpanRatio         = 0.08;
constexpr double kSemanticFallbackScoreSpanKeepRatio = 0.35;
constexpr double kSemanticElbowGapToSpanRatio        = 0.18;
constexpr double kSemanticElbowGapToMedianRatio      = 3.0;

// ---- result cells (duckorm select_by_query rows, see duckorm::VarTypes) ----

auto CellString(const duckorm::VarTypes& value) -> std::string {
  const auto& text = std::get<std::unique_ptr<std::string>>(value);
  return text ? *text : std::string{};
}

// BOOLEAN cells are decoded as the text "true" / "false".
auto CellBool(const duckorm::VarTypes& value) -> bool { return CellString(value) == "true"; }

auto Column(const char* name, duckorm::DuckDBType type) -> duckorm::DuckFieldDesc {
  return duckorm::DuckFieldDesc{name, type, 0};
}

// SemanticModel columns read by GetModel and ListModels, in MapModelRow order.
constexpr const char* kModelColumns =
    "model_key, model_id, revision, embedding_dim, image_size, engine_id, profile_id, "
    "supported_text_languages_json, prompt_config_hash, asset_manifest_json, active";
const std::array<duckorm::DuckFieldDesc, 11> kModelRowFields = {
    Column("model_key", duckorm::DuckDBType::VARCHAR),
    Column("model_id", duckorm::DuckDBType::VARCHAR),
    Column("revision", duckorm::DuckDBType::VARCHAR),
    Column("embedding_dim", duckorm::DuckDBType::INT32),
    Column("image_size", duckorm::DuckDBType::INT32),
    Column("engine_id", duckorm::DuckDBType::NULLABLE_STRING),
    Column("profile_id", duckorm::DuckDBType::NULLABLE_STRING),
    Column("supported_text_languages_json", duckorm::DuckDBType::NULLABLE_STRING),
    Column("prompt_config_hash", duckorm::DuckDBType::NULLABLE_STRING),
    Column("asset_manifest_json", duckorm::DuckDBType::NULLABLE_STRING),
    Column("active", duckorm::DuckDBType::BOOLEAN)};

auto MapModelRow(const std::vector<duckorm::VarTypes>& row) -> SemanticModelRecord {
  SemanticModelRecord record;
  record.model_key_                     = CellString(row[0]);
  record.model_id_                      = CellString(row[1]);
  record.revision_                      = CellString(row[2]);
  record.embedding_dim_                 = std::get<int32_t>(row[3]);
  record.image_size_                    = std::get<int32_t>(row[4]);
  record.engine_id_                     = CellString(row[5]);
  record.profile_id_                    = CellString(row[6]);
  record.supported_text_languages_json_ = CellString(row[7]);
  record.prompt_config_hash_            = CellString(row[8]);
  record.asset_manifest_json_           = CellString(row[9]);
  record.active_                        = CellBool(row[10]);
  return record;
}

auto SummarizeEmbeddingRecords(std::span<const SemanticImageEmbeddingRecord> records,
                               size_t max_items = 16) -> QString {
  QStringList  parts;
  const size_t count = std::min(records.size(), max_items);
  for (size_t i = 0; i < count; ++i) {
    parts << QStringLiteral("%1:%2")
                 .arg(static_cast<qulonglong>(records[i].file_id_))
                 .arg(static_cast<qulonglong>(records[i].image_id_));
  }
  if (records.size() > max_items) {
    parts << QStringLiteral("...");
  }
  return parts.join(QLatin1Char(','));
}

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

auto RequestedCandidateLimit(size_t offset, size_t limit) -> size_t {
  const auto max_size      = std::numeric_limits<size_t>::max();
  const auto requested_end = offset > max_size - limit ? max_size : offset + limit;
  return std::max(requested_end, kSemanticSearchMinCandidatePool);
}

// Elbow/knee cutoff over a pre-sorted descending score sequence. Score-only so it
// can be shared by semantic search (over many files) and label assignment (over a
// handful of prototypes). Returns a cutoff score: every item at or above it is kept.
auto ElbowScoreCutoffCore(std::span<const double> scores_desc) -> double {
  if (scores_desc.empty()) {
    return 0.0;
  }
  if (scores_desc.size() <= 2) {
    return scores_desc.back();
  }

  const auto   sample_count = std::min(scores_desc.size(), kSemanticSearchCutoffSampleLimit);
  const double top_score    = scores_desc.front();
  const double tail_score   = scores_desc[sample_count - 1];
  const double score_span   = top_score - tail_score;
  if (score_span <= std::abs(top_score) * kSemanticFlatScoreSpanRatio) {
    return tail_score;
  }

  std::vector<double> gaps;
  gaps.reserve(sample_count - 1);
  double best_gap   = 0.0;
  size_t best_index = 0;
  for (size_t i = 0; i + 1 < sample_count; ++i) {
    const double gap = scores_desc[i] - scores_desc[i + 1];
    gaps.push_back(gap);
    if (gap > best_gap) {
      best_gap   = gap;
      best_index = i;
    }
  }

  auto sorted_gaps = gaps;
  std::sort(sorted_gaps.begin(), sorted_gaps.end());
  const double median_gap      = sorted_gaps[sorted_gaps.size() / 2];
  const bool   has_clear_elbow = best_gap >= score_span * kSemanticElbowGapToSpanRatio &&
                               best_gap >= median_gap * kSemanticElbowGapToMedianRatio;
  if (has_clear_elbow) {
    return (scores_desc[best_index] + scores_desc[best_index + 1]) / 2.0;
  }

  return top_score - (score_span * kSemanticFallbackScoreSpanKeepRatio);
}

auto SemanticSearchScoreCutoff(std::span<const SemanticRankedFile> ranked) -> double {
  std::vector<double> scores;
  scores.reserve(ranked.size());
  for (const auto& row : ranked) {
    scores.push_back(row.score_);
  }
  return ElbowScoreCutoffCore(scores);
}

// Decides how many label candidates to keep for one image from its ranked (label, score)
// list, which is sorted by score descending. Mirrors the small-N short-circuit of the
// elbow core (keep everything when there are at most two candidates), then keeps every
// candidate at or above the elbow cutoff, clamped to ``ceiling`` (the display cap).
auto ElbowLabelKeepCount(const std::vector<std::pair<std::string, double>>& scores, size_t ceiling)
    -> size_t {
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
  const double cutoff = ElbowScoreCutoffCore(score_values);

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

auto FilterAndPageSemanticCandidates(std::vector<SemanticRankedFile> candidates, size_t offset,
                                     size_t limit) -> std::vector<SemanticRankedFile> {
  const double cutoff = SemanticSearchScoreCutoff(candidates);
  candidates.erase(
      std::remove_if(candidates.begin(), candidates.end(),
                     [cutoff](const SemanticRankedFile& row) { return row.score_ < cutoff; }),
      candidates.end());
  if (offset >= candidates.size()) {
    return {};
  }
  const auto first          = candidates.begin() + static_cast<std::ptrdiff_t>(offset);
  const auto remaining_rows = static_cast<size_t>(std::distance(first, candidates.end()));
  const auto count          = std::min(limit, remaining_rows);
  return {first, first + static_cast<std::ptrdiff_t>(count)};
}

void SetError(std::string* error, const std::string& message) {
  if (error) {
    *error = message;
  }
}

auto IsSupportedSemanticEmbeddingDim(int dim) -> bool {
  return dim == kSemanticEmbeddingDim || dim == kSemanticEmbeddingDim768;
}

auto ImageEmbeddingTableName(int dim) -> const char* {
  return dim == kSemanticEmbeddingDim768 ? "SemanticImageEmbedding768" : "SemanticImageEmbedding";
}

auto LabelPrototypeTableName(int dim) -> const char* {
  return dim == kSemanticEmbeddingDim768 ? "SemanticLabelPrototype768" : "SemanticLabelPrototype";
}

auto ImageEmbeddingIndexName(int dim) -> const char* {
  return dim == kSemanticEmbeddingDim768 ? "idx_semantic_image_embedding768_hnsw"
                                         : "idx_semantic_image_embedding_hnsw";
}

// ---- predicates ----

auto ColumnEquals(const char* column, const std::string& value) -> duckorm::SqlFragment {
  return expr::eq(expr::col(column), expr::param(value));
}

auto ColumnEquals(const char* column, int64_t value) -> duckorm::SqlFragment {
  return expr::eq(expr::col(column), expr::param(value));
}

// ---- statements (all throw std::runtime_error with DuckDB's message on failure) ----

// Embedding size of a registered model, or std::nullopt when the model is not registered.
auto ModelEmbeddingDim(duckdb_connection conn, const std::string& model_key) -> std::optional<int> {
  auto query = expr::raw("SELECT embedding_dim FROM SemanticModel WHERE ");
  query.append(ColumnEquals("model_key", model_key));
  const auto value = duckorm::select_int64(conn, query);
  if (!value.has_value()) {
    return std::nullopt;
  }
  return static_cast<int>(*value);
}

// Count of a COUNT(*) query. A failed query reads as 0: the store's count methods report no
// error.
auto CountOrZero(duckdb_connection conn, const duckorm::SqlFragment& query) -> size_t {
  try {
    const auto count = duckorm::select_int64(conn, query);
    return count.has_value() ? static_cast<size_t>(*count) : 0U;
  } catch (const std::exception&) {
    return 0U;
  }
}

void InsertReadyImageEmbedding(duckdb_connection conn, const SemanticImageEmbeddingRecord& record,
                               int model_dim) {
  static constexpr std::array<duckorm::DuckFieldDesc, 5> fields = {
      FIELD_AS(SemanticImageEmbeddingRecord, file_id_, "file_id", UINT32),
      FIELD_AS(SemanticImageEmbeddingRecord, image_id_, "image_id", UINT32),
      FIELD_AS(SemanticImageEmbeddingRecord, model_key_, "model_key", STRING),
      FIELD_AS(SemanticImageEmbeddingRecord, embedding_, "embedding", FLOAT_ARRAY),
      FIELD_AS(SemanticImageEmbeddingRecord, thumbnail_resolution_, "thumbnail_resolution", INT32)};
  duckorm::insert_by_query(conn,
                           std::format("INSERT INTO {} "
                                       "(file_id, image_id, model_key, embedding, embedding_dim, "
                                       "thumbnail_resolution, status, error) "
                                       "VALUES (?, ?, ?, ?, {}, ?, 'ready', NULL);",
                                       ImageEmbeddingTableName(model_dim), model_dim),
                           &record, fields, fields.size());
}

void InsertOrReplaceLabelPrototype(duckdb_connection                   conn,
                                   const SemanticLabelPrototypeRecord& record, int model_dim) {
  static constexpr std::array<duckorm::DuckFieldDesc, 4> fields = {
      FIELD_AS(SemanticLabelPrototypeRecord, model_key_, "model_key", STRING),
      FIELD_AS(SemanticLabelPrototypeRecord, label_, "label", STRING),
      FIELD_AS(SemanticLabelPrototypeRecord, prompt_config_hash_, "prompt_config_hash", STRING),
      FIELD_AS(SemanticLabelPrototypeRecord, embedding_, "embedding", FLOAT_ARRAY)};
  duckorm::insert_or_replace(conn, LabelPrototypeTableName(model_dim), &record, fields,
                             fields.size());
}

auto ValidateEmbedding(std::span<const float> embedding, int expected_dim, std::string* error)
    -> bool {
  if (!IsSupportedSemanticEmbeddingDim(expected_dim)) {
    SetError(error, std::format("Semantic storage does not support {}-dimensional embeddings.",
                                expected_dim));
    return false;
  }
  if (embedding.size() != static_cast<size_t>(expected_dim)) {
    SetError(error, std::format("Embedding dimension mismatch: expected {}, got {}.", expected_dim,
                                embedding.size()));
    return false;
  }

  double norm_sq = 0.0;
  for (const float value : embedding) {
    if (!std::isfinite(value)) {
      SetError(error, "Embedding contains NaN or infinity.");
      return false;
    }
    norm_sq += static_cast<double>(value) * static_cast<double>(value);
  }
  if (norm_sq <= 0.0) {
    SetError(error, "Embedding norm is zero.");
    return false;
  }
  return true;
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

auto MakeTopScoresJson(const std::vector<std::pair<std::string, double>>& scores, size_t limit)
    -> std::string {
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

void InsertSemanticLabel(duckdb_connection conn, const SemanticImageLabelRecord& label) {
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

void UpsertSemanticModel(duckdb_connection conn, const SemanticModelRecord& model) {
  static constexpr std::array<duckorm::DuckFieldDesc, 11> fields = {
      FIELD_AS(SemanticModelRecord, model_key_, "model_key", STRING),
      FIELD_AS(SemanticModelRecord, model_id_, "model_id", STRING),
      FIELD_AS(SemanticModelRecord, revision_, "revision", STRING),
      FIELD_AS(SemanticModelRecord, embedding_dim_, "embedding_dim", INT32),
      FIELD_AS(SemanticModelRecord, image_size_, "image_size", INT32),
      FIELD_AS(SemanticModelRecord, engine_id_, "engine_id", NULLABLE_STRING),
      FIELD_AS(SemanticModelRecord, profile_id_, "profile_id", NULLABLE_STRING),
      FIELD_AS(SemanticModelRecord, supported_text_languages_json_, "supported_text_languages_json",
               NULLABLE_STRING),
      FIELD_AS(SemanticModelRecord, prompt_config_hash_, "prompt_config_hash", NULLABLE_STRING),
      FIELD_AS(SemanticModelRecord, asset_manifest_json_, "asset_manifest_json", NULLABLE_STRING),
      FIELD_AS(SemanticModelRecord, active_, "active", BOOLEAN)};
  duckorm::insert_or_replace(conn, "SemanticModel", &model, fields, fields.size());
}

auto BuildAssignedLabel(sl_element_id_t file_id, const std::string& model_key,
                        const SemanticLabelAssignmentOptions&              assignment_options,
                        const std::vector<std::pair<std::string, double>>& scores)
    -> SemanticImageLabelRecord {
  // The elbow decides how many of the ranked candidates are genuinely relevant for this
  // image; the rest are noise and are dropped before they reach the stored JSON. The
  // display ceiling (top_score_count_, capped at kMaxSemanticImageLabelCount) bounds k.
  const size_t ceiling = std::min(assignment_options.top_score_count_, kMaxSemanticImageLabelCount);
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
  label.margin_    = scores[0].second - second_score;
  label.confident_ = label.score_ >= assignment_options.confidence_score_threshold_ &&
                     label.margin_ >= assignment_options.confidence_margin_threshold_;
  label.top_scores_json_ = MakeTopScoresJson(scores, keep);
  return label;
}

void ValidateAssignmentOptions(const SemanticLabelAssignmentOptions& assignment_options) {
  if (assignment_options.prompt_config_hash_.empty()) {
    throw std::runtime_error("Semantic label assignment prompt config hash is empty.");
  }
  if (!std::isfinite(assignment_options.confidence_score_threshold_) ||
      !std::isfinite(assignment_options.confidence_margin_threshold_)) {
    throw std::runtime_error("Semantic label assignment threshold contains NaN or infinity.");
  }
}

// Ranks the label prototypes against the embedding just written for @p record and builds its
// label. Throws when the options are invalid, a label is empty, or no prototype exists.
auto QueryAssignedLabel(duckdb_connection conn, const SemanticImageEmbeddingRecord& record,
                        int model_dim, const SemanticLabelAssignmentOptions& assignment_options)
    -> SemanticImageLabelRecord {
  ValidateAssignmentOptions(assignment_options);

  // Rank enough prototypes for the elbow to find a knee; the display cap is applied
  // later in BuildAssignedLabel, not here.
  auto query = expr::raw(std::format(
      "SELECT lp.label, array_inner_product(lp.embedding, se.embedding) AS score "
      "FROM {} lp "
      "JOIN {} se ON se.model_key = lp.model_key AND se.file_id = ",
      LabelPrototypeTableName(model_dim), ImageEmbeddingTableName(model_dim)));
  query.append(expr::param(static_cast<int64_t>(record.file_id_)));
  query.append(expr::raw(" AND se.image_id = "));
  query.append(expr::param(static_cast<int64_t>(record.image_id_)));
  query.append(expr::raw(" WHERE lp.model_key = "));
  query.append(expr::param(record.model_key_));
  query.append(expr::raw(" AND lp.prompt_config_hash = "));
  query.append(expr::param(assignment_options.prompt_config_hash_));
  query.append(expr::raw(std::format(" AND se.status = 'ready' AND se.error IS NULL "
                                     "ORDER BY score DESC, lp.label LIMIT {};",
                                     kSemanticLabelCandidatePoolSize)));

  static const std::array<duckorm::DuckFieldDesc, 2> fields = {
      Column("label", duckorm::DuckDBType::VARCHAR), Column("score", duckorm::DuckDBType::DOUBLE)};
  const auto rows = duckorm::select_by_query(conn, fields, fields.size(), query);

  std::vector<std::pair<std::string, double>> scores;
  scores.reserve(rows.size());
  for (const auto& row : rows) {
    auto label = CellString(row[0]);
    if (label.empty()) {
      throw std::runtime_error("Semantic label assignment returned an empty label.");
    }
    scores.emplace_back(std::move(label), std::get<double>(row[1]));
  }
  if (scores.empty()) {
    throw std::runtime_error("Semantic label prototype cache is empty.");
  }
  return BuildAssignedLabel(record.file_id_, record.model_key_, assignment_options, scores);
}

// Bulk-inserts ready image embedding rows through the DuckDB Appender within the caller's
// transaction. Only the non-default columns are appended; `generated_at` and `error` fall back
// to their column defaults.
void AppendImageEmbeddingRows(duckdb_connection                             conn,
                              std::span<const SemanticImageEmbeddingRecord> records,
                              int                                           model_dim) {
  static constexpr std::array<const char*, 7> kColumns = {
      "file_id", "image_id", "model_key", "embedding", "embedding_dim", "thumbnail_resolution",
      "status"};
  duckorm::Appender appender(conn, ImageEmbeddingTableName(model_dim), kColumns);
  for (const auto& record : records) {
    if (record.embedding_.size() != static_cast<size_t>(model_dim)) {
      throw std::runtime_error(
          std::format("DuckDB appender failed for {}", ImageEmbeddingTableName(model_dim)));
    }
    appender.append_uint32(static_cast<uint32_t>(record.file_id_));
    appender.append_uint32(static_cast<uint32_t>(record.image_id_));
    appender.append_varchar(record.model_key_);
    appender.append_float_array(record.embedding_);
    appender.append_int32(static_cast<int32_t>(model_dim));
    appender.append_int32(static_cast<int32_t>(record.thumbnail_resolution_));
    appender.append_varchar("ready");
    appender.end_row();
  }
  appender.flush();
}

// Bulk-inserts assigned label rows through the DuckDB Appender within the caller's
// transaction. `updated_at` falls back to its column default; an empty second label or top
// scores text is stored as NULL.
void AppendSemanticLabelRows(duckdb_connection                         conn,
                             std::span<const SemanticImageLabelRecord> labels) {
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

// Assigns labels for an entire batch with a single SQL query. Ranks every prototype
// against each just-inserted embedding and keeps the top-N per file via a window
// function, then builds one SemanticImageLabelRecord per input record, in input order.
// Requires all records to share the same model_key and to have unique file_ids. Throws when
// a file of the batch gets no label.
auto QueryAssignedLabelsBatch(duckdb_connection                             conn,
                              std::span<const SemanticImageEmbeddingRecord> records,
                              std::span<const sl_element_id_t> file_ids, const std::string& model_key,
                              int                                   model_dim,
                              const SemanticLabelAssignmentOptions& assignment_options)
    -> std::vector<SemanticImageLabelRecord> {
  std::vector<SemanticImageLabelRecord>       out_labels(records.size());
  std::unordered_map<sl_element_id_t, size_t> index_by_file;
  index_by_file.reserve(records.size() * 2);
  for (size_t i = 0; i < records.size(); ++i) {
    index_by_file.emplace(records[i].file_id_, i);
  }

  // Rank enough prototypes for the elbow to find a knee; the display cap is applied
  // later in BuildAssignedLabel, not here.
  auto query = expr::raw(std::format(
      "WITH scored AS ("
      "SELECT se.file_id AS file_id, lp.label AS label, "
      "array_inner_product(lp.embedding, se.embedding) AS score "
      "FROM {} se "
      "JOIN {} lp ON lp.model_key = se.model_key AND lp.prompt_config_hash = ",
      ImageEmbeddingTableName(model_dim), LabelPrototypeTableName(model_dim)));
  query.append(expr::param(assignment_options.prompt_config_hash_));
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
  std::vector<std::pair<std::string, double>> scores;
  for (size_t r = 0; r < rows.size(); ++r) {
    const auto file_id = static_cast<sl_element_id_t>(std::get<int64_t>(rows[r][0]));
    auto       label   = CellString(rows[r][1]);
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
    out_labels[it->second] = BuildAssignedLabel(file_id, model_key, assignment_options, scores);
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

// Deletes the embedding rows and the label rows of @p model_key for @p file_ids.
void DeleteEmbeddingAndLabelRows(duckdb_connection conn, int model_dim,
                                 const std::string&               model_key,
                                 std::span<const sl_element_id_t> file_ids) {
  const auto where = expr::and_(
      {ColumnEquals("model_key", model_key), expr::in_list(expr::col("file_id"), file_ids)});
  duckorm::remove(conn, ImageEmbeddingTableName(model_dim), where);
  duckorm::remove(conn, "SemanticImageLabel", where);
}

/// The key of the active semantic model as stored, or std::nullopt when the query fails. An
/// empty string means no model is active.
auto ReadActiveModelKey(duckdb_connection conn) -> std::optional<std::string> {
  try {
    return duckorm::select_string(conn, expr::raw("SELECT model_key FROM SemanticModel WHERE "
                                                  "active = TRUE ORDER BY created_at DESC, "
                                                  "model_key DESC LIMIT 1"))
        .value_or(std::string{});
  } catch (const std::exception&) {
    return std::nullopt;
  }
}
}  // namespace

void DeleteSemanticRowsForFiles(duckdb_connection                conn,
                                std::span<const sl_element_id_t> file_ids) {
  if (file_ids.empty()) {
    return;
  }
  const auto file_in = expr::in_list(expr::col("file_id"), file_ids);
  duckorm::remove(conn, "SemanticImageEmbedding", file_in);
  duckorm::remove(conn, "SemanticImageEmbedding768", file_in);
  duckorm::remove(conn, "SemanticImageLabel", file_in);
}

SemanticStore::SemanticStore(Database& db_ctrl) : database_(db_ctrl) {
  auto guard   = database_.GetConnectionGuard();
  auto db_lock = guard.Lock();
  RefreshActiveModelKey(guard.conn_);
}

void SemanticStore::RefreshActiveModelKey(duckdb_connection conn) const {
  auto key = ReadActiveModelKey(conn);
  if (!key.has_value()) {
    return;
  }
  std::lock_guard lock(active_model_key_mutex_);
  active_model_key_ = std::move(*key);
}

auto SemanticStore::UpsertModel(const SemanticModelRecord& model, std::string* error) const
    -> bool {
  auto guard   = database_.GetConnectionGuard();
  auto db_lock = guard.Lock();
  if (model.model_key_.empty()) {
    SetError(error, "Semantic model key is empty.");
    return false;
  }
  if (model.model_id_.empty()) {
    SetError(error, "Semantic model id is empty.");
    return false;
  }
  if (!IsSupportedSemanticEmbeddingDim(model.embedding_dim_)) {
    SetError(error, std::format("Semantic storage does not support {}-dimensional embeddings.",
                                model.embedding_dim_));
    return false;
  }
  if (model.image_size_ <= 0) {
    SetError(error, "Semantic model image size must be positive.");
    return false;
  }

  try {
    if (model.active_) {
      duckorm::Transaction transaction(guard.conn_);
      duckorm::execute(guard.conn_, expr::raw("UPDATE SemanticModel SET active = FALSE"));
      UpsertSemanticModel(guard.conn_, model);
      transaction.commit();
    } else {
      // Replacing the active model's row with an inactive one leaves no active model.
      UpsertSemanticModel(guard.conn_, model);
    }
  } catch (const std::exception& e) {
    SetError(error, e.what());
    return false;
  }
  RefreshActiveModelKey(guard.conn_);
  return true;
}

auto SemanticStore::HasModel(const std::string& model_key) const -> bool {
  return GetModelEmbeddingDim(model_key).has_value();
}

auto SemanticStore::GetModelEmbeddingDim(const std::string& model_key) const
    -> std::optional<int> {
  auto guard   = database_.GetConnectionGuard();
  auto db_lock = guard.Lock();
  try {
    return ModelEmbeddingDim(guard.conn_, model_key);
  } catch (const std::exception&) {
    return std::nullopt;
  }
}

auto SemanticStore::GetModelSupportedTextLanguagesJson(const std::string& model_key) const
    -> std::string {
  auto guard   = database_.GetConnectionGuard();
  auto db_lock = guard.Lock();
  auto query   = expr::raw("SELECT supported_text_languages_json FROM SemanticModel WHERE ");
  query.append(ColumnEquals("model_key", model_key));
  try {
    return duckorm::select_string(guard.conn_, query).value_or(std::string{});
  } catch (const std::exception&) {
    return {};
  }
}

auto SemanticStore::GetModel(const std::string& model_key, std::string* error) const
    -> std::optional<SemanticModelRecord> {
  auto guard   = database_.GetConnectionGuard();
  auto db_lock = guard.Lock();
  auto query   = expr::raw(std::format("SELECT {} FROM SemanticModel WHERE ", kModelColumns));
  query.append(ColumnEquals("model_key", model_key));
  query.append(expr::raw(" LIMIT 1"));
  try {
    const auto rows =
        duckorm::select_by_query(guard.conn_, kModelRowFields, kModelRowFields.size(), query);
    if (rows.empty()) {
      return std::nullopt;
    }
    return MapModelRow(rows.front());
  } catch (const std::exception& e) {
    SetError(error, e.what());
    return std::nullopt;
  }
}

auto SemanticStore::ActiveModel(std::string* error) const -> std::optional<SemanticModelRecord> {
  const auto key = ActiveModelKey();
  if (key.empty()) {
    return std::nullopt;
  }
  return GetModel(key, error);
}

auto SemanticStore::ActiveModelKey() const -> std::string {
  std::lock_guard lock(active_model_key_mutex_);
  return active_model_key_;
}

auto SemanticStore::ListModels(std::string* error) const -> std::vector<SemanticModelRecord> {
  auto guard   = database_.GetConnectionGuard();
  auto db_lock = guard.Lock();
  try {
    const auto rows = duckorm::select_by_query(
        guard.conn_, kModelRowFields, kModelRowFields.size(),
        expr::raw(std::format(
            "SELECT {} FROM SemanticModel ORDER BY created_at DESC, model_key DESC",
            kModelColumns)));
    std::vector<SemanticModelRecord> models;
    models.reserve(rows.size());
    for (const auto& row : rows) {
      models.push_back(MapModelRow(row));
    }
    return models;
  } catch (const std::exception& e) {
    SetError(error, e.what());
    return {};
  }
}

auto SemanticStore::PurgeModel(const std::string& model_key, std::string* error) const -> bool {
  auto guard   = database_.GetConnectionGuard();
  auto db_lock = guard.Lock();
  if (model_key.empty()) {
    SetError(error, "Semantic model key is empty.");
    return false;
  }
  // Embedding/prototype tables are dimension-sharded (512 vs 768); a given
  // model_key lives in only one of each pair, so deleting from both is a safe
  // no-op on the empty shard. SemanticModel holds the registry row itself.
  static constexpr std::array<const char*, 6> kTables = {
      "SemanticImageEmbedding", "SemanticImageEmbedding768", "SemanticImageLabel",
      "SemanticLabelPrototype", "SemanticLabelPrototype768", "SemanticModel",
  };
  try {
    duckorm::Transaction transaction(guard.conn_);
    const auto           where = ColumnEquals("model_key", model_key);
    for (const auto* table : kTables) {
      duckorm::remove(guard.conn_, table, where);
    }
    transaction.commit();
  } catch (const std::exception& e) {
    SetError(error, e.what());
    return false;
  }
  RefreshActiveModelKey(guard.conn_);
  return true;
}

auto SemanticStore::SetActiveModelKey(const std::string& model_key, std::string* error) const
    -> bool {
  auto guard   = database_.GetConnectionGuard();
  auto db_lock = guard.Lock();
  if (model_key.empty()) {
    SetError(error, "Semantic model key is empty.");
    return false;
  }
  if (!HasModel(model_key)) {
    SetError(error, "Semantic model is not registered.");
    return false;
  }
  try {
    duckorm::Transaction transaction(guard.conn_);
    duckorm::execute(guard.conn_, expr::raw("UPDATE SemanticModel SET active = FALSE"));
    auto activate = expr::raw("UPDATE SemanticModel SET active = TRUE WHERE ");
    activate.append(ColumnEquals("model_key", model_key));
    duckorm::execute(guard.conn_, activate);
    transaction.commit();
  } catch (const std::exception& e) {
    SetError(error, e.what());
    return false;
  }
  RefreshActiveModelKey(guard.conn_);
  return true;
}

auto SemanticStore::LatestModelKey() const -> std::string {
  auto guard   = database_.GetConnectionGuard();
  auto db_lock = guard.Lock();
  try {
    return duckorm::select_string(guard.conn_,
                                  expr::raw("SELECT model_key FROM SemanticModel ORDER BY "
                                            "created_at DESC, model_key DESC LIMIT 1"))
        .value_or(std::string{});
  } catch (const std::exception&) {
    return {};
  }
}

auto SemanticStore::UpsertImageEmbedding(const SemanticImageEmbeddingRecord& record,
                                         std::string*                        error) const -> bool {
  return UpsertImageEmbeddingWithLabel(record, nullptr, error);
}

auto SemanticStore::UpsertImageEmbeddingWithLabel(const SemanticImageEmbeddingRecord& record,
                                                  const SemanticImageLabelRecord*     label,
                                                  std::string* error) const -> bool {
  auto guard   = database_.GetConnectionGuard();
  auto db_lock = guard.Lock();
  if (record.file_id_ == 0) {
    SetError(error, "Semantic embedding file id is zero.");
    return false;
  }
  if (record.model_key_.empty()) {
    SetError(error, "Semantic embedding model key is empty.");
    return false;
  }
  try {
    const auto model_dim = ModelEmbeddingDim(guard.conn_, record.model_key_);
    if (!model_dim.has_value()) {
      SetError(error, "Semantic model is not registered.");
      return false;
    }
    if (!ValidateEmbedding(record.embedding_, *model_dim, error) ||
        !ValidateLabel(record, label, error)) {
      return false;
    }

    duckorm::Transaction transaction(guard.conn_);
    DeleteEmbeddingAndLabelRows(guard.conn_, *model_dim, record.model_key_,
                                std::span<const sl_element_id_t>(&record.file_id_, 1));
    InsertReadyImageEmbedding(guard.conn_, record, *model_dim);
    if (label) {
      InsertSemanticLabel(guard.conn_, *label);
    }
    transaction.commit();
    return true;
  } catch (const std::exception& e) {
    SetError(error, e.what());
    return false;
  }
}

auto SemanticStore::UpsertImageEmbeddingAndAssignLabel(
    const SemanticImageEmbeddingRecord&   record,
    const SemanticLabelAssignmentOptions& assignment_options,
    SemanticImageLabelRecord* assigned_label, std::string* error) const -> bool {
  auto             guard   = database_.GetConnectionGuard();
  auto             db_lock = guard.Lock();
  diag::TraceScope trace(diag::semanticDbLog(), QStringLiteral("semantic.db.embedding.upsert_one"),
                         QStringLiteral("file_id=%1 image_id=%2 model_key=%3")
                             .arg(static_cast<qulonglong>(record.file_id_))
                             .arg(static_cast<qulonglong>(record.image_id_))
                             .arg(QString::fromStdString(record.model_key_)));
  if (record.file_id_ == 0) {
    SetError(error, "Semantic embedding file id is zero.");
    return false;
  }
  if (record.model_key_.empty()) {
    SetError(error, "Semantic embedding model key is empty.");
    return false;
  }
  SemanticImageLabelRecord label;
  try {
    const auto model_dim = ModelEmbeddingDim(guard.conn_, record.model_key_);
    if (!model_dim.has_value()) {
      SetError(error, "Semantic model is not registered.");
      return false;
    }
    if (!ValidateEmbedding(record.embedding_, *model_dim, error)) {
      return false;
    }

    duckorm::Transaction transaction(guard.conn_);
    DeleteEmbeddingAndLabelRows(guard.conn_, *model_dim, record.model_key_,
                                std::span<const sl_element_id_t>(&record.file_id_, 1));
    InsertReadyImageEmbedding(guard.conn_, record, *model_dim);
    label = QueryAssignedLabel(guard.conn_, record, *model_dim, assignment_options);
    if (!ValidateLabel(record, &label, error)) {
      return false;
    }
    InsertSemanticLabel(guard.conn_, label);
    transaction.commit();
  } catch (const std::exception& e) {
    SetError(error, e.what());
    return false;
  }

  qCInfo(diag::semanticDbLog).noquote()
      << QStringLiteral(
             "semantic.db.embedding.upsert_one.ok file_id=%1 image_id=%2 label=%3 "
             "score=%4")
             .arg(static_cast<qulonglong>(record.file_id_))
             .arg(static_cast<qulonglong>(record.image_id_))
             .arg(QString::fromStdString(label.label_))
             .arg(label.score_, 0, 'f', 4);
  if (assigned_label) {
    *assigned_label = std::move(label);
  }
  return true;
}

auto SemanticStore::UpsertImageEmbeddingsAndAssignLabels(
    std::span<const SemanticImageEmbeddingRecord> records,
    const SemanticLabelAssignmentOptions&         assignment_options,
    std::vector<SemanticImageLabelRecord>* assigned_labels, std::string* error) const -> bool {
  auto guard   = database_.GetConnectionGuard();
  auto db_lock = guard.Lock();
  if (assigned_labels) {
    assigned_labels->clear();
  }
  if (records.empty()) {
    return true;
  }
  diag::TraceScope trace(diag::semanticDbLog(),
                         QStringLiteral("semantic.db.embedding.upsert_batch"),
                         QStringLiteral("count=%1 ids=%2")
                             .arg(static_cast<qulonglong>(records.size()))
                             .arg(SummarizeEmbeddingRecords(records)));

  std::vector<SemanticImageLabelRecord> labels;
  const auto&                           model_key = records.front().model_key_;
  try {
    ValidateAssignmentOptions(assignment_options);
    if (model_key.empty()) {
      SetError(error, "Semantic embedding model key is empty.");
      return false;
    }
    const auto model_dim = ModelEmbeddingDim(guard.conn_, model_key);
    if (!model_dim.has_value()) {
      SetError(error, "Semantic model is not registered.");
      return false;
    }
    std::vector<sl_element_id_t> file_ids;
    file_ids.reserve(records.size());
    for (const auto& record : records) {
      if (record.file_id_ == 0) {
        SetError(error, "Semantic embedding file id is zero.");
        return false;
      }
      if (record.model_key_ != model_key) {
        SetError(error, "Semantic embedding batch mixes multiple model keys.");
        return false;
      }
      if (!ValidateEmbedding(record.embedding_, *model_dim, error)) {
        return false;
      }
      file_ids.push_back(record.file_id_);
    }

    duckorm::Transaction transaction(guard.conn_);
    DeleteEmbeddingAndLabelRows(guard.conn_, *model_dim, model_key, file_ids);
    AppendImageEmbeddingRows(guard.conn_, records, *model_dim);
    labels = QueryAssignedLabelsBatch(guard.conn_, records, file_ids, model_key, *model_dim,
                                      assignment_options);
    for (size_t i = 0; i < records.size(); ++i) {
      if (!ValidateLabel(records[i], &labels[i], error)) {
        return false;
      }
    }
    AppendSemanticLabelRows(guard.conn_, labels);
    transaction.commit();
  } catch (const std::exception& e) {
    SetError(error, e.what());
    return false;
  }

  qCInfo(diag::semanticDbLog).noquote()
      << QStringLiteral("semantic.db.embedding.upsert_batch.ok count=%1 labels=%2 model_key=%3")
             .arg(static_cast<qulonglong>(records.size()))
             .arg(static_cast<qulonglong>(labels.size()))
             .arg(QString::fromStdString(model_key));
  if (assigned_labels) {
    *assigned_labels = std::move(labels);
  }
  return true;
}

auto SemanticStore::UpsertLabelPrototype(const SemanticLabelPrototypeRecord& record,
                                         std::string*                        error) const -> bool {
  return UpsertLabelPrototypes(std::span<const SemanticLabelPrototypeRecord>(&record, 1), error);
}

auto SemanticStore::UpsertLabelPrototypes(std::span<const SemanticLabelPrototypeRecord> records,
                                          std::string* error) const -> bool {
  auto guard   = database_.GetConnectionGuard();
  auto db_lock = guard.Lock();
  if (records.empty()) {
    return true;
  }
  try {
    duckorm::Transaction transaction(guard.conn_);
    for (const auto& record : records) {
      if (record.model_key_.empty()) {
        SetError(error, "Semantic label prototype model key is empty.");
        return false;
      }
      if (record.label_.empty()) {
        SetError(error, "Semantic label prototype label is empty.");
        return false;
      }
      if (record.prompt_config_hash_.empty()) {
        SetError(error, "Semantic label prototype prompt config hash is empty.");
        return false;
      }
      const auto model_dim = ModelEmbeddingDim(guard.conn_, record.model_key_);
      if (!model_dim.has_value()) {
        SetError(error, "Semantic model is not registered.");
        return false;
      }
      if (!ValidateEmbedding(record.embedding_, *model_dim, error)) {
        return false;
      }
      InsertOrReplaceLabelPrototype(guard.conn_, record, *model_dim);
    }
    transaction.commit();
    return true;
  } catch (const std::exception& e) {
    SetError(error, e.what());
    return false;
  }
}

void SemanticStore::DeleteImageEmbeddingsForFiles(
    std::span<const sl_element_id_t> file_ids) const {
  auto guard   = database_.GetConnectionGuard();
  auto db_lock = guard.Lock();
  DeleteSemanticRowsForFiles(guard.conn_, file_ids);
}

auto SemanticStore::CountImageEmbeddings(const std::string& model_key) const -> size_t {
  const auto model_dim = GetModelEmbeddingDim(model_key);
  if (!model_dim.has_value()) {
    return 0;
  }
  auto guard   = database_.GetConnectionGuard();
  auto db_lock = guard.Lock();
  auto query   = expr::raw(
      std::format("SELECT COUNT(*) FROM {} WHERE ", ImageEmbeddingTableName(*model_dim)));
  query.append(ColumnEquals("model_key", model_key));
  return CountOrZero(guard.conn_, query);
}

auto SemanticStore::CountImageEmbeddingsForFile(sl_element_id_t    file_id,
                                                const std::string& model_key) const -> size_t {
  const auto model_dim = GetModelEmbeddingDim(model_key);
  if (!model_dim.has_value()) {
    return 0;
  }
  auto guard   = database_.GetConnectionGuard();
  auto db_lock = guard.Lock();
  auto query   = expr::raw(
      std::format("SELECT COUNT(*) FROM {} WHERE ", ImageEmbeddingTableName(*model_dim)));
  query.append(expr::and_({ColumnEquals("file_id", static_cast<int64_t>(file_id)),
                           ColumnEquals("model_key", model_key)}));
  return CountOrZero(guard.conn_, query);
}

auto SemanticStore::HasReadyImageEmbedding(sl_element_id_t file_id, image_id_t image_id,
                                           const std::string& model_key,
                                           bool               require_label) const -> bool {
  const auto model_dim = GetModelEmbeddingDim(model_key);
  if (!model_dim.has_value()) {
    return false;
  }
  auto guard   = database_.GetConnectionGuard();
  auto db_lock = guard.Lock();
  auto query   = expr::raw(std::format(
      "SELECT COUNT(*) FROM {} se {}WHERE ", ImageEmbeddingTableName(*model_dim),
      require_label ? "JOIN SemanticImageLabel sl ON sl.file_id = se.file_id AND "
                        "sl.model_key = se.model_key "
                      : ""));
  query.append(expr::and_({ColumnEquals("se.file_id", static_cast<int64_t>(file_id)),
                           ColumnEquals("se.image_id", static_cast<int64_t>(image_id)),
                           ColumnEquals("se.model_key", model_key),
                           ColumnEquals("se.embedding_dim", static_cast<int64_t>(*model_dim)),
                           expr::raw("se.status = 'ready' AND se.error IS NULL")}));
  if (require_label) {
    query.append(expr::raw(" AND sl.label IS NOT NULL AND sl.label <> ''"));
  }
  return CountOrZero(guard.conn_, query) > 0;
}

auto SemanticStore::CountImageLabelsForFile(sl_element_id_t    file_id,
                                            const std::string& model_key) const -> size_t {
  auto guard   = database_.GetConnectionGuard();
  auto db_lock = guard.Lock();
  auto query   = expr::raw("SELECT COUNT(*) FROM SemanticImageLabel WHERE ");
  query.append(expr::and_({ColumnEquals("file_id", static_cast<int64_t>(file_id)),
                           ColumnEquals("model_key", model_key)}));
  return CountOrZero(guard.conn_, query);
}

auto SemanticStore::CountImageLabelsInFolder(sl_element_id_t    folder_id,
                                             const std::string& model_key) const -> size_t {
  if (model_key.empty()) {
    return 0;
  }
  auto       guard   = database_.GetConnectionGuard();
  auto       db_lock = guard.Lock();
  const auto scope   = BuildScopedFileQuery(folder_id);
  auto       query   = expr::raw(std::format(
      "SELECT COUNT(*) FROM SemanticImageLabel sl "
                "JOIN (SELECT e.id AS file_id {}) scoped ON scoped.file_id = sl.file_id WHERE ",
      scope.from_where_));
  query.append(ColumnEquals("sl.model_key", model_key));
  query.append(expr::raw(" AND sl.label IS NOT NULL AND sl.label <> ''"));
  return CountOrZero(guard.conn_, query);
}

auto SemanticStore::CountLabelPrototypes(const std::string& model_key,
                                         const std::string& prompt_config_hash) const -> size_t {
  const auto model_dim = GetModelEmbeddingDim(model_key);
  if (!model_dim.has_value()) {
    return 0;
  }
  auto guard   = database_.GetConnectionGuard();
  auto db_lock = guard.Lock();
  auto query   = expr::raw(
      std::format("SELECT COUNT(*) FROM {} WHERE ", LabelPrototypeTableName(*model_dim)));
  query.append(expr::and_({ColumnEquals("model_key", model_key),
                           ColumnEquals("prompt_config_hash", prompt_config_hash)}));
  return CountOrZero(guard.conn_, query);
}

auto SemanticStore::CountLabelQueries(const std::string& prompt_config_hash) const -> size_t {
  auto guard   = database_.GetConnectionGuard();
  auto db_lock = guard.Lock();
  auto query   = expr::raw("SELECT COUNT(*) FROM SemanticLabelQuery WHERE ");
  query.append(ColumnEquals("prompt_config_hash", prompt_config_hash));
  return CountOrZero(guard.conn_, query);
}

auto SemanticStore::ListLabelQueries(const std::string& prompt_config_hash,
                                     std::string*       error) const
    -> std::vector<SemanticLabelQueryRecord> {
  auto guard   = database_.GetConnectionGuard();
  auto db_lock = guard.Lock();
  auto query   = expr::raw(
      "SELECT prompt_config_hash, label, query_text FROM SemanticLabelQuery WHERE ");
  query.append(ColumnEquals("prompt_config_hash", prompt_config_hash));
  query.append(expr::raw(" ORDER BY label"));
  static const std::array<duckorm::DuckFieldDesc, 3> fields = {
      Column("prompt_config_hash", duckorm::DuckDBType::VARCHAR),
      Column("label", duckorm::DuckDBType::VARCHAR),
      Column("query_text", duckorm::DuckDBType::VARCHAR)};

  std::vector<SemanticLabelQueryRecord> out;
  try {
    const auto rows = duckorm::select_by_query(guard.conn_, fields, fields.size(), query);
    out.reserve(rows.size());
    for (const auto& row : rows) {
      out.push_back(SemanticLabelQueryRecord{.prompt_config_hash_ = CellString(row[0]),
                                             .label_              = CellString(row[1]),
                                             .query_text_         = CellString(row[2])});
    }
  } catch (const std::exception& e) {
    SetError(error, e.what());
    out.clear();
  }
  return out;
}

auto SemanticStore::LoadLabelPrototypes(const std::string& model_key,
                                        const std::string& prompt_config_hash,
                                        std::string*       error) const
    -> std::vector<SemanticGenerationLabelPrototype> {
  auto                                          guard   = database_.GetConnectionGuard();
  auto                                          db_lock = guard.Lock();
  std::vector<SemanticGenerationLabelPrototype> out;
  try {
    const auto model_dim = ModelEmbeddingDim(guard.conn_, model_key);
    if (!model_dim.has_value()) {
      SetError(error, "Semantic model is not registered.");
      return out;
    }
    if (!IsSupportedSemanticEmbeddingDim(*model_dim)) {
      SetError(error, std::format("Semantic storage does not support {}-dimensional embeddings.",
                                  *model_dim));
      return out;
    }

    // One column for each embedding element: the select decoder has no array type.
    std::string                         columns;
    std::vector<duckorm::DuckFieldDesc> fields = {Column("label", duckorm::DuckDBType::VARCHAR)};
    fields.reserve(static_cast<size_t>(*model_dim) + 1);
    for (int i = 1; i <= *model_dim; ++i) {
      columns += std::format(", embedding[{}]", i);
      fields.push_back(Column("embedding", duckorm::DuckDBType::DOUBLE));
    }
    auto query = expr::raw(std::format("SELECT label{} FROM {} WHERE ", columns,
                                       LabelPrototypeTableName(*model_dim)));
    query.append(expr::and_({ColumnEquals("model_key", model_key),
                             ColumnEquals("prompt_config_hash", prompt_config_hash)}));
    query.append(expr::raw(" ORDER BY label"));

    const auto rows = duckorm::select_by_query(guard.conn_, fields, fields.size(), query);
    out.reserve(rows.size());
    for (const auto& row : rows) {
      SemanticGenerationLabelPrototype prototype;
      prototype.label = CellString(row[0]);
      prototype.embedding.resize(static_cast<size_t>(*model_dim), 0.0F);
      for (int i = 0; i < *model_dim; ++i) {
        prototype.embedding[static_cast<size_t>(i)] =
            static_cast<float>(std::get<double>(row[static_cast<size_t>(i) + 1]));
      }
      out.push_back(std::move(prototype));
    }
  } catch (const std::exception& e) {
    SetError(error, e.what());
    out.clear();
  }
  return out;
}

auto SemanticStore::GetImageLabelForFile(sl_element_id_t file_id, const std::string& model_key,
                                         std::string* error) const
    -> std::optional<SemanticImageLabelRecord> {
  auto guard   = database_.GetConnectionGuard();
  auto db_lock = guard.Lock();
  auto query   = expr::raw(
      "SELECT file_id, model_key, label, score, second_label, second_score, margin, confident, "
        "top_scores FROM SemanticImageLabel WHERE ");
  query.append(expr::and_({ColumnEquals("file_id", static_cast<int64_t>(file_id)),
                           ColumnEquals("model_key", model_key)}));
  static const std::array<duckorm::DuckFieldDesc, 9> fields = {
      Column("file_id", duckorm::DuckDBType::INT64),
      Column("model_key", duckorm::DuckDBType::VARCHAR),
      Column("label", duckorm::DuckDBType::VARCHAR),
      Column("score", duckorm::DuckDBType::DOUBLE),
      Column("second_label", duckorm::DuckDBType::NULLABLE_STRING),
      Column("second_score", duckorm::DuckDBType::NULLABLE_DOUBLE),
      Column("margin", duckorm::DuckDBType::DOUBLE),
      Column("confident", duckorm::DuckDBType::BOOLEAN),
      Column("top_scores", duckorm::DuckDBType::NULLABLE_STRING)};
  try {
    const auto rows = duckorm::select_by_query(guard.conn_, fields, fields.size(), query);
    if (rows.empty()) {
      return std::nullopt;
    }
    const auto&              row = rows.front();
    SemanticImageLabelRecord record;
    record.file_id_         = static_cast<sl_element_id_t>(std::get<int64_t>(row[0]));
    record.model_key_       = CellString(row[1]);
    record.label_           = CellString(row[2]);
    record.score_           = std::get<double>(row[3]);
    record.second_label_    = CellString(row[4]);
    record.second_score_    = std::get<std::optional<double>>(row[5]);
    record.margin_          = std::get<double>(row[6]);
    record.confident_       = CellBool(row[7]);
    record.top_scores_json_ = CellString(row[8]);
    return record;
  } catch (const std::exception& e) {
    SetError(error, e.what());
    return std::nullopt;
  }
}

auto SemanticStore::SearchImageEmbeddings(sl_element_id_t folder_id, const std::string& model_key,
                                          std::span<const float> query_embedding, size_t offset,
                                          size_t limit, std::string* error) const
    -> std::vector<SemanticRankedFile> {
  auto             guard   = database_.GetConnectionGuard();
  auto             db_lock = guard.Lock();
  diag::TraceScope trace(diag::semanticDbLog(), QStringLiteral("semantic.db.search"),
                         QStringLiteral("folder_id=%1 model_key=%2 offset=%3 limit=%4 dim=%5")
                             .arg(static_cast<qulonglong>(folder_id))
                             .arg(QString::fromStdString(model_key))
                             .arg(static_cast<qulonglong>(offset))
                             .arg(static_cast<qulonglong>(limit))
                             .arg(static_cast<qulonglong>(query_embedding.size())));
  if (limit == 0) {
    return {};
  }
  const auto model_dim = GetModelEmbeddingDim(model_key);
  if (!model_dim.has_value()) {
    SetError(error, "Semantic model is not registered.");
    return {};
  }
  if (!ValidateEmbedding(query_embedding, *model_dim, error)) {
    return {};
  }
  if (!EnsureVectorSearchIndex(model_key, error)) {
    return {};
  }

  // The query vector is an array literal: DuckDB binds here have no array type.
  const auto scope         = BuildScopedFileQuery(folder_id);
  const auto query_literal = expr::lit_float_array(query_embedding);
  auto       query         = expr::raw(
      "WITH nearest AS (SELECT se.file_id, se.image_id, array_distance(se.embedding, ");
  query.append(query_literal);
  query.append(expr::raw(
      std::format(") AS distance FROM {} se WHERE ", ImageEmbeddingTableName(*model_dim))));
  query.append(ColumnEquals("se.model_key", model_key));
  query.append(expr::raw(std::format(
      " AND se.status = 'ready' AND se.embedding_dim = {} ORDER BY array_distance(se.embedding, ",
      *model_dim)));
  query.append(query_literal);
  query.append(expr::raw(std::format(
      ") ASC LIMIT {}), "
      "scoped AS (SELECT e.id AS file_id, fi.image_id AS image_id, e.element_name AS file_name "
      "{}), "
      "ranked AS (SELECT scoped.file_id, scoped.image_id, scoped.file_name, "
      "1.0 - ((nearest.distance * nearest.distance) / 2.0) AS score "
      "FROM nearest JOIN scoped ON scoped.file_id = nearest.file_id "
      "AND scoped.image_id = nearest.image_id) "
      "SELECT file_id, image_id, file_name, score FROM ranked ORDER BY score DESC, file_id",
      RequestedCandidateLimit(offset, limit), scope.from_where_)));

  static const std::array<duckorm::DuckFieldDesc, 4> fields = {
      Column("file_id", duckorm::DuckDBType::INT64), Column("image_id", duckorm::DuckDBType::INT64),
      Column("file_name", duckorm::DuckDBType::VARCHAR),
      Column("score", duckorm::DuckDBType::DOUBLE)};
  std::vector<SemanticRankedFile> candidates;
  try {
    const auto rows = duckorm::select_by_query(guard.conn_, fields, fields.size(), query);
    candidates.reserve(rows.size());
    for (const auto& row : rows) {
      candidates.push_back(
          SemanticRankedFile{.file_id_   = static_cast<sl_element_id_t>(std::get<int64_t>(row[0])),
                             .image_id_  = static_cast<image_id_t>(std::get<int64_t>(row[1])),
                             .file_name_ = CellString(row[2]),
                             .score_     = std::get<double>(row[3])});
    }
  } catch (const std::exception& e) {
    SetError(error, e.what());
    return {};
  }

  const auto raw_count = candidates.size();
  auto       page      = FilterAndPageSemanticCandidates(std::move(candidates), offset, limit);
  qCInfo(diag::semanticDbLog).noquote()
      << QStringLiteral(
             "semantic.db.search.result folder_id=%1 model_key=%2 raw_candidates=%3 "
             "page_count=%4 offset=%5 limit=%6")
             .arg(static_cast<qulonglong>(folder_id))
             .arg(QString::fromStdString(model_key))
             .arg(static_cast<qulonglong>(raw_count))
             .arg(static_cast<qulonglong>(page.size()))
             .arg(static_cast<qulonglong>(offset))
             .arg(static_cast<qulonglong>(limit));
  return page;
}

auto SemanticStore::EnsureVectorSearchIndex(const std::string& model_key,
                                            std::string*       error) const -> bool {
  const auto model_dim = GetModelEmbeddingDim(model_key);
  if (!model_dim.has_value()) {
    SetError(error, "Semantic model is not registered.");
    return false;
  }
  if (!IsSupportedSemanticEmbeddingDim(*model_dim)) {
    SetError(error, std::format("Semantic storage does not support {}-dimensional embeddings.",
                                *model_dim));
    return false;
  }
  auto guard   = database_.GetConnectionGuard();
  auto db_lock = guard.Lock();
  if (!LoadPackagedDuckDbExtension(guard.conn_, "vss", error)) {
    return false;
  }
  try {
    duckorm::execute(guard.conn_, expr::raw("SET hnsw_enable_experimental_persistence = true;"));
    duckorm::execute(guard.conn_,
                     expr::raw(std::format("CREATE INDEX IF NOT EXISTS {} ON {} USING HNSW "
                                           "(embedding);",
                                           ImageEmbeddingIndexName(*model_dim),
                                           ImageEmbeddingTableName(*model_dim))));
    return true;
  } catch (const std::exception& e) {
    SetError(error, e.what());
    return false;
  }
}
}  // namespace alcedo
