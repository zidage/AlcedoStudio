//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "storage/store/edit_history/commit_graph_store.hpp"

#include <format>
#include <stdexcept>
#include <string_view>
#include <utility>

#include "edit/history/edit_commit.hpp"
#include "edit/history/pipeline_document_checkpoint.hpp"
#include "storage/mapper/duckorm/duckdb_orm.hpp"

namespace alcedo {
namespace {

auto MakeStringPtr(std::string value) -> std::unique_ptr<std::string> {
  // Always allocate a string (possibly empty). DuckORM's select path constructs
  // std::string from duckdb_value_varchar without a null check, so SQL NULL is unsafe.
  return std::make_unique<std::string>(std::move(value));
}

auto SqlQuote(std::string_view value) -> std::string {
  std::string quoted;
  quoted.reserve(value.size() + 2);
  quoted.push_back('\'');
  for (const char ch : value) {
    if (ch == '\'') {
      quoted.push_back('\'');
    }
    quoted.push_back(ch);
  }
  quoted.push_back('\'');
  return quoted;
}

void ExecuteOrThrow(duckdb_connection conn, const std::string& sql) {
  duckdb_result result;
  if (duckdb_query(conn, sql.c_str(), &result) != DuckDBSuccess) {
    const char*       error   = duckdb_result_error(&result);
    const std::string message = error ? error : "CommitGraphStore query failed";
    duckdb_destroy_result(&result);
    throw std::runtime_error(message);
  }
  duckdb_destroy_result(&result);
}

auto QueryUint64(duckdb_connection conn, const std::string& sql) -> std::uint64_t {
  duckdb_result result;
  if (duckdb_query(conn, sql.c_str(), &result) != DuckDBSuccess) {
    const char* error = duckdb_result_error(&result);
    duckdb_destroy_result(&result);
    throw std::runtime_error(error ? error : "CommitGraphStore count query failed");
  }
  std::uint64_t value = 0;
  if (duckdb_row_count(&result) > 0) {
    value = static_cast<std::uint64_t>(duckdb_value_int64(&result, 0, 0));
  }
  duckdb_destroy_result(&result);
  return value;
}

}  // namespace

CommitGraphStore::CommitGraphStore(duckdb_connection& conn)
    : conn_(conn),
      commit_mapper_(conn),
      version_ref_mapper_(conn),
      image_edit_state_mapper_(conn) {}

auto CommitGraphStore::ToCommitParams(const EditCommit& commit) -> EditCommitMapperParams {
  EditCommitMapperParams params;
  params.commit_hash        = MakeStringPtr(commit.GetCommitHash().ToString());
  params.root_id            = MakeStringPtr(commit.GetRootId().ToString());
  params.first_parent_hash  = MakeStringPtr(HeadCommitHashToStorage(commit.GetFirstParentHash()));
  params.second_parent_hash = MakeStringPtr("");
  params.created_at_ns      = commit.GetCreatedAtNs();
  params.kind               = 0;
  params.edit_payload       = MakeStringPtr(commit.GetPayloadJSON().dump());
  return params;
}

auto CommitGraphStore::FromCommitParams(EditCommitMapperParams&& params) -> EditCommit {
  if (params.kind != 0) {
    throw std::runtime_error("CommitGraphStore::FromCommitParams: non-edit kind is not supported");
  }
  if (params.second_parent_hash && !params.second_parent_hash->empty()) {
    throw std::runtime_error(
        "CommitGraphStore::FromCommitParams: non-empty second parent is not supported");
  }
  nlohmann::json j;
  j["commit_hash"]        = params.commit_hash ? *params.commit_hash : std::string{};
  j["root_id"]            = params.root_id ? *params.root_id : std::string{};
  j["first_parent_hash"]  = params.first_parent_hash ? *params.first_parent_hash : std::string{};
  j["second_parent_hash"] = std::string{};
  j["created_at_ns"]      = params.created_at_ns;
  j["kind"]               = "edit";
  j["edit_payload"] = nlohmann::json::parse(params.edit_payload ? *params.edit_payload : "{}");
  return EditCommit::FromJSON(j);
}

auto CommitGraphStore::ToVersionRefParams(const VersionRef& ref) -> VersionRefMapperParams {
  VersionRefMapperParams params;
  params.version_id       = MakeStringPtr(ref.version_id.ToString());
  params.element_id       = ref.element_id;
  params.display_name     = MakeStringPtr(ref.display_name);
  params.head_commit_hash = MakeStringPtr(HeadCommitHashToStorage(ref.head_commit_hash));
  params.created_at_unix  = static_cast<std::int64_t>(ref.created_at);
  params.updated_at_unix  = static_cast<std::int64_t>(ref.updated_at);
  return params;
}

auto CommitGraphStore::FromVersionRefParams(VersionRefMapperParams&& params) -> VersionRef {
  VersionRef ref;
  ref.version_id   = Hash128::FromString(params.version_id ? *params.version_id : std::string{});
  ref.element_id   = params.element_id;
  ref.display_name = params.display_name ? *params.display_name : std::string{};
  ref.head_commit_hash =
      HeadCommitHashFromStorage(params.head_commit_hash ? *params.head_commit_hash : std::string{});
  ref.created_at = static_cast<std::time_t>(params.created_at_unix);
  ref.updated_at = static_cast<std::time_t>(params.updated_at_unix);
  return ref;
}

auto CommitGraphStore::ToImageEditStateParams(const ImageEditState& state)
    -> ImageEditStateMapperParams {
  ImageEditStateMapperParams params;
  params.element_id        = state.element_id;
  params.root_id           = MakeStringPtr(state.root_id.ToString());
  params.active_version_id = MakeStringPtr(state.active_version_id.ToString());
  params.materialized_head_commit_hash =
      MakeStringPtr(HeadCommitHashToStorage(state.materialized_head_commit_hash));
  params.materialized_transaction_chain_hash =
      MakeStringPtr(state.materialized_transaction_chain_hash.ToString());
  if (state.serialized_pipeline_state.has_value()) {
    params.serialized_pipeline_state = MakeStringPtr(state.serialized_pipeline_state->dump());
  } else {
    params.serialized_pipeline_state = MakeStringPtr(std::string{"null"});
  }
  params.project_schema_version = state.project_schema_version;
  return params;
}

auto CommitGraphStore::FromImageEditStateParams(ImageEditStateMapperParams&& params)
    -> ImageEditState {
  ImageEditState state;
  state.element_id = params.element_id;
  state.root_id    = Hash128::FromString(params.root_id ? *params.root_id : std::string{});
  state.active_version_id =
      Hash128::FromString(params.active_version_id ? *params.active_version_id : std::string{});
  state.materialized_head_commit_hash = HeadCommitHashFromStorage(
      params.materialized_head_commit_hash ? *params.materialized_head_commit_hash : std::string{});
  state.materialized_transaction_chain_hash = Hash128::FromString(
      params.materialized_transaction_chain_hash ? *params.materialized_transaction_chain_hash
                                                 : std::string{});
  if (params.serialized_pipeline_state && !params.serialized_pipeline_state->empty() &&
      *params.serialized_pipeline_state != "null") {
    state.serialized_pipeline_state = nlohmann::json::parse(*params.serialized_pipeline_state);
  }
  state.project_schema_version = params.project_schema_version;
  return state;
}

auto CommitGraphStore::InsertCommitIfAbsent(const EditCommit& commit) -> bool {
  commit.ValidateStructure();
  const auto existing = GetCommit(commit.GetCommitHash());
  if (existing.has_value()) {
    // Same content-addressed hash must mean identical canonical hash input (parents included).
    if (existing->CanonicalHashInput() != commit.CanonicalHashInput()) {
      throw std::runtime_error("CommitGraphStore: commit hash collision with different content");
    }
    return false;
  }
  commit_mapper_.Insert(ToCommitParams(commit));
  return true;
}

auto CommitGraphStore::GetCommit(const commit_hash_t& commit_hash) -> std::optional<EditCommit> {
  auto rows = commit_mapper_.Get(std::format("commit_hash='{}'", commit_hash.ToString()).c_str());
  if (rows.empty()) {
    return std::nullopt;
  }
  return FromCommitParams(std::move(rows.front()));
}

auto CommitGraphStore::CountCommits() -> std::uint64_t {
  return QueryUint64(conn_, "SELECT COUNT(*) FROM EditCommit;");
}

auto CommitGraphStore::CountCommitsForRoot(const root_id_t& root_id) -> std::uint64_t {
  return QueryUint64(conn_, std::format("SELECT COUNT(*) FROM EditCommit WHERE root_id='{}';",
                                        root_id.ToString()));
}

void CommitGraphStore::UpsertVersionRef(const VersionRef& version_ref) {
  version_ref_mapper_.Update(version_ref.version_id.ToString(), ToVersionRefParams(version_ref));
}

auto CommitGraphStore::GetVersionRef(const version_ref_id_t& version_id)
    -> std::optional<VersionRef> {
  auto rows =
      version_ref_mapper_.Get(std::format("version_id='{}'", version_id.ToString()).c_str());
  if (rows.empty()) {
    return std::nullopt;
  }
  return FromVersionRefParams(std::move(rows.front()));
}

auto CommitGraphStore::ListVersionRefsForElement(sl_element_id_t element_id)
    -> std::vector<VersionRef> {
  auto rows = version_ref_mapper_.Get(std::format("element_id={}", element_id).c_str());
  std::vector<VersionRef> refs;
  refs.reserve(rows.size());
  for (auto& row : rows) {
    refs.push_back(FromVersionRefParams(std::move(row)));
  }
  return refs;
}

void CommitGraphStore::UpsertImageEditState(const ImageEditState& state) {
  image_edit_state_mapper_.Update(state.element_id, ToImageEditStateParams(state));
}

auto CommitGraphStore::GetImageEditState(sl_element_id_t element_id)
    -> std::optional<ImageEditState> {
  auto rows = image_edit_state_mapper_.Get(std::format("element_id={}", element_id).c_str());
  if (rows.empty()) {
    return std::nullopt;
  }
  return FromImageEditStateParams(std::move(rows.front()));
}

auto CommitGraphStore::GetMaterializedHistoryLabel(sl_element_id_t element_id)
    -> std::optional<MaterializedHistoryLabel> {
  duckdb_result result;
  const auto    sql = std::format(
      "SELECT root_id, materialized_head_commit_hash, materialized_transaction_chain_hash "
         "FROM ImageEditState WHERE element_id={};",
      element_id);
  if (duckdb_query(conn_, sql.c_str(), &result) != DuckDBSuccess) {
    const char*       error   = duckdb_result_error(&result);
    const std::string message = error ? error : "CommitGraphStore history label query failed";
    duckdb_destroy_result(&result);
    throw std::runtime_error(message);
  }
  if (duckdb_row_count(&result) == 0) {
    duckdb_destroy_result(&result);
    return std::nullopt;
  }
  const auto read_text = [&result](idx_t column) -> std::string {
    if (duckdb_value_is_null(&result, column, 0)) {
      return {};
    }
    char*       raw  = duckdb_value_varchar(&result, column, 0);
    std::string text = raw ? raw : "";
    if (raw) {
      duckdb_free(raw);
    }
    return text;
  };
  MaterializedHistoryLabel label;
  label.root_id                = Hash128::FromString(read_text(0));
  label.head_commit_hash       = HeadCommitHashFromStorage(read_text(1));
  label.transaction_chain_hash = Hash128::FromString(read_text(2));
  duckdb_destroy_result(&result);
  return label;
}

void CommitGraphStore::InsertRootSerializedPipelineStates(
    std::span<const EncodedRootPipeline> roots) {
  std::string sql =
      "INSERT INTO PipelineRoot (root_id, element_id, serialized_pipeline_state) VALUES ";
  for (std::size_t index = 0; index < roots.size(); ++index) {
    sql += index == 0 ? "(?, ?, CAST(? AS JSON))" : ", (?, ?, CAST(? AS JSON))";
  }
  sql += ";";

  duckdb_prepared_statement statement = nullptr;
  if (duckdb_prepare(conn_, sql.c_str(), &statement) != DuckDBSuccess) {
    const char*       error   = duckdb_prepare_error(statement);
    const std::string message = error ? error : "CommitGraphStore root insert prepare failed";
    duckdb_destroy_prepare(&statement);
    throw std::runtime_error(message);
  }
  std::vector<std::string> root_ids;
  root_ids.reserve(roots.size());
  idx_t parameter = 1;
  for (const auto& root : roots) {
    root_ids.push_back(root.root_id.ToString());
    duckdb_bind_varchar(statement, parameter++, root_ids.back().c_str());
    duckdb_bind_int64(statement, parameter++, static_cast<int64_t>(root.element_id));
    duckdb_bind_varchar_length(statement, parameter++, root.root_state_json.data(),
                               static_cast<idx_t>(root.root_state_json.size()));
  }
  duckdb_result result;
  if (duckdb_execute_prepared(statement, &result) != DuckDBSuccess) {
    const char*       error   = duckdb_result_error(&result);
    const std::string message = error ? error : "CommitGraphStore root insert failed";
    duckdb_destroy_result(&result);
    duckdb_destroy_prepare(&statement);
    throw std::runtime_error(message);
  }
  duckdb_destroy_result(&result);
  duckdb_destroy_prepare(&statement);
}

auto CommitGraphStore::GetRootSerializedPipelineState(sl_element_id_t  element_id,
                                                        const root_id_t& root_id)
    -> std::optional<nlohmann::json> {
  duckdb_result result;
  const auto    sql = std::format(
      "SELECT element_id, serialized_pipeline_state::VARCHAR "
         "FROM PipelineRoot "
         "WHERE root_id={};",
      SqlQuote(root_id.ToString()));
  if (duckdb_query(conn_, sql.c_str(), &result) != DuckDBSuccess) {
    const char*       error   = duckdb_result_error(&result);
    const std::string message = error ? error : "CommitGraphStore root state query failed";
    duckdb_destroy_result(&result);
    throw std::runtime_error(message);
  }
  if (duckdb_row_count(&result) == 0) {
    duckdb_destroy_result(&result);
    return std::nullopt;
  }
  const auto stored_element_id = static_cast<sl_element_id_t>(duckdb_value_int64(&result, 0, 0));
  if (stored_element_id != element_id) {
    duckdb_destroy_result(&result);
    throw std::runtime_error("CommitGraphStore: root belongs to a different image");
  }
  char*             raw     = duckdb_value_varchar(&result, 1, 0);
  const std::string encoded = raw ? raw : "";
  if (raw) {
    duckdb_free(raw);
  }
  duckdb_destroy_result(&result);
  if (encoded.empty()) {
    throw std::runtime_error("CommitGraphStore: root serialized pipeline state is empty");
  }
  return nlohmann::json::parse(encoded);
}

void CommitGraphStore::Materialize(const CommitGraphMaterialization& materialization) {
  // Validate fully before any DuckDB write so a bad capture leaves prior rows unchanged.
  materialization.Validate();

  duckorm::begin_transaction(conn_);
  try {
    for (const auto& commit : materialization.commits) {
      InsertCommitIfAbsent(commit);
    }
    std::string keep_version_ids;
    for (std::size_t index = 0; index < materialization.version_refs.size(); ++index) {
      if (index != 0) keep_version_ids += ", ";
      keep_version_ids += SqlQuote(materialization.version_refs[index].version_id.ToString());
    }
    ExecuteOrThrow(
        conn_, std::format("DELETE FROM VersionRef WHERE element_id={} AND version_id NOT IN ({});",
                           materialization.image_state.element_id, keep_version_ids));
    for (const auto& ref : materialization.version_refs) {
      UpsertVersionRef(ref);
    }
    UpsertImageEditState(materialization.image_state);
    duckorm::commit_transaction(conn_);
  } catch (...) {
    duckorm::rollback_transaction(conn_);
    throw;
  }
}

auto CommitGraphStore::LoadGraph(sl_element_id_t element_id) -> std::optional<CommitGraph> {
  auto state = GetImageEditState(element_id);
  if (!state.has_value()) {
    return std::nullopt;
  }

  auto refs = ListVersionRefsForElement(element_id);
  auto commit_rows =
      commit_mapper_.Get(std::format("root_id='{}'", state->root_id.ToString()).c_str());
  std::vector<EditCommit> commits;
  commits.reserve(commit_rows.size());
  for (auto& row : commit_rows) {
    commits.push_back(FromCommitParams(std::move(row)));
  }

  // FromParts validates structure, parent rules, and materialized head/chain agreement.
  return CommitGraph::FromParts(std::move(*state), std::move(refs), std::move(commits));
}

auto CommitGraphStore::ListImageElementIds() -> std::vector<sl_element_id_t> {
  auto                         rows = image_edit_state_mapper_.Get("1=1");
  std::vector<sl_element_id_t> ids;
  ids.reserve(rows.size());
  for (auto& row : rows) {
    ids.push_back(row.element_id);
  }
  return ids;
}

auto CommitGraphStore::DeleteUnreachableCommits(sl_element_id_t element_id) -> std::size_t {
  auto graph = LoadGraph(element_id);
  if (!graph.has_value()) {
    return 0;
  }
  const auto unreachable = graph->ListUnreachableCommitHashes();
  if (unreachable.empty()) {
    return 0;
  }

  duckorm::begin_transaction(conn_);
  try {
    for (const auto& hash : unreachable) {
      commit_mapper_.Remove(hash.ToString());
    }
    duckorm::commit_transaction(conn_);
  } catch (...) {
    duckorm::rollback_transaction(conn_);
    throw;
  }
  return unreachable.size();
}

auto CommitGraphStore::DeleteUnreachableCommitsForProject() -> std::size_t {
  std::size_t total = 0;
  for (const auto element_id : ListImageElementIds()) {
    total += DeleteUnreachableCommits(element_id);
  }
  return total;
}

void CommitGraphStore::DeleteGraphForElement(sl_element_id_t element_id) {
  const auto state = GetImageEditState(element_id);
  if (!state.has_value()) {
    return;
  }

  duckorm::begin_transaction(conn_);
  try {
    version_ref_mapper_.RemoveByClause(std::format("element_id={}", element_id));
    commit_mapper_.RemoveByClause(std::format("root_id={}", SqlQuote(state->root_id.ToString())));
    image_edit_state_mapper_.Remove(element_id);
    ExecuteOrThrow(conn_, std::format("DELETE FROM PipelineRoot WHERE element_id={};", element_id));
    duckorm::commit_transaction(conn_);
  } catch (...) {
    duckorm::rollback_transaction(conn_);
    throw;
  }
}

auto CommitGraphStore::CreateEmptyPersisted(sl_element_id_t element_id,
                                              std::string     default_display_name) -> CommitGraph {
  auto graph           = CommitGraph::CreateEmpty(element_id, std::move(default_display_name));
  auto materialization = graph.CaptureMaterialization();
  Materialize(materialization);
  graph.ApplyMaterializedState(materialization.image_state);
  return graph;
}

auto CommitGraphStore::CreateRootPipelinePersisted(
    sl_element_id_t element_id, const PipelineDocument& root_document,
    std::optional<nlohmann::json> raw_color_context, std::string default_display_name)
    -> CommitGraph {
  const auto root_id = ComputeRootId(element_id, root_document, raw_color_context);
  auto       graph =
      CommitGraph::CreateEmptyWithRootId(element_id, root_id, std::move(default_display_name));
  const auto encoded = EncodeRootPipelineForGraph(graph, root_document, raw_color_context);
  InsertRootPipelines(std::span<const EncodedRootPipeline>(&encoded, 1));
  graph.ApplyMaterializedState(encoded.materialization.image_state);
  return graph;
}

auto CommitGraphStore::EncodeRootPipeline(sl_element_id_t                      element_id,
                                          const PipelineDocument&              root_document,
                                          const std::optional<nlohmann::json>& raw_color_context,
                                          std::string default_display_name) -> EncodedRootPipeline {
  const auto root_id = ComputeRootId(element_id, root_document, raw_color_context);
  const auto graph =
      CommitGraph::CreateEmptyWithRootId(element_id, root_id, std::move(default_display_name));
  return EncodeRootPipelineForGraph(graph, root_document, raw_color_context);
}

auto CommitGraphStore::EncodeRootPipelineForGraph(
    const CommitGraph& graph, const PipelineDocument& root_document,
    const std::optional<nlohmann::json>& raw_color_context) -> EncodedRootPipeline {
  const auto& state      = graph.GetImageEditState();
  const auto  root_id    = graph.GetRootId();
  const auto  checkpoint = EncodePipelineDocumentCheckpoint(
      root_id, std::nullopt, ComputeRootChainHash(root_id), root_document);
  EncodedRootPipeline encoded;
  encoded.element_id = state.element_id;
  encoded.root_id    = root_id;
  encoded.root_state_json =
      EncodePipelineRootState(state.element_id, root_document, raw_color_context).dump();
  encoded.materialization = graph.CaptureMaterializationWithSerializedPipelineState(checkpoint);
  encoded.materialization.Validate();
  return encoded;
}

void CommitGraphStore::InsertRootPipelines(std::span<const EncodedRootPipeline> roots,
                                           const std::function<void()>&         write_rows) {
  if (roots.empty()) {
    return;
  }
  // One existence query for the whole batch: the stored root of an image is never replaced.
  std::string element_ids;
  for (std::size_t index = 0; index < roots.size(); ++index) {
    if (index != 0) element_ids += ",";
    element_ids += std::to_string(roots[index].element_id);
  }
  if (QueryUint64(conn_,
                  std::format("SELECT COUNT(*) FROM ImageEditState WHERE element_id IN ({});",
                              element_ids)) != 0) {
    throw std::runtime_error("CommitGraphStore: image root already exists");
  }

  // One multi-row statement per table: a prepare and execute per row made the database lock the
  // import bottleneck.
  std::vector<VersionRefMapperParams>     version_ref_rows;
  std::vector<ImageEditStateMapperParams> image_edit_state_rows;
  image_edit_state_rows.reserve(roots.size());
  for (const auto& root : roots) {
    for (const auto& ref : root.materialization.version_refs) {
      version_ref_rows.push_back(ToVersionRefParams(ref));
    }
    image_edit_state_rows.push_back(ToImageEditStateParams(root.materialization.image_state));
  }

  duckorm::begin_transaction(conn_);
  try {
    InsertRootSerializedPipelineStates(roots);
    version_ref_mapper_.UpsertParamsRows(version_ref_rows);
    image_edit_state_mapper_.UpsertParamsRows(image_edit_state_rows);
    if (write_rows) {
      write_rows();
    }
    duckorm::commit_transaction(conn_);
  } catch (...) {
    duckorm::rollback_transaction(conn_);
    throw;
  }
}

}  // namespace alcedo
