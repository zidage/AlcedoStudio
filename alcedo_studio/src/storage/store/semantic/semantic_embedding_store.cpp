//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "storage/store/semantic/semantic_embedding_store.hpp"

#include <QString>
#include <QStringList>
#include <algorithm>
#include <array>
#include <cstdint>
#include <exception>
#include <format>
#include <stdexcept>

#include "semantic_label_assignment.hpp"
#include "semantic_tables.hpp"
#include "storage/mapper/duckorm/duckdb_expr.hpp"
#include "storage/mapper/duckorm/duckdb_orm.hpp"
#include "utils/diagnostics/app_logging.hpp"

namespace alcedo {
namespace {
namespace expr = duckorm::expr;
using semantic_tables::ImageEmbeddingTable;
using semantic_tables::SetError;
using semantic_tables::ValidateEmbedding;

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
                                       ImageEmbeddingTable(model_dim), model_dim),
                           &record, fields, fields.size());
}

// Bulk-inserts ready image embedding rows through the DuckDB Appender within the caller's
// transaction. Only the non-default columns are appended; `generated_at` and `error` fall back
// to their column defaults.
void AppendImageEmbeddings(duckdb_connection                             conn,
                           std::span<const SemanticImageEmbeddingRecord> records, int model_dim) {
  static constexpr std::array<const char*, 7> kColumns = {
      "file_id", "image_id", "model_key", "embedding", "embedding_dim", "thumbnail_resolution",
      "status"};
  duckorm::Appender appender(conn, ImageEmbeddingTable(model_dim), kColumns);
  for (const auto& record : records) {
    if (record.embedding_.size() != static_cast<size_t>(model_dim)) {
      throw std::runtime_error(
          std::format("DuckDB appender failed for {}", ImageEmbeddingTable(model_dim)));
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

// Deletes the embedding rows and the label rows of @p model_key for @p file_ids.
void DeleteEmbeddingAndLabelRows(duckdb_connection conn, int model_dim,
                                 const std::string&               model_key,
                                 std::span<const sl_element_id_t> file_ids) {
  const auto where = expr::and_(
      {expr::column_eq("model_key", model_key), expr::in_list(expr::col("file_id"), file_ids)});
  duckorm::remove(conn, ImageEmbeddingTable(model_dim), where);
  duckorm::remove(conn, "SemanticImageLabel", where);
}

auto RequireFileAndModel(const SemanticImageEmbeddingRecord& record, std::string* error) -> bool {
  if (record.file_id_ == 0) {
    SetError(error, "Semantic embedding file id is zero.");
    return false;
  }
  if (record.model_key_.empty()) {
    SetError(error, "Semantic embedding model key is empty.");
    return false;
  }
  return true;
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

SemanticEmbeddingStore::SemanticEmbeddingStore(Database& db_ctrl) : database_(db_ctrl) {}

auto SemanticEmbeddingStore::UpsertImageEmbedding(const SemanticImageEmbeddingRecord& record,
                                                  std::string* error) const -> bool {
  return UpsertImageEmbeddingWithLabel(record, nullptr, error);
}

auto SemanticEmbeddingStore::UpsertImageEmbeddingWithLabel(
    const SemanticImageEmbeddingRecord& record, const SemanticImageLabelRecord* label,
    std::string* error) const -> bool {
  auto guard   = database_.GetConnectionGuard();
  auto db_lock = guard.Lock();
  if (!RequireFileAndModel(record, error)) {
    return false;
  }
  try {
    const auto model_dim = semantic_tables::ModelEmbeddingDim(guard.conn_, record.model_key_);
    if (!model_dim.has_value()) {
      SetError(error, "Semantic model is not registered.");
      return false;
    }
    if (!ValidateEmbedding(record.embedding_, *model_dim, error) ||
        !semantic_label_assignment::ValidateLabel(record, label, error)) {
      return false;
    }

    duckorm::Transaction transaction(guard.conn_);
    DeleteEmbeddingAndLabelRows(guard.conn_, *model_dim, record.model_key_,
                                std::span<const sl_element_id_t>(&record.file_id_, 1));
    InsertReadyImageEmbedding(guard.conn_, record, *model_dim);
    if (label) {
      semantic_label_assignment::InsertLabel(guard.conn_, *label);
    }
    transaction.commit();
    return true;
  } catch (const std::exception& e) {
    SetError(error, e.what());
    return false;
  }
}

auto SemanticEmbeddingStore::UpsertImageEmbeddingAndAssignLabel(
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
  if (!RequireFileAndModel(record, error)) {
    return false;
  }
  SemanticImageLabelRecord label;
  try {
    const auto model_dim = semantic_tables::ModelEmbeddingDim(guard.conn_, record.model_key_);
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
    label =
        semantic_label_assignment::AssignLabel(guard.conn_, record, *model_dim, assignment_options);
    if (!semantic_label_assignment::ValidateLabel(record, &label, error)) {
      return false;
    }
    semantic_label_assignment::InsertLabel(guard.conn_, label);
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

auto SemanticEmbeddingStore::UpsertImageEmbeddingsAndAssignLabels(
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
    semantic_label_assignment::ValidateOptions(assignment_options);
    if (model_key.empty()) {
      SetError(error, "Semantic embedding model key is empty.");
      return false;
    }
    const auto model_dim = semantic_tables::ModelEmbeddingDim(guard.conn_, model_key);
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
    AppendImageEmbeddings(guard.conn_, records, *model_dim);
    labels = semantic_label_assignment::AssignLabels(guard.conn_, records, file_ids, model_key,
                                                     *model_dim, assignment_options);
    for (size_t i = 0; i < records.size(); ++i) {
      if (!semantic_label_assignment::ValidateLabel(records[i], &labels[i], error)) {
        return false;
      }
    }
    semantic_label_assignment::AppendLabels(guard.conn_, labels);
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

auto SemanticEmbeddingStore::CountImageEmbeddings(const std::string& model_key) const -> size_t {
  auto guard   = database_.GetConnectionGuard();
  auto db_lock = guard.Lock();
  try {
    const auto model_dim = semantic_tables::ModelEmbeddingDim(guard.conn_, model_key);
    if (!model_dim.has_value()) {
      return 0;
    }
    auto query =
        expr::raw(std::format("SELECT COUNT(*) FROM {} WHERE ", ImageEmbeddingTable(*model_dim)));
    query.append(expr::column_eq("model_key", model_key));
    return semantic_tables::CountOrZero(guard.conn_, query);
  } catch (const std::exception&) {
    return 0;
  }
}

auto SemanticEmbeddingStore::CountImageEmbeddingsForFile(sl_element_id_t    file_id,
                                                         const std::string& model_key) const
    -> size_t {
  auto guard   = database_.GetConnectionGuard();
  auto db_lock = guard.Lock();
  try {
    const auto model_dim = semantic_tables::ModelEmbeddingDim(guard.conn_, model_key);
    if (!model_dim.has_value()) {
      return 0;
    }
    auto query =
        expr::raw(std::format("SELECT COUNT(*) FROM {} WHERE ", ImageEmbeddingTable(*model_dim)));
    query.append(expr::and_({expr::column_eq("file_id", static_cast<int64_t>(file_id)),
                             expr::column_eq("model_key", model_key)}));
    return semantic_tables::CountOrZero(guard.conn_, query);
  } catch (const std::exception&) {
    return 0;
  }
}

auto SemanticEmbeddingStore::HasReadyImageEmbedding(sl_element_id_t file_id, image_id_t image_id,
                                                    const std::string& model_key,
                                                    bool               require_label) const
    -> bool {
  auto guard   = database_.GetConnectionGuard();
  auto db_lock = guard.Lock();
  try {
    const auto model_dim = semantic_tables::ModelEmbeddingDim(guard.conn_, model_key);
    if (!model_dim.has_value()) {
      return false;
    }
    auto query = expr::raw(std::format(
        "SELECT COUNT(*) FROM {} se {}WHERE ", ImageEmbeddingTable(*model_dim),
        require_label ? "JOIN SemanticImageLabel sl ON sl.file_id = se.file_id AND "
                        "sl.model_key = se.model_key "
                      : ""));
    query.append(expr::and_({expr::column_eq("se.file_id", static_cast<int64_t>(file_id)),
                             expr::column_eq("se.image_id", static_cast<int64_t>(image_id)),
                             expr::column_eq("se.model_key", model_key),
                             expr::column_eq("se.embedding_dim", static_cast<int64_t>(*model_dim)),
                             expr::raw("se.status = 'ready' AND se.error IS NULL")}));
    if (require_label) {
      query.append(expr::raw(" AND sl.label IS NOT NULL AND sl.label <> ''"));
    }
    return semantic_tables::CountOrZero(guard.conn_, query) > 0;
  } catch (const std::exception&) {
    return false;
  }
}

}  // namespace alcedo
