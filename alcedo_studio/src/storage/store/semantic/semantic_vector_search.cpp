//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "storage/store/semantic/semantic_vector_search.hpp"

#include <QString>
#include <algorithm>
#include <array>
#include <cstdint>
#include <exception>
#include <format>
#include <iterator>
#include <limits>
#include <variant>

#include "semantic_score_elbow.hpp"
#include "semantic_tables.hpp"
#include "storage/mapper/duckorm/duckdb_expr.hpp"
#include "storage/mapper/duckorm/duckdb_orm.hpp"
#include "storage/store/duckdb_extension.hpp"
#include "storage/store/sleeve/element_store.hpp"
#include "utils/diagnostics/app_logging.hpp"

namespace alcedo {
namespace {
namespace expr = duckorm::expr;
using semantic_tables::Column;
using semantic_tables::ImageEmbeddingTable;
using semantic_tables::SetError;

constexpr size_t kMinCandidatePool = 512;

auto RequestedCandidateLimit(size_t offset, size_t limit) -> size_t {
  const auto max_size      = std::numeric_limits<size_t>::max();
  const auto requested_end = offset > max_size - limit ? max_size : offset + limit;
  return std::max(requested_end, kMinCandidatePool);
}

auto FilterAndPageCandidates(std::vector<SemanticRankedFile> candidates, size_t offset,
                             size_t limit) -> std::vector<SemanticRankedFile> {
  std::vector<double> scores;
  scores.reserve(candidates.size());
  for (const auto& row : candidates) {
    scores.push_back(row.score_);
  }
  const double cutoff = semantic_score_elbow::CutoffScore(scores);
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
}  // namespace

SemanticVectorSearch::SemanticVectorSearch(Database& db_ctrl) : database_(db_ctrl) {}

auto SemanticVectorSearch::SearchImageEmbeddings(sl_element_id_t        folder_id,
                                                 const std::string&     model_key,
                                                 std::span<const float> query_embedding,
                                                 size_t offset, size_t limit,
                                                 std::string* error) const
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
  std::vector<SemanticRankedFile> candidates;
  try {
    const auto model_dim = semantic_tables::ModelEmbeddingDim(guard.conn_, model_key);
    if (!model_dim.has_value()) {
      SetError(error, "Semantic model is not registered.");
      return {};
    }
    if (!semantic_tables::ValidateEmbedding(query_embedding, *model_dim, error)) {
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
        std::format(") AS distance FROM {} se WHERE ", ImageEmbeddingTable(*model_dim))));
    query.append(expr::column_eq("se.model_key", model_key));
    query.append(expr::raw(std::format(" AND se.status = 'ready' AND se.embedding_dim = {} ORDER "
                                       "BY array_distance(se.embedding, ",
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
        Column("file_id", duckorm::DuckDBType::INT64),
        Column("image_id", duckorm::DuckDBType::INT64),
        Column("file_name", duckorm::DuckDBType::VARCHAR),
        Column("score", duckorm::DuckDBType::DOUBLE)};
    const auto rows = duckorm::select_by_query(guard.conn_, fields, fields.size(), query);
    candidates.reserve(rows.size());
    for (const auto& row : rows) {
      candidates.push_back(
          SemanticRankedFile{.file_id_   = static_cast<sl_element_id_t>(std::get<int64_t>(row[0])),
                             .image_id_  = static_cast<image_id_t>(std::get<int64_t>(row[1])),
                             .file_name_ = duckorm::cell_text(row[2]),
                             .score_     = std::get<double>(row[3])});
    }
  } catch (const std::exception& e) {
    SetError(error, e.what());
    return {};
  }

  const auto raw_count = candidates.size();
  auto       page      = FilterAndPageCandidates(std::move(candidates), offset, limit);
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

auto SemanticVectorSearch::EnsureVectorSearchIndex(const std::string& model_key,
                                                   std::string*       error) const -> bool {
  auto guard   = database_.GetConnectionGuard();
  auto db_lock = guard.Lock();
  try {
    const auto model_dim = semantic_tables::ModelEmbeddingDim(guard.conn_, model_key);
    if (!model_dim.has_value()) {
      SetError(error, "Semantic model is not registered.");
      return false;
    }
    if (!semantic_tables::IsSupportedEmbeddingDim(*model_dim)) {
      SetError(error, std::format("Semantic storage does not support {}-dimensional embeddings.",
                                  *model_dim));
      return false;
    }
    if (!LoadPackagedDuckDbExtension(guard.conn_, "vss", error)) {
      return false;
    }
    duckorm::execute(guard.conn_, expr::raw("SET hnsw_enable_experimental_persistence = true;"));
    duckorm::execute(guard.conn_,
                     expr::raw(std::format("CREATE INDEX IF NOT EXISTS {} ON {} USING HNSW "
                                           "(embedding);",
                                           semantic_tables::ImageEmbeddingIndex(*model_dim),
                                           ImageEmbeddingTable(*model_dim))));
    return true;
  } catch (const std::exception& e) {
    SetError(error, e.what());
    return false;
  }
}

}  // namespace alcedo
