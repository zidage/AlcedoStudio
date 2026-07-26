//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "storage/service/sleeve/edit_history/commit_graph_service.hpp"

#include <format>
#include <stdexcept>
#include <string_view>
#include <utility>

#include "storage/mapper/duckorm/duckdb_orm.hpp"

namespace alcedo {
namespace {

auto MakeStringPtr(std::string value) -> std::unique_ptr<std::string> {
  // Always allocate a string (possibly empty). DuckORM's select path constructs
  // std::string from duckdb_value_varchar without a null check, so SQL NULL is unsafe.
  return std::make_unique<std::string>(std::move(value));
}

auto MakeSerializedPipelineState(const root_id_t& root_id, const nlohmann::json& pipeline_params)
    -> nlohmann::json {
  return nlohmann::json{{"state_format_version", 1},
                        {"root_id", root_id.ToString()},
                        {"head_commit_hash", ""},
                        {"transaction_chain_hash", ComputeRootChainHash(root_id).ToString()},
                        {"pipeline_params", pipeline_params}};
}

auto MakeRootSerializedPipelineState(const nlohmann::json&                pipeline_params,
                                     const std::optional<nlohmann::json>& raw_color_context)
    -> nlohmann::json {
  nlohmann::json state{{"state_format_version", 1}, {"pipeline_params", pipeline_params}};
  state["raw_color_context"] = raw_color_context.value_or(nullptr);
  return state;
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
    const std::string message = error ? error : "CommitGraphService query failed";
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
    throw std::runtime_error(error ? error : "CommitGraphService count query failed");
  }
  std::uint64_t value = 0;
  if (duckdb_row_count(&result) > 0) {
    value = static_cast<std::uint64_t>(duckdb_value_int64(&result, 0, 0));
  }
  duckdb_destroy_result(&result);
  return value;
}

}  // namespace

CommitGraphService::CommitGraphService(duckdb_connection& conn)
    : conn_(conn),
      commit_mapper_(conn),
      version_ref_mapper_(conn),
      image_edit_state_mapper_(conn) {}

auto CommitGraphService::ToCommitParams(const EditCommit& commit) -> EditCommitMapperParams {
  EditCommitMapperParams params;
  params.commit_hash        = MakeStringPtr(commit.GetCommitHash().ToString());
  params.root_id            = MakeStringPtr(commit.GetRootId().ToString());
  params.first_parent_hash  = MakeStringPtr(HeadCommitHashToStorage(commit.GetFirstParentHash()));
  params.second_parent_hash = MakeStringPtr(commit.GetSecondParentHash().has_value()
                                                ? commit.GetSecondParentHash()->ToString()
                                                : std::string{});
  params.created_at_ns      = commit.GetCreatedAtNs();
  params.kind               = static_cast<std::uint32_t>(commit.GetKind());
  params.edit_payload       = MakeStringPtr(commit.GetPayloadJSON().dump());
  return params;
}

auto CommitGraphService::FromCommitParams(EditCommitMapperParams&& params) -> EditCommit {
  nlohmann::json j;
  j["commit_hash"]        = params.commit_hash ? *params.commit_hash : std::string{};
  j["root_id"]            = params.root_id ? *params.root_id : std::string{};
  j["first_parent_hash"]  = params.first_parent_hash ? *params.first_parent_hash : std::string{};
  j["second_parent_hash"] = params.second_parent_hash ? *params.second_parent_hash : std::string{};
  j["created_at_ns"]      = params.created_at_ns;
  j["kind"]               = static_cast<int>(params.kind);
  j["edit_payload"] = nlohmann::json::parse(params.edit_payload ? *params.edit_payload : "{}");
  return EditCommit::FromJSON(j);
}

auto CommitGraphService::ToVersionRefParams(const VersionRef& ref) -> VersionRefMapperParams {
  VersionRefMapperParams params;
  params.version_id       = MakeStringPtr(ref.version_id.ToString());
  params.element_id       = ref.element_id;
  params.display_name     = MakeStringPtr(ref.display_name);
  params.head_commit_hash = MakeStringPtr(HeadCommitHashToStorage(ref.head_commit_hash));
  params.created_at_unix  = static_cast<std::int64_t>(ref.created_at);
  params.updated_at_unix  = static_cast<std::int64_t>(ref.updated_at);
  return params;
}

