//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "storage/store/semantic/semantic_vector_search.hpp"

#include <QString>
#include <array>
#include <cmath>
#include <cstdint>
#include <exception>
#include <format>
#include <optional>
#include <span>
#include <stdexcept>
#include <variant>

#include "semantic_tables.hpp"
#include "storage/mapper/duckorm/duckdb_orm.hpp"
#include "storage/store/sleeve/element_store.hpp"
#include "utils/diagnostics/app_logging.hpp"

namespace alcedo {
namespace {
namespace expr = duckorm::expr;
using semantic_tables::Column;
using semantic_tables::ImageEmbeddingTable;
using semantic_tables::SetError;

/// Checks the embedding against the registered model and returns the model dimension.
/// @throws std::runtime_error with the reason.
auto ValidatedModelDim(duckdb_connection conn, const SemanticQueryEmbedding& embedding) -> int {
  const auto model_dim = semantic_tables::ModelEmbeddingDim(conn, embedding.model_key_);
  if (!model_dim.has_value()) {
    throw std::runtime_error("Semantic model is not registered.");
  }
  if (embedding.dimension_ != *model_dim) {
    throw std::runtime_error("Semantic query embedding dimension does not match the model.");
  }
  std::string error;
  if (!semantic_tables::ValidateEmbedding(embedding.query_, *model_dim, &error)) {
    throw std::runtime_error("Semantic query embedding is invalid: " + error);
  }
  if (!semantic_tables::ValidateEmbedding(embedding.neutral_, *model_dim, &error)) {
    throw std::runtime_error("Semantic neutral embedding is invalid: " + error);
  }
  return *model_dim;
}

/// @p values scaled to unit length. ValidatedModelDim has rejected a zero norm.
auto UnitLength(std::span<const float> values) -> std::vector<double> {
  double norm_sq = 0.0;
  for (const auto value : values) {
    norm_sq += static_cast<double>(value) * static_cast<double>(value);
  }
  const double        norm = std::sqrt(norm_sq);
  std::vector<double> unit;
  unit.reserve(values.size());
  for (const auto value : values) {
    unit.push_back(static_cast<double>(value) / norm);
  }
  return unit;
}

/// The unit-length query vector q of @p embedding.
auto QueryDirection(const SemanticQueryEmbedding& embedding) -> std::vector<float> {
  const auto         unit = UnitLength(embedding.query_);
  std::vector<float> direction(unit.begin(), unit.end());
  return direction;
}

/// q - n of the unit-length query and neutral vectors: `x . (q - n) / |x|` is the relevance.
auto RelevanceDirection(const SemanticQueryEmbedding& embedding) -> std::vector<float> {
  const auto         query   = UnitLength(embedding.query_);
  const auto         neutral = UnitLength(embedding.neutral_);
  std::vector<float> direction(query.size());
  for (size_t i = 0; i < query.size(); ++i) {
    direction[i] = static_cast<float>(query[i] - neutral[i]);
  }
  return direction;
}

/**
 * The common table expressions of the relevance rule over the folder scope:
 * `scoped` (the direct child files), `scored` (c and r of each file with a ready embedding),
 * `stats` (count, mean, and population deviation of r), and `rated` (c, r, the z value of r,
 * and the scored count on each row).
 *
 * c and b are cosine similarities. With the text vectors q and n scaled to unit length,
 * `c = x . q / |x|`, `r = c - b = x . (q - n) / |x|`, and `|x| = sqrt(x . x)`: three inner
 * products per embedding row. Stored image vectors need not have unit length.
 *
 * The products are computed only for the embedding rows of scope files, in two steps. The
 * `scope_row` join reads only the scalar columns of the ready embedding rows of the model and
 * returns the row ids of the scope files. The outer scan then reads the vectors of those rows
 * through a semi join on the row id and computes the products in a projection over it. The semi
 * join builds its hash table on the row ids, because the outer scan has no filter and its
 * estimate is the full table, so no vector is copied into a hash table. When the vectors go
 * into a join with the scope, DuckDB 1.2 builds the hash table on the embedding rows and copies
 * every vector (about 60 ms at 20 000 rows of 512 values). The semi join also pushes the row id
 * range of the scope rows into the outer scan, which skips the vectors outside that range.
 * `scored` is materialized because `stats` and `rated` both read it. The vectors are array
 * literals: DuckDB binds have no array type.
 */
auto BuildRatedCte(sl_element_id_t folder_id, const SemanticQueryEmbedding& embedding,
                   int model_dim) -> duckorm::SqlFragment {
  const auto scope = BuildScopedFileQuery(folder_id);
  auto       query = expr::raw(std::format(
      "WITH scoped AS (SELECT e.id AS file_id, fi.image_id AS image_id {}), "
            "scored AS MATERIALIZED (SELECT products.file_id, products.image_id, "
            "products.query_product / products.embedding_norm AS query_similarity, "
            "products.relevance_product / products.embedding_norm AS relevance "
            "FROM (SELECT se.file_id, se.image_id, "
            "sqrt(CAST(array_inner_product(se.embedding, se.embedding) AS DOUBLE)) AS "
            "embedding_norm, CAST(array_inner_product(se.embedding, ",
      scope.from_where_));
  const char* table = ImageEmbeddingTable(model_dim);
  query.append(expr::lit_float_array(QueryDirection(embedding)));
  query.append(expr::raw(") AS DOUBLE) AS query_product, CAST(array_inner_product(se.embedding, "));
  query.append(expr::lit_float_array(RelevanceDirection(embedding)));
  query.append(expr::raw(std::format(
      ") AS DOUBLE) AS relevance_product FROM {0} se WHERE se.rowid IN (SELECT scope_row.rowid "
      "FROM {0} scope_row JOIN scoped ON scoped.file_id = scope_row.file_id AND "
      "scoped.image_id = scope_row.image_id WHERE ",
      table)));
  query.append(expr::column_eq("scope_row.model_key", embedding.model_key_));
  query.append(expr::raw(std::format(
      " AND scope_row.status = 'ready' AND scope_row.embedding_dim = {})) products), "
      "stats AS (SELECT COUNT(*) AS scored_count, AVG(relevance) AS relevance_mean, "
      "STDDEV_POP(relevance) AS relevance_deviation FROM scored), "
      "rated AS (SELECT scored.file_id, scored.image_id, scored.query_similarity, "
      "scored.relevance, CASE WHEN stats.relevance_deviation > 0 THEN "
      "(scored.relevance - stats.relevance_mean) / stats.relevance_deviation END AS relevance_z, "
      "stats.scored_count FROM scored CROSS JOIN stats) ",
      model_dim)));
  return query;
}

/// The keep rule over the columns of `rated`. A NULL z value (zero deviation) keeps no file
/// when the scope statistics are used.
auto KeepCondition() -> duckorm::SqlFragment {
  auto condition = expr::raw("(query_similarity >= ");
  condition.append(expr::param(SemanticVectorSearch::kMinQuerySimilarity));
  condition.append(expr::raw(" AND (scored_count < "));
  condition.append(
      expr::param(static_cast<int64_t>(SemanticVectorSearch::kMinScopeStatisticsCount)));
  condition.append(expr::raw(" OR relevance_z >= "));
  condition.append(expr::param(SemanticVectorSearch::kMinRelevanceZ));
  condition.append(expr::raw("))"));
  return condition;
}

auto BuildRelevanceSelectWithDim(sl_element_id_t folder_id, const SemanticQueryEmbedding& embedding,
                                 int model_dim) -> duckorm::SqlFragment {
  auto query = BuildRatedCte(folder_id, embedding, model_dim);
  query.append(expr::raw("SELECT file_id, image_id, relevance FROM rated WHERE "));
  query.append(KeepCondition());
  return query;
}
}  // namespace

SemanticVectorSearch::SemanticVectorSearch(Database& db_ctrl) : database_(db_ctrl) {}

auto BuildSemanticRelevanceSelect(duckorm::Connection conn, sl_element_id_t folder_id,
                                  const SemanticQueryEmbedding& embedding) -> duckorm::SqlFragment {
  return BuildRelevanceSelectWithDim(folder_id, embedding, ValidatedModelDim(conn, embedding));
}

auto SemanticVectorSearch::SearchImageEmbeddings(sl_element_id_t               folder_id,
                                                 const SemanticQueryEmbedding& embedding,
                                                 size_t offset, size_t limit,
                                                 std::string* error) const -> SemanticRankedPage {
  auto             guard   = database_.GetConnectionGuard();
  auto             db_lock = guard.Lock();
  diag::TraceScope trace(diag::semanticDbLog(), QStringLiteral("semantic.db.search"),
                         QStringLiteral("folder_id=%1 model_key=%2 offset=%3 limit=%4 dim=%5")
                             .arg(static_cast<qulonglong>(folder_id))
                             .arg(QString::fromStdString(embedding.model_key_))
                             .arg(static_cast<qulonglong>(offset))
                             .arg(static_cast<qulonglong>(limit))
                             .arg(static_cast<qulonglong>(embedding.query_.size())));
  SemanticRankedPage page;
  try {
    const auto model_dim = ValidatedModelDim(guard.conn_, embedding);
    const auto kept      = BuildRelevanceSelectWithDim(folder_id, embedding, model_dim);
    const auto count_kept = [&]() -> size_t {
      auto count = expr::raw("SELECT COUNT(*) FROM (");
      count.append(kept);
      count.append(expr::raw(") kept"));
      return static_cast<size_t>(duckorm::select_int64(guard.conn_, count).value_or(0));
    };

    if (limit == 0) {
      page.total_ = count_kept();
      return page;
    }

    auto query = expr::raw(
        "SELECT file_id, image_id, relevance, COUNT(*) OVER () AS kept_count FROM (");
    query.append(kept);
    query.append(expr::raw(") kept ORDER BY relevance DESC, file_id ASC LIMIT "));
    query.append(expr::param(static_cast<int64_t>(limit)));
    query.append(expr::raw(" OFFSET "));
    query.append(expr::param(static_cast<int64_t>(offset)));

    static const std::array<duckorm::DuckFieldDesc, 4> fields = {
        Column("file_id", duckorm::DuckDBType::INT64),
        Column("image_id", duckorm::DuckDBType::INT64),
        Column("relevance", duckorm::DuckDBType::DOUBLE),
        Column("kept_count", duckorm::DuckDBType::INT64)};
    const auto rows = duckorm::select_by_query(guard.conn_, fields, fields.size(), query);
    page.rows_.reserve(rows.size());
    for (const auto& row : rows) {
      page.rows_.push_back(
          SemanticRankedFile{.file_id_   = static_cast<sl_element_id_t>(std::get<int64_t>(row[0])),
                             .image_id_  = static_cast<image_id_t>(std::get<int64_t>(row[1])),
                             .relevance_ = std::get<double>(row[2])});
    }
    // The window value exists only on a returned row; a page past the end counts separately.
    page.total_ = rows.empty() ? (offset == 0 ? 0 : count_kept())
                               : static_cast<size_t>(std::get<int64_t>(rows.front()[3]));
  } catch (const std::exception& e) {
    SetError(error, e.what());
    return {};
  }

  qCInfo(diag::semanticDbLog).noquote()
      << QStringLiteral(
             "semantic.db.search.result folder_id=%1 model_key=%2 kept=%3 page_count=%4 "
             "offset=%5 limit=%6")
             .arg(static_cast<qulonglong>(folder_id))
             .arg(QString::fromStdString(embedding.model_key_))
             .arg(static_cast<qulonglong>(page.total_))
             .arg(static_cast<qulonglong>(page.rows_.size()))
             .arg(static_cast<qulonglong>(offset))
             .arg(static_cast<qulonglong>(limit));
  return page;
}

auto SemanticVectorSearch::ReadRelevanceScores(sl_element_id_t               folder_id,
                                               const SemanticQueryEmbedding& embedding,
                                               std::string*                  error) const
    -> std::vector<SemanticRelevanceScore> {
  auto guard   = database_.GetConnectionGuard();
  auto db_lock = guard.Lock();
  std::vector<SemanticRelevanceScore> scores;
  try {
    const auto model_dim = ValidatedModelDim(guard.conn_, embedding);
    auto       query     = BuildRatedCte(folder_id, embedding, model_dim);
    query.append(expr::raw(
        "SELECT rated.file_id, rated.image_id, e.element_name AS file_name, query_similarity, "
        "query_similarity - relevance AS neutral_similarity, relevance, relevance_z, CAST("));
    query.append(KeepCondition());
    query.append(expr::raw(
        " AS INTEGER) AS kept FROM rated JOIN Element e ON e.id = rated.file_id "
        "ORDER BY relevance DESC, rated.file_id"));

    static const std::array<duckorm::DuckFieldDesc, 8> fields = {
        Column("file_id", duckorm::DuckDBType::INT64),
        Column("image_id", duckorm::DuckDBType::INT64),
        Column("file_name", duckorm::DuckDBType::VARCHAR),
        Column("query_similarity", duckorm::DuckDBType::DOUBLE),
        Column("neutral_similarity", duckorm::DuckDBType::DOUBLE),
        Column("relevance", duckorm::DuckDBType::DOUBLE),
        Column("relevance_z", duckorm::DuckDBType::NULLABLE_DOUBLE),
        Column("kept", duckorm::DuckDBType::NULLABLE_INT64)};
    const auto rows = duckorm::select_by_query(guard.conn_, fields, fields.size(), query);
    scores.reserve(rows.size());
    for (const auto& row : rows) {
      const auto kept = std::get<std::optional<int64_t>>(row[7]);
      scores.push_back(SemanticRelevanceScore{
          .file_id_            = static_cast<sl_element_id_t>(std::get<int64_t>(row[0])),
          .image_id_           = static_cast<image_id_t>(std::get<int64_t>(row[1])),
          .file_name_          = duckorm::cell_text(row[2]),
          .query_similarity_   = std::get<double>(row[3]),
          .neutral_similarity_ = std::get<double>(row[4]),
          .relevance_          = std::get<double>(row[5]),
          .relevance_z_        = std::get<std::optional<double>>(row[6]),
          .kept_               = kept.value_or(0) != 0});
    }
  } catch (const std::exception& e) {
    SetError(error, e.what());
    return {};
  }
  return scores;
}

}  // namespace alcedo
