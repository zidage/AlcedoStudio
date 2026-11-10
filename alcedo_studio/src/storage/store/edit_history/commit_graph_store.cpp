//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "storage/store/edit_history/commit_graph_store.hpp"

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "edit/history/edit_commit.hpp"
#include "edit/history/pipeline_document_checkpoint.hpp"
#include "storage/mapper/duckorm/duckdb_expr.hpp"
#include "storage/mapper/duckorm/duckdb_orm.hpp"
#include "storage/mapper/duckorm/duckdb_statement.hpp"

namespace alcedo {
namespace {

namespace expr = duckorm::expr;

auto MakeStringPtr(std::string value) -> std::unique_ptr<std::string> {
  // Always allocate a string (possibly empty). DuckORM's select path constructs
  // std::string from the cell text without a null check, so SQL NULL is unsafe.
  return std::make_unique<std::string>(std::move(value));
}

/// History labels of one ImageEditState row (GetMaterializedHistoryLabel).
struct MaterializedHistoryLabelRow {
  std::string                root_id;
  std::optional<std::string> materialized_head_commit_hash;
  std::string                materialized_transaction_chain_hash;

  using Columns =
      duckorm::RowColumns<&MaterializedHistoryLabelRow::root_id,
                          &MaterializedHistoryLabelRow::materialized_head_commit_hash,
                          &MaterializedHistoryLabelRow::materialized_transaction_chain_hash>;
};

/// Owner and document of one PipelineRoot row (GetRootSerializedPipelineState).
struct RootPipelineStateRow {
  std::int64_t element_id = 0;
  std::string  serialized_pipeline_state;

  using Columns = duckorm::RowColumns<&RootPipelineStateRow::element_id,
                                      &RootPipelineStateRow::serialized_pipeline_state>;
};

/// One EditCommitAttribution row (ListAttributionsForRoot). `note` is NULL when empty.
struct AttributionRow {
  std::string                commit_hash;
  std::string                author;
  std::optional<std::string> note;

  using Columns = duckorm::RowColumns<&AttributionRow::commit_hash, &AttributionRow::author,
                                      &AttributionRow::note>;
};

/// The element id of one ImageEditState row (ListImageElementIds).
struct ImageEditStateIdRow {
  std::int64_t element_id = 0;

  using Columns           = duckorm::RowColumns<&ImageEditStateIdRow::element_id>;
};

/// The text of one stored history JSON value (VisitStoredHistoryJsonContaining).
struct StoredHistoryJsonRow {
  std::string json;

  using Columns = duckorm::RowColumns<&StoredHistoryJsonRow::json>;
};

auto ElementIdEquals(sl_element_id_t element_id) -> duckorm::SqlFragment {
  return expr::column_eq("element_id", static_cast<std::int64_t>(element_id));
}

auto HashStrings(std::span<const commit_hash_t> hashes) -> std::vector<std::string> {
  std::vector<std::string> strings;
  strings.reserve(hashes.size());
  for (const auto& hash : hashes) {
    strings.push_back(hash.ToString());
  }
  return strings;
}

}  // namespace

CommitGraphStore::CommitGraphStore(duckorm::Connection& conn)
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
  auto row = commit_mapper_.GetById(commit_hash.ToString());
  if (!row.has_value()) {
    return std::nullopt;
  }
  return FromCommitParams(std::move(*row));
}

auto CommitGraphStore::CountCommits() -> std::uint64_t {
  const auto count =
      duckorm::scalar<std::int64_t>(conn_, expr::raw("SELECT COUNT(*) FROM EditCommit"));
  return static_cast<std::uint64_t>(count.value_or(0));
}

auto CommitGraphStore::CountCommitsForRoot(const root_id_t& root_id) -> std::uint64_t {
  auto query = expr::raw("SELECT COUNT(*) FROM EditCommit WHERE ");
  query.append(expr::column_eq("root_id", root_id.ToString()));
  return static_cast<std::uint64_t>(duckorm::scalar<std::int64_t>(conn_, query).value_or(0));
}

void CommitGraphStore::UpsertVersionRef(const VersionRef& version_ref) {
  version_ref_mapper_.UpdateParams(version_ref.version_id.ToString(),
                                   ToVersionRefParams(version_ref));
}

auto CommitGraphStore::GetVersionRef(const version_ref_id_t& version_id)
    -> std::optional<VersionRef> {
  auto row = version_ref_mapper_.GetById(version_id.ToString());
  if (!row.has_value()) {
    return std::nullopt;
  }
  return FromVersionRefParams(std::move(*row));
}

auto CommitGraphStore::ListVersionRefsForElement(sl_element_id_t element_id)
    -> std::vector<VersionRef> {
  auto                    rows = version_ref_mapper_.GetWhere(ElementIdEquals(element_id));
  std::vector<VersionRef> refs;
  refs.reserve(rows.size());
  for (auto& row : rows) {
    refs.push_back(FromVersionRefParams(std::move(row)));
  }
  return refs;
}

void CommitGraphStore::UpsertImageEditState(const ImageEditState& state) {
  image_edit_state_mapper_.UpdateParams(state.element_id, ToImageEditStateParams(state));
}

