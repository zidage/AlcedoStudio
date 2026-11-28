//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "storage/store/edit_history/commit_graph_store.hpp"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
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

/// History labels of one ImageEditState row (ListMaterializedHistoryLabels).
struct MaterializedHistoryLabelRow {
  std::int64_t               element_id = 0;
  std::string                root_id;
  std::string                active_version_id;
  std::optional<std::string> materialized_head_commit_hash;
  std::string                materialized_transaction_chain_hash;

  using Columns =
      duckorm::RowColumns<&MaterializedHistoryLabelRow::element_id,
                          &MaterializedHistoryLabelRow::root_id,
                          &MaterializedHistoryLabelRow::active_version_id,
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

/// Root, owner, and document of one PipelineRoot row (ListRootSerializedPipelineStates).
struct KeyedRootPipelineStateRow {
  std::string  root_id;
  std::int64_t element_id = 0;
  std::string  serialized_pipeline_state;

  using Columns = duckorm::RowColumns<&KeyedRootPipelineStateRow::root_id,
                                      &KeyedRootPipelineStateRow::element_id,
                                      &KeyedRootPipelineStateRow::serialized_pipeline_state>;
};

/// One EditCommitAttribution row with the root of its commit (ListAttributionsForRoots). `note`
/// is NULL when empty.
struct AttributionRow {
  std::string                root_id;
  std::string                commit_hash;
  std::string                author;
  std::optional<std::string> note;

  using Columns = duckorm::RowColumns<&AttributionRow::root_id, &AttributionRow::commit_hash,
                                      &AttributionRow::author, &AttributionRow::note>;
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

/// The hash of one EditCommit row (ListCommitHashesForRoots).
struct CommitHashRow {
  std::string commit_hash;