auto CommitGraphService::FromVersionRefParams(VersionRefMapperParams&& params) -> VersionRef {
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

auto CommitGraphService::ToImageEditStateParams(const ImageEditState& state)
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

auto CommitGraphService::FromImageEditStateParams(ImageEditStateMapperParams&& params)
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

auto CommitGraphService::InsertCommitIfAbsent(const EditCommit& commit) -> bool {
  commit.ValidateStructure();
  const auto existing = GetCommit(commit.GetCommitHash());
  if (existing.has_value()) {
    // Same content-addressed hash must mean identical canonical hash input (parents included).
    if (existing->CanonicalHashInput() != commit.CanonicalHashInput()) {
      throw std::runtime_error("CommitGraphService: commit hash collision with different content");
    }
    return false;
  }
  commit_mapper_.Insert(ToCommitParams(commit));
  return true;
}

auto CommitGraphService::GetCommit(const commit_hash_t& commit_hash) -> std::optional<EditCommit> {
  auto rows = commit_mapper_.Get(std::format("commit_hash='{}'", commit_hash.ToString()).c_str());
  if (rows.empty()) {
    return std::nullopt;
  }
  return FromCommitParams(std::move(rows.front()));
}

auto CommitGraphService::CountCommits() -> std::uint64_t {
  return QueryUint64(conn_, "SELECT COUNT(*) FROM EditCommit;");
}

auto CommitGraphService::CountCommitsForRoot(const root_id_t& root_id) -> std::uint64_t {
  return QueryUint64(conn_, std::format("SELECT COUNT(*) FROM EditCommit WHERE root_id='{}';",
                                        root_id.ToString()));
}

void CommitGraphService::UpsertVersionRef(const VersionRef& version_ref) {
  version_ref_mapper_.Update(version_ref.version_id.ToString(), ToVersionRefParams(version_ref));
}

auto CommitGraphService::GetVersionRef(const version_ref_id_t& version_id)
    -> std::optional<VersionRef> {
  auto rows =
      version_ref_mapper_.Get(std::format("version_id='{}'", version_id.ToString()).c_str());
  if (rows.empty()) {
    return std::nullopt;
  }
  return FromVersionRefParams(std::move(rows.front()));
}

auto CommitGraphService::ListVersionRefsForElement(sl_element_id_t element_id)
    -> std::vector<VersionRef> {
  auto rows = version_ref_mapper_.Get(std::format("element_id={}", element_id).c_str());
  std::vector<VersionRef> refs;
  refs.reserve(rows.size());
  for (auto& row : rows) {
    refs.push_back(FromVersionRefParams(std::move(row)));
  }
  return refs;
}

void CommitGraphService::UpsertImageEditState(const ImageEditState& state) {
  image_edit_state_mapper_.Update(state.element_id, ToImageEditStateParams(state));
}

auto CommitGraphService::GetImageEditState(sl_element_id_t element_id)
    -> std::optional<ImageEditState> {
  auto rows = image_edit_state_mapper_.Get(std::format("element_id={}", element_id).c_str());
  if (rows.empty()) {
    return std::nullopt;
  }
  return FromImageEditStateParams(std::move(rows.front()));
}

void CommitGraphService::InsertRootSerializedPipelineState(
    const root_id_t& root_id, sl_element_id_t element_id,
    const nlohmann::json& serialized_pipeline_state) {
  ExecuteOrThrow(conn_, std::format("INSERT INTO PipelineRoot "
                                    "(root_id, element_id, serialized_pipeline_state) "
                                    "VALUES ({}, {}, CAST({} AS JSON));",
                                    SqlQuote(root_id.ToString()), element_id,
                                    SqlQuote(serialized_pipeline_state.dump())));
}

auto CommitGraphService::GetRootSerializedPipelineState(sl_element_id_t  element_id,
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
    const std::string message = error ? error : "CommitGraphService root state query failed";
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
    throw std::runtime_error("CommitGraphService: root belongs to a different image");
  }
  char*             raw     = duckdb_value_varchar(&result, 1, 0);
  const std::string encoded = raw ? raw : "";
  if (raw) {
    duckdb_free(raw);
  }
  duckdb_destroy_result(&result);
  if (encoded.empty()) {
    throw std::runtime_error("CommitGraphService: root serialized pipeline state is empty");
  }
  return nlohmann::json::parse(encoded);
}

void CommitGraphService::Materialize(const CommitGraphMaterialization& materialization) {
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

auto CommitGraphService::LoadGraph(sl_element_id_t element_id) -> std::optional<CommitGraph> {
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

auto CommitGraphService::ListImageElementIds() -> std::vector<sl_element_id_t> {
  auto                         rows = image_edit_state_mapper_.Get("1=1");
  std::vector<sl_element_id_t> ids;
  ids.reserve(rows.size());
  for (auto& row : rows) {
    ids.push_back(row.element_id);
  }
  return ids;
}

auto CommitGraphService::DeleteUnreachableCommits(sl_element_id_t element_id) -> std::size_t {
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

auto CommitGraphService::DeleteUnreachableCommitsForProject() -> std::size_t {
  std::size_t total = 0;
  for (const auto element_id : ListImageElementIds()) {
    total += DeleteUnreachableCommits(element_id);
  }
  return total;
}

void CommitGraphService::DeleteGraphForElement(sl_element_id_t element_id) {
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

auto CommitGraphService::CreateEmptyPersisted(sl_element_id_t element_id,
                                              std::string     default_display_name) -> CommitGraph {
  auto graph           = CommitGraph::CreateEmpty(element_id, std::move(default_display_name));
  auto materialization = graph.CaptureMaterialization();
  Materialize(materialization);
  graph.ApplyMaterializedState(materialization.image_state);
  return graph;
}

auto CommitGraphService::CreateRootPipelinePersisted(
    sl_element_id_t element_id, const nlohmann::json& root_pipeline_params,
    std::optional<nlohmann::json> raw_color_context, std::string default_display_name)
    -> CommitGraph {
  if (GetImageEditState(element_id).has_value()) {
    throw std::runtime_error("CommitGraphService: image root already exists");
  }

  auto graph           = CommitGraph::CreateEmpty(element_id, std::move(default_display_name));
  auto materialization = graph.CaptureMaterializationWithSerializedPipelineState(
      MakeSerializedPipelineState(graph.GetRootId(), root_pipeline_params));
  materialization.Validate();

  duckorm::begin_transaction(conn_);
  try {
    InsertRootSerializedPipelineState(
        graph.GetRootId(), element_id,
        MakeRootSerializedPipelineState(root_pipeline_params, raw_color_context));
    for (const auto& ref : materialization.version_refs) {
      UpsertVersionRef(ref);
    }
    UpsertImageEditState(materialization.image_state);
    duckorm::commit_transaction(conn_);
  } catch (...) {
    duckorm::rollback_transaction(conn_);
    throw;
  }

  graph.ApplyMaterializedState(materialization.image_state);
  return graph;
}

}  // namespace alcedo