auto CommitGraphStore::GetImageEditState(sl_element_id_t element_id)
    -> std::optional<ImageEditState> {
  auto row = image_edit_state_mapper_.GetById(element_id);
  if (!row.has_value()) {
    return std::nullopt;
  }
  return FromImageEditStateParams(std::move(*row));
}

auto CommitGraphStore::GetMaterializedHistoryLabel(sl_element_id_t element_id)
    -> std::optional<MaterializedHistoryLabel> {
  auto query = expr::raw(
      "SELECT root_id, materialized_head_commit_hash, materialized_transaction_chain_hash "
      "FROM ImageEditState WHERE ");
  query.append(ElementIdEquals(element_id));
  const auto row = duckorm::query_one<MaterializedHistoryLabelRow>(conn_, query);
  if (!row.has_value()) {
    return std::nullopt;
  }
  MaterializedHistoryLabel label;
  label.root_id = Hash128::FromString(row->root_id);
  label.head_commit_hash =
      HeadCommitHashFromStorage(row->materialized_head_commit_hash.value_or(std::string{}));
  label.transaction_chain_hash = Hash128::FromString(row->materialized_transaction_chain_hash);
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

  duckorm::Statement insert(conn_, sql);
  idx_t              parameter = 1;
  for (const auto& root : roots) {
    insert.Bind(parameter++, std::string_view{root.root_id.ToString()});
    insert.Bind(parameter++, static_cast<std::int64_t>(root.element_id));
    insert.Bind(parameter++, std::string_view{root.root_state_json});
  }
  (void)insert.Execute();
}

auto CommitGraphStore::GetRootSerializedPipelineState(sl_element_id_t  element_id,
                                                      const root_id_t& root_id)
    -> std::optional<nlohmann::json> {
  auto query =
      expr::raw("SELECT element_id, serialized_pipeline_state::VARCHAR FROM PipelineRoot WHERE ");
  query.append(expr::column_eq("root_id", root_id.ToString()));
  const auto row = duckorm::query_one<RootPipelineStateRow>(conn_, query);
  if (!row.has_value()) {
    return std::nullopt;
  }
  if (static_cast<sl_element_id_t>(row->element_id) != element_id) {
    throw std::runtime_error("CommitGraphStore: root belongs to a different image");
  }
  if (row->serialized_pipeline_state.empty()) {
    throw std::runtime_error("CommitGraphStore: root serialized pipeline state is empty");
  }
  return nlohmann::json::parse(row->serialized_pipeline_state);
}

void CommitGraphStore::InsertAttributionIfAbsent(const commit_hash_t&         commit_hash,
                                                 const EditCommitAttribution& attribution) {
  // A commit hash names one commit, so a stored row already holds this attribution.
  duckorm::SqlFragment insert{
      "INSERT OR IGNORE INTO EditCommitAttribution (commit_hash, author, note) VALUES (?, ?, ?);",
      {duckorm::BindValue{commit_hash.ToString()}, duckorm::BindValue{attribution.author_},
       attribution.note_.empty() ? duckorm::BindValue{} : duckorm::BindValue{attribution.note_}}};
  duckorm::execute(conn_, insert);
}

auto CommitGraphStore::ListAttributionsForRoot(const root_id_t& root_id)
    -> std::unordered_map<commit_hash_t, EditCommitAttribution> {
  auto query = expr::raw(
      "SELECT a.commit_hash, a.author, a.note FROM EditCommitAttribution a "
      "JOIN EditCommit c ON c.commit_hash = a.commit_hash WHERE ");
  query.append(expr::column_eq("c.root_id", root_id.ToString()));
  std::unordered_map<commit_hash_t, EditCommitAttribution> attributions;
  for (auto& row : duckorm::query<AttributionRow>(conn_, query)) {
    attributions.emplace(
        Hash128::FromString(row.commit_hash),
        EditCommitAttribution{std::move(row.author), std::move(row.note).value_or(std::string{})});
  }
  return attributions;
}

void CommitGraphStore::Materialize(const CommitGraphMaterialization& materialization) {
  // Validate fully before any write so a bad capture leaves prior rows unchanged.
  materialization.Validate();

  std::vector<std::string> keep_version_ids;
  keep_version_ids.reserve(materialization.version_refs.size());
  for (const auto& ref : materialization.version_refs) {
    keep_version_ids.push_back(ref.version_id.ToString());
  }

  duckorm::Transaction transaction(conn_);
  for (const auto& commit : materialization.commits) {
    InsertCommitIfAbsent(commit);
  }
  for (const auto& [hash, attribution] : materialization.attributions) {
    InsertAttributionIfAbsent(hash, attribution);
  }
  version_ref_mapper_.RemoveWhere(
      expr::and_({ElementIdEquals(materialization.image_state.element_id),
                  expr::not_(expr::in_list(expr::col("version_id"),
                                           std::span<const std::string>(keep_version_ids)))}));
  for (const auto& ref : materialization.version_refs) {
    UpsertVersionRef(ref);
  }
  UpsertImageEditState(materialization.image_state);
  transaction.commit();
}