  using Columns = duckorm::RowColumns<&CommitHashRow::commit_hash>;
};

/// True when @p stored holds every column that ToVersionRefParams writes for @p ref.
auto SameStoredVersionRef(const VersionRef& stored, const VersionRef& ref) -> bool {
  return stored.version_id == ref.version_id && stored.element_id == ref.element_id &&
         stored.display_name == ref.display_name &&
         stored.head_commit_hash == ref.head_commit_hash && stored.created_at == ref.created_at &&
         stored.updated_at == ref.updated_at;
}

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
  params.edit_payload       = MakeStringPtr(commit.GetPayloadText());
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
  // The payload text is the stored hash input: the commit checks its hash on it and parses it
  // only when a reader uses the payload.
  return EditCommit::FromStoredRow(
      Hash128::FromString(params.commit_hash ? *params.commit_hash : std::string{}),
      Hash128::FromString(params.root_id ? *params.root_id : std::string{}),
      HeadCommitHashFromStorage(params.first_parent_hash ? *params.first_parent_hash
                                                         : std::string{}),
      params.created_at_ns, params.edit_payload ? std::move(*params.edit_payload) : std::string{});
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
  auto labels = ListMaterializedHistoryLabels(std::span<const sl_element_id_t>(&element_id, 1));
  const auto found = labels.find(element_id);
  if (found == labels.end()) {
    return std::nullopt;
  }
  return found->second;
}

auto CommitGraphStore::ListMaterializedHistoryLabels(std::span<const sl_element_id_t> element_ids)
    -> std::unordered_map<sl_element_id_t, MaterializedHistoryLabel> {
  std::unordered_map<sl_element_id_t, MaterializedHistoryLabel> labels;
  if (element_ids.empty()) {
    return labels;
  }
  auto query = expr::raw(
      "SELECT element_id, root_id, active_version_id, materialized_head_commit_hash, "
      "materialized_transaction_chain_hash FROM ImageEditState WHERE ");
  query.append(expr::in_list(expr::col("element_id"), element_ids));
  for (const auto& row : duckorm::query<MaterializedHistoryLabelRow>(conn_, query)) {
    MaterializedHistoryLabel label;
    label.root_id           = Hash128::FromString(row.root_id);
    label.active_version_id = Hash128::FromString(row.active_version_id);
    label.head_commit_hash =
        HeadCommitHashFromStorage(row.materialized_head_commit_hash.value_or(std::string{}));
    label.transaction_chain_hash = Hash128::FromString(row.materialized_transaction_chain_hash);
    labels.emplace(static_cast<sl_element_id_t>(row.element_id), label);
  }
  return labels;
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

void CommitGraphStore::InsertAttributionsIfAbsent(
    std::span<const std::pair<commit_hash_t, const EditCommitAttribution*>> attributions) {
  // A commit hash names one commit, so a stored row already holds this attribution.
  constexpr std::size_t kRowsPerStatement = 256;
  for (std::size_t begin = 0; begin < attributions.size(); begin += kRowsPerStatement) {
    const auto           end = std::min(attributions.size(), begin + kRowsPerStatement);
    duckorm::SqlFragment insert{
        "INSERT OR IGNORE INTO EditCommitAttribution (commit_hash, author, note) VALUES ", {}};
    for (std::size_t index = begin; index < end; ++index) {
      const auto& [hash, attribution] = attributions[index];
      insert.append(expr::raw(index == begin ? "(?, ?, ?)" : ", (?, ?, ?)"));
      insert.binds_.push_back(duckorm::BindValue{hash.ToString()});
      insert.binds_.push_back(duckorm::BindValue{attribution->author_});
      insert.binds_.push_back(attribution->note_.empty() ? duckorm::BindValue{}
                                                         : duckorm::BindValue{attribution->note_});
    }
    insert.append(expr::raw(";"));
    duckorm::execute(conn_, insert);
  }
}

auto CommitGraphStore::ListRootSerializedPipelineStates(std::span<const root_id_t> root_ids)
    -> std::unordered_map<root_id_t, StoredRootPipelineState> {
  std::unordered_map<root_id_t, StoredRootPipelineState> roots;
  if (root_ids.empty()) {
    return roots;
  }
  const auto root_strings = HashStrings(root_ids);
  auto       query        = expr::raw(
      "SELECT root_id, element_id, serialized_pipeline_state::VARCHAR FROM PipelineRoot WHERE ");
  query.append(expr::in_list(expr::col("root_id"), std::span<const std::string>(root_strings)));
  for (auto& row : duckorm::query<KeyedRootPipelineStateRow>(conn_, query)) {
    roots.emplace(Hash128::FromString(row.root_id),
                  StoredRootPipelineState{
                      .element_id                = static_cast<sl_element_id_t>(row.element_id),
                      .serialized_pipeline_state = std::move(row.serialized_pipeline_state),
                  });
  }
  return roots;
}

auto CommitGraphStore::ListAttributionsForRoots(std::span<const root_id_t> root_ids)
    -> std::unordered_map<root_id_t, std::unordered_map<commit_hash_t, EditCommitAttribution>> {
  std::unordered_map<root_id_t, std::unordered_map<commit_hash_t, EditCommitAttribution>>
      attributions;
  if (root_ids.empty()) {
    return attributions;
  }
  const auto root_strings = HashStrings(root_ids);
  auto       query        = expr::raw(
      "SELECT c.root_id, a.commit_hash, a.author, a.note FROM EditCommitAttribution a "
                   "JOIN EditCommit c ON c.commit_hash = a.commit_hash WHERE ");
  query.append(expr::in_list(expr::col("c.root_id"), std::span<const std::string>(root_strings)));
  for (auto& row : duckorm::query<AttributionRow>(conn_, query)) {
    attributions[Hash128::FromString(row.root_id)].emplace(
        Hash128::FromString(row.commit_hash),
        EditCommitAttribution{std::move(row.author), std::move(row.note).value_or(std::string{})});
  }
  return attributions;
}

auto CommitGraphStore::ListCommitHashesForRoots(std::span<const root_id_t> root_ids)
    -> std::unordered_set<commit_hash_t> {
  std::unordered_set<commit_hash_t> hashes;
  if (root_ids.empty()) {
    return hashes;
  }
  const auto root_strings = HashStrings(root_ids);
  auto       query        = expr::raw("SELECT commit_hash FROM EditCommit WHERE ");
  query.append(expr::in_list(expr::col("root_id"), std::span<const std::string>(root_strings)));
  for (const auto& row : duckorm::query<CommitHashRow>(conn_, query)) {
    hashes.insert(Hash128::FromString(row.commit_hash));
  }
  return hashes;
}

void CommitGraphStore::Materialize(const CommitGraphMaterialization& materialization) {
  const CommitGraphMaterialization* const one = &materialization;
  MaterializeAll(std::span<const CommitGraphMaterialization* const>(&one, 1));
}

void CommitGraphStore::MaterializeAll(
    std::span<const CommitGraphMaterialization* const> materializations) {
  if (materializations.empty()) {
    return;
  }
  // Validate fully before any write so a bad capture leaves prior rows unchanged.
  std::unordered_set<sl_element_id_t> element_ids;
  std::vector<sl_element_id_t>        element_id_list;
  std::vector<root_id_t>              root_ids;
  for (const auto* materialization : materializations) {
    materialization->Validate();
    const auto element_id = materialization->image_state.element_id;
    if (!element_ids.insert(element_id).second) {
      throw std::invalid_argument(
          "CommitGraphStore: MaterializeAll takes one capture for each image");
    }
    element_id_list.push_back(element_id);
    root_ids.push_back(materialization->image_state.root_id);
  }

  duckorm::Transaction transaction(conn_);

  // Commits are content addressed and immutable: a stored hash is the stored commit, so only the
  // commits that storage does not hold are written.
  const auto                          stored_hashes = ListCommitHashesForRoots(root_ids);
  std::vector<EditCommitMapperParams> new_commit_rows;
  std::vector<std::pair<commit_hash_t, const EditCommitAttribution*>> new_attributions;
  for (const auto* materialization : materializations) {
    for (const auto& commit : materialization->commits) {
      const auto hash = commit.GetCommitHash();
      if (stored_hashes.contains(hash)) {
        continue;
      }
      new_commit_rows.push_back(ToCommitParams(commit));
      // A stored commit was written with its attribution in one transaction, so only a new
      // commit can bring an attribution row.
      if (const auto found = materialization->attributions.find(hash);
          found != materialization->attributions.end()) {
        new_attributions.emplace_back(hash, &found->second);
      }
    }
  }
  commit_mapper_.InsertParamsRows(new_commit_rows);
  InsertAttributionsIfAbsent(new_attributions);

  // Version refs: delete the stored refs that a capture dropped, and write the new or changed
  // ones. A capture that adds one Version writes one row.
  std::unordered_map<version_ref_id_t, VersionRef> stored_refs;
  for (auto& row : version_ref_mapper_.GetWhere(expr::in_list(
           expr::col("element_id"), std::span<const sl_element_id_t>(element_id_list)))) {
    auto       ref        = FromVersionRefParams(std::move(row));
    const auto version_id = ref.version_id;
    stored_refs.emplace(version_id, std::move(ref));
  }
  std::unordered_set<version_ref_id_t>    kept_refs;
  std::vector<VersionRefMapperParams>     changed_ref_rows;
  std::vector<ImageEditStateMapperParams> state_rows;
  state_rows.reserve(materializations.size());
  for (const auto* materialization : materializations) {
    for (const auto& ref : materialization->version_refs) {
      kept_refs.insert(ref.version_id);
      const auto stored = stored_refs.find(ref.version_id);
      if (stored == stored_refs.end() || !SameStoredVersionRef(stored->second, ref)) {
        changed_ref_rows.push_back(ToVersionRefParams(ref));
      }
    }
    state_rows.push_back(ToImageEditStateParams(materialization->image_state));
  }
  std::vector<std::string> removed_version_ids;
  for (const auto& [version_id, ref] : stored_refs) {
    (void)ref;
    if (!kept_refs.contains(version_id)) {
      removed_version_ids.push_back(version_id.ToString());
    }
  }
  if (!removed_version_ids.empty()) {
    version_ref_mapper_.RemoveWhere(
        expr::in_list(expr::col("version_id"), std::span<const std::string>(removed_version_ids)));
  }
  version_ref_mapper_.UpsertParamsRows(changed_ref_rows);
  image_edit_state_mapper_.UpsertParamsRows(state_rows);
  transaction.commit();
}

auto CommitGraphStore::LoadGraph(sl_element_id_t element_id) -> std::optional<CommitGraph> {
  auto  reads = LoadGraphs(std::span<const sl_element_id_t>(&element_id, 1));
  auto& read  = reads.front();
  if (!read.error.empty()) {
    throw std::runtime_error(read.error);
  }
  return std::move(read.graph);
}

auto CommitGraphStore::LoadGraphs(std::span<const sl_element_id_t> element_ids)
    -> std::vector<StoredGraphRead> {
  std::vector<StoredGraphRead> reads(element_ids.size());
  for (std::size_t index = 0; index < element_ids.size(); ++index) {
    reads[index].element_id = element_ids[index];
  }
  if (element_ids.empty()) {
    return reads;
  }

  std::unordered_map<sl_element_id_t, ImageEditState> states;
  std::unordered_map<sl_element_id_t, std::string>    errors;
  for (auto& row :
       image_edit_state_mapper_.GetWhere(expr::in_list(expr::col("element_id"), element_ids))) {
    const auto element_id = static_cast<sl_element_id_t>(row.element_id);
    try {
      states.emplace(element_id, FromImageEditStateParams(std::move(row)));
    } catch (const std::exception& e) {
      errors.emplace(element_id, e.what());
    }
  }

  std::vector<sl_element_id_t>                   state_ids;
  std::vector<root_id_t>                         root_ids;
  std::unordered_map<root_id_t, sl_element_id_t> element_by_root;
  for (const auto& [element_id, state] : states) {
    state_ids.push_back(element_id);
    root_ids.push_back(state.root_id);
    element_by_root.emplace(state.root_id, element_id);
  }

  std::unordered_map<sl_element_id_t, std::vector<VersionRef>> refs;
  std::unordered_map<sl_element_id_t, std::vector<EditCommit>> commits;
  std::unordered_map<root_id_t, std::unordered_map<commit_hash_t, EditCommitAttribution>>
      attributions;
  if (!state_ids.empty()) {
    for (auto& row : version_ref_mapper_.GetWhere(
             expr::in_list(expr::col("element_id"), std::span<const sl_element_id_t>(state_ids)))) {
      auto ref = FromVersionRefParams(std::move(row));
      refs[ref.element_id].push_back(std::move(ref));
    }
    const auto root_strings = HashStrings(root_ids);
    for (auto& row : commit_mapper_.GetWhere(
             expr::in_list(expr::col("root_id"), std::span<const std::string>(root_strings)))) {
      const auto root  = Hash128::FromString(row.root_id ? *row.root_id : std::string{});
      const auto owner = element_by_root.find(root);
      if (owner == element_by_root.end()) {
        continue;
      }
      try {
        commits[owner->second].push_back(FromCommitParams(std::move(row)));
      } catch (const std::exception& e) {
        errors.emplace(owner->second, e.what());
      }
    }
    attributions = ListAttributionsForRoots(root_ids);
  }

  for (auto& read : reads) {
    const auto element_id = read.element_id;
    if (const auto error = errors.find(element_id); error != errors.end()) {
      read.error = error->second;
      continue;
    }
    const auto state = states.find(element_id);
    if (state == states.end()) {
      continue;
    }
    try {
      const auto root_id = state->second.root_id;
      // FromParts validates parent rules, Version heads, and materialized head/chain agreement.
      auto graph = CommitGraph::FromParts(state->second, refs[element_id], commits[element_id]);
      if (const auto found = attributions.find(root_id); found != attributions.end()) {
        for (const auto& [hash, attribution] : found->second) {
          graph.SetCommitAttribution(hash, attribution);
        }
      }
      read.graph = std::move(graph);
    } catch (const std::exception& e) {
      read.error = e.what();
    }
  }
  return reads;
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