auto CommitGraphStore::LoadGraph(sl_element_id_t element_id) -> std::optional<CommitGraph> {
  auto state = GetImageEditState(element_id);
  if (!state.has_value()) {
    return std::nullopt;
  }

  auto refs        = ListVersionRefsForElement(element_id);
  auto commit_rows = commit_mapper_.GetWhere(expr::column_eq("root_id", state->root_id.ToString()));
  std::vector<EditCommit> commits;
  commits.reserve(commit_rows.size());
  for (auto& row : commit_rows) {
    commits.push_back(FromCommitParams(std::move(row)));
  }

  const auto root_id = state->root_id;
  // FromParts validates structure, parent rules, and materialized head/chain agreement.
  auto       graph = CommitGraph::FromParts(std::move(*state), std::move(refs), std::move(commits));
  for (auto& [hash, attribution] : ListAttributionsForRoot(root_id)) {
    graph.SetCommitAttribution(hash, std::move(attribution));
  }
  return graph;
}

auto CommitGraphStore::ListImageElementIds() -> std::vector<sl_element_id_t> {
  const auto rows = duckorm::query<ImageEditStateIdRow>(
      conn_, expr::raw("SELECT element_id FROM ImageEditState"));
  std::vector<sl_element_id_t> ids;
  ids.reserve(rows.size());
  for (const auto& row : rows) {
    ids.push_back(static_cast<sl_element_id_t>(row.element_id));
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

  const auto hashes = HashStrings(unreachable);
  const auto hash_in =
      expr::in_list(expr::col("commit_hash"), std::span<const std::string>(hashes));
  duckorm::Transaction transaction(conn_);
  auto                 delete_attribution = expr::raw("DELETE FROM EditCommitAttribution WHERE ");
  delete_attribution.append(hash_in);
  duckorm::execute(conn_, delete_attribution);
  commit_mapper_.RemoveWhere(hash_in);
  transaction.commit();
  return unreachable.size();
}

auto CommitGraphStore::DeleteUnreachableCommitsForProject() -> std::size_t {
  std::size_t total = 0;
  for (const auto element_id : ListImageElementIds()) {
    total += DeleteUnreachableCommits(element_id);
  }
  return total;
}

void CommitGraphStore::VisitStoredHistoryJsonContaining(
    std::string_view text, const std::function<void(const nlohmann::json&)>& visit) {
  // `$1` appears three times and takes the one bound value.
  const duckorm::SqlFragment query{
      "SELECT edit_payload::VARCHAR FROM EditCommit "
      "WHERE contains(edit_payload::VARCHAR, $1) "
      "UNION ALL SELECT serialized_pipeline_state::VARCHAR FROM ImageEditState "
      "WHERE serialized_pipeline_state IS NOT NULL "
      "AND contains(serialized_pipeline_state::VARCHAR, $1) "
      "UNION ALL SELECT serialized_pipeline_state::VARCHAR FROM PipelineRoot "
      "WHERE contains(serialized_pipeline_state::VARCHAR, $1);",
      {duckorm::BindValue{std::string(text)}}};
  for (const auto& row : duckorm::query<StoredHistoryJsonRow>(conn_, query)) {
    visit(nlohmann::json::parse(row.json));
  }
}

void CommitGraphStore::DeleteGraphForElement(sl_element_id_t element_id) {
  const auto state = GetImageEditState(element_id);
  if (!state.has_value()) {
    return;
  }

  const auto           root_equals = expr::column_eq("root_id", state->root_id.ToString());
  duckorm::Transaction transaction(conn_);
  version_ref_mapper_.RemoveWhere(ElementIdEquals(element_id));
  auto delete_attribution = expr::raw(
      "DELETE FROM EditCommitAttribution WHERE commit_hash IN "
      "(SELECT commit_hash FROM EditCommit WHERE ");
  delete_attribution.append(root_equals);
  delete_attribution.append(expr::raw(")"));
  duckorm::execute(conn_, delete_attribution);
  commit_mapper_.RemoveWhere(root_equals);
  image_edit_state_mapper_.RemoveById(element_id);
  auto delete_root = expr::raw("DELETE FROM PipelineRoot WHERE ");
  delete_root.append(ElementIdEquals(element_id));
  duckorm::execute(conn_, delete_root);
  transaction.commit();
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
  std::vector<sl_element_id_t> element_ids;
  element_ids.reserve(roots.size());
  for (const auto& root : roots) {
    element_ids.push_back(root.element_id);
  }
  auto existing_roots = expr::raw("SELECT COUNT(*) FROM ImageEditState WHERE ");
  existing_roots.append(
      expr::in_list(expr::col("element_id"), std::span<const sl_element_id_t>(element_ids)));
  if (duckorm::scalar<std::int64_t>(conn_, existing_roots).value_or(0) != 0) {
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

  duckorm::Transaction transaction(conn_);
  InsertRootSerializedPipelineStates(roots);
  version_ref_mapper_.UpsertParamsRows(version_ref_rows);
  image_edit_state_mapper_.UpsertParamsRows(image_edit_state_rows);
  if (write_rows) {
    write_rows();
  }
  transaction.commit();
}

}  // namespace alcedo
