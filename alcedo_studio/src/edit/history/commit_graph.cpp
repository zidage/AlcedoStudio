//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/history/commit_graph.hpp"

#include <chrono>
#include <utility>

#include "utils/clock/time_provider.hpp"

namespace alcedo {
namespace {

auto NowTime() -> std::time_t { return std::chrono::system_clock::to_time_t(TimeProvider::Now()); }

}  // namespace

void CommitGraphMaterialization::Validate() const {
  if (version_refs.empty()) {
    throw std::runtime_error("CommitGraphMaterialization: requires at least one Version ref");
  }
  if (image_state.project_schema_version != kImageEditSchemaVersion) {
    throw std::runtime_error("CommitGraphMaterialization: incompatible image edit schema version");
  }

  const VersionRef* active = nullptr;
  for (const auto& ref : version_refs) {
    if (ref.element_id != image_state.element_id) {
      throw std::runtime_error(
          "CommitGraphMaterialization: Version ref element_id does not match image state");
    }
    if (ref.version_id == image_state.active_version_id) {
      active = &ref;
    }
  }
  if (active == nullptr) {
    throw std::runtime_error("CommitGraphMaterialization: active Version ref is missing");
  }
  if (active->head_commit_hash != image_state.materialized_head_commit_hash) {
    throw std::runtime_error(
        "CommitGraphMaterialization: Version head disagrees with materialized head");
  }

  // Rebuild a temporary graph to validate structure and chain fold.
  auto       graph  = CommitGraph::FromParts(image_state, version_refs, commits);
  const auto folded = graph.ChainHashForHead(image_state.materialized_head_commit_hash);
  if (folded != image_state.materialized_transaction_chain_hash) {
    throw std::runtime_error(
        "CommitGraphMaterialization: chain hash disagrees with first-parent fold");
  }
}

auto CommitGraph::CreateEmpty(sl_element_id_t element_id, std::string default_display_name)
    -> CommitGraph {
  auto [state, default_ref] =
      CreateEmptyImageEditState(element_id, std::move(default_display_name));
  CommitGraph graph;
  graph.state_ = std::move(state);
  graph.version_refs_.emplace(default_ref.version_id, std::move(default_ref));
  return graph;
}

auto CommitGraph::FromParts(ImageEditState state, std::vector<VersionRef> version_refs,
                            std::vector<EditCommit> commits) -> CommitGraph {
  if (version_refs.empty()) {
    throw std::runtime_error("CommitGraph: FromParts requires at least one Version ref");
  }
  if (state.project_schema_version != kImageEditSchemaVersion) {
    throw std::runtime_error("CommitGraph: incompatible image edit schema version");
  }

  CommitGraph graph;
  graph.state_ = std::move(state);

  for (auto& commit : commits) {
    commit.ValidateStructure();
    if (commit.GetRootId() != graph.state_.root_id) {
      throw std::runtime_error("CommitGraph: commit root_id does not match image root");
    }
    const auto hash = commit.GetCommitHash();
    if (commit.ComputeCommitHash() != hash) {
      throw std::runtime_error("CommitGraph: commit hash does not match content");
    }
    graph.commits_.emplace(hash, std::move(commit));
  }

  bool found_active = false;
  for (auto& ref : version_refs) {
    if (ref.element_id != graph.state_.element_id) {
      throw std::runtime_error("CommitGraph: Version ref element_id mismatch");
    }
    if (ref.head_commit_hash.has_value() &&
        graph.commits_.find(*ref.head_commit_hash) == graph.commits_.end()) {
      throw std::runtime_error("CommitGraph: Version ref head commit is missing");
    }
    if (ref.version_id == graph.state_.active_version_id) {
      found_active = true;
    }
    graph.version_refs_.emplace(ref.version_id, std::move(ref));
  }
  if (!found_active) {
    throw std::runtime_error("CommitGraph: active Version ref is missing");
  }

  graph.ValidateReachableStructure();
  graph.ValidateMaterializedAgreement(graph.state_);
  return graph;
}

void CommitGraph::ValidateCommitAgainstGraph(const EditCommit& commit) const {
  commit.ValidateStructure();
  if (commit.GetRootId() != state_.root_id) {
    throw std::runtime_error("CommitGraph: commit root_id does not match graph root");
  }
  if (commit.GetFirstParentHash().has_value()) {
    const auto* parent = FindCommit(*commit.GetFirstParentHash());
    if (parent == nullptr) {
      throw std::runtime_error("CommitGraph: first parent commit is missing");
    }
    if (parent->GetRootId() != state_.root_id) {
      throw std::runtime_error("CommitGraph: first parent belongs to another root");
    }
  }
  if (commit.GetKind() == EditCommitKind::kEdit) {
    if (commit.GetSecondParentHash().has_value()) {
      throw std::runtime_error("CommitGraph: Edit commit must not have a second parent");
    }
  } else if (commit.GetKind() == EditCommitKind::kMerge) {
    if (!commit.GetSecondParentHash().has_value()) {
      throw std::runtime_error("CommitGraph: Merge commit requires a second parent");
    }
    const auto* parent = FindCommit(*commit.GetSecondParentHash());
    if (parent == nullptr) {
      throw std::runtime_error("CommitGraph: second parent commit is missing");
    }
    if (parent->GetRootId() != state_.root_id) {
      throw std::runtime_error("CommitGraph: second parent belongs to another root");
    }
  } else {
    throw std::runtime_error("CommitGraph: unknown commit kind");
  }
}

void CommitGraph::ValidateReachableStructure() const {
  for (const auto& [hash, commit] : commits_) {
    (void)hash;
    ValidateCommitAgainstGraph(commit);
  }
  for (const auto& [version_id, ref] : version_refs_) {
    (void)version_id;
    if (ref.head_commit_hash.has_value() && FindCommit(*ref.head_commit_hash) == nullptr) {
      throw std::runtime_error("CommitGraph: Version head is not present in commit table");
    }
    // First-parent path from every ref head must be fully reachable.
    (void)FirstParentChain(ref.head_commit_hash);
  }
}

void CommitGraph::ValidateMaterializedAgreement(const ImageEditState& candidate) const {
  const auto& active = GetVersionRef(candidate.active_version_id);
  if (active.element_id != candidate.element_id) {
    throw std::runtime_error("CommitGraph: active Version element_id mismatch");
  }
  if (active.head_commit_hash != candidate.materialized_head_commit_hash) {
    throw std::runtime_error("CommitGraph: active Version head disagrees with materialized head");
  }
  const auto folded = ChainHashForHead(candidate.materialized_head_commit_hash);
  if (folded != candidate.materialized_transaction_chain_hash) {
    throw std::runtime_error(
        "CommitGraph: materialized chain hash disagrees with first-parent fold");
  }
}

auto CommitGraph::GetVersionRef(const version_ref_id_t& version_id) const -> const VersionRef& {
  const auto it = version_refs_.find(version_id);
  if (it == version_refs_.end()) {
    throw std::runtime_error("CommitGraph: Version ref not found");
  }
  return it->second;
}

auto CommitGraph::GetVersionRef(const version_ref_id_t& version_id) -> VersionRef& {
  auto it = version_refs_.find(version_id);
  if (it == version_refs_.end()) {
    throw std::runtime_error("CommitGraph: Version ref not found");
  }
  return it->second;
}

auto CommitGraph::GetActiveVersionRef() const -> const VersionRef& {
  return GetVersionRef(state_.active_version_id);
}

auto CommitGraph::GetActiveVersionRef() -> VersionRef& {
  return GetVersionRef(state_.active_version_id);
}

auto CommitGraph::GetCommit(const commit_hash_t& commit_hash) const -> const EditCommit& {
  const auto it = commits_.find(commit_hash);
  if (it == commits_.end()) {
    throw std::runtime_error("CommitGraph: commit not found");
  }
  return it->second;
}

auto CommitGraph::FindCommit(const commit_hash_t& commit_hash) const -> const EditCommit* {
  const auto it = commits_.find(commit_hash);
  if (it == commits_.end()) {
    return nullptr;
  }
  return &it->second;
}

auto CommitGraph::InsertCommit(EditCommit commit) -> bool {
  ValidateCommitAgainstGraph(commit);
  const auto hash = commit.GetCommitHash();
  if (commits_.find(hash) != commits_.end()) {
    return false;
  }
  commits_.emplace(hash, std::move(commit));
  return true;
}

auto CommitGraph::CreateVersionRefInternal(std::string display_name, head_commit_hash_t head,
                                           std::time_t created_at) -> version_ref_id_t {
  if (head.has_value() && commits_.find(*head) == commits_.end()) {
    throw std::runtime_error("CommitGraph: head commit is missing");
  }

  auto [scratch_state, scratch_ref] =
      CreateEmptyImageEditState(state_.element_id, std::move(display_name));
  (void)scratch_state;
  VersionRef ref        = std::move(scratch_ref);
  ref.element_id        = state_.element_id;
  ref.head_commit_hash  = std::move(head);
  const auto stamp      = created_at != 0 ? created_at : NowTime();
  ref.created_at        = stamp;
  ref.updated_at        = stamp;
  const auto version_id = ref.version_id;
  version_refs_.emplace(version_id, std::move(ref));
  return version_id;
}

auto CommitGraph::CreateVersionRefAtRoot(std::string display_name, std::time_t created_at)
    -> version_ref_id_t {
  return CreateVersionRefInternal(std::move(display_name), std::nullopt, created_at);
}

auto CommitGraph::CreateVersionRefAtHead(std::string display_name, head_commit_hash_t head,
                                         std::time_t created_at) -> version_ref_id_t {
  return CreateVersionRefInternal(std::move(display_name), std::move(head), created_at);
}

auto CommitGraph::CreateVersionRefAtActiveHead(std::string display_name, std::time_t created_at)
    -> version_ref_id_t {
  return CreateVersionRefInternal(std::move(display_name), GetActiveVersionRef().head_commit_hash,
                                  created_at);
}

void CommitGraph::MoveWorkingHead(const version_ref_id_t& version_id, head_commit_hash_t new_head,
                                  std::time_t updated_at) {
  if (new_head.has_value() && commits_.find(*new_head) == commits_.end()) {
    throw std::runtime_error("CommitGraph: head commit is missing");
  }
  auto& ref = GetVersionRef(version_id);
  // Working-head only: ImageEditState.materialized_* is intentionally left unchanged.
  alcedo::MoveVersionRefHead(ref, std::move(new_head), updated_at);
}

void CommitGraph::SetActiveVersionId(const version_ref_id_t& version_id) {
  (void)GetVersionRef(version_id);
  state_.active_version_id = version_id;
}

auto CommitGraph::FirstParentChain(const head_commit_hash_t& head) const
    -> std::vector<commit_hash_t> {
  std::vector<commit_hash_t> reverse_path;
  auto                       current = head;
  while (current.has_value()) {
    const auto* commit = FindCommit(*current);
    if (commit == nullptr) {
      throw std::runtime_error("CommitGraph: missing commit on first-parent path");
    }
    if (commit->GetRootId() != state_.root_id) {
      throw std::runtime_error("CommitGraph: first-parent path crosses roots");
    }
    reverse_path.push_back(*current);
    current = commit->GetFirstParentHash();
  }
  return std::vector<commit_hash_t>(reverse_path.rbegin(), reverse_path.rend());
}

auto CommitGraph::ChainHashForHead(const head_commit_hash_t& head) const
    -> transaction_chain_hash_t {
  return FoldFirstParentChain(state_.root_id, FirstParentChain(head));
}

auto CommitGraph::ChainHashForVersion(const version_ref_id_t& version_id) const
    -> transaction_chain_hash_t {
  return ChainHashForHead(GetVersionRef(version_id).head_commit_hash);
}

namespace {

auto BuildMaterializationBase(const CommitGraph& graph, const ImageEditState& state)
    -> CommitGraphMaterialization {
  CommitGraphMaterialization materialization;
  materialization.image_state                               = state;
  const auto& active                                        = graph.GetActiveVersionRef();
  materialization.image_state.materialized_head_commit_hash = active.head_commit_hash;
  materialization.image_state.materialized_transaction_chain_hash =
      graph.ChainHashForHead(active.head_commit_hash);
  materialization.image_state.active_version_id = active.version_id;

  materialization.version_refs.reserve(graph.GetAllVersionRefs().size());
  for (const auto& [id, ref] : graph.GetAllVersionRefs()) {
    (void)id;
    materialization.version_refs.push_back(ref);
  }
  materialization.commits.reserve(graph.GetAllCommits().size());
  for (const auto& [hash, commit] : graph.GetAllCommits()) {
    (void)hash;
    materialization.commits.push_back(commit);
  }
  return materialization;
}

}  // namespace

auto CommitGraph::CaptureMaterialization() const -> CommitGraphMaterialization {
  // Preserve the current serialized state; do not treat absence of an argument as clear.
  auto materialization = BuildMaterializationBase(*this, state_);
  materialization.Validate();
  return materialization;
}

auto CommitGraph::CaptureMaterializationWithSerializedPipelineState(
    std::optional<nlohmann::json> serialized_pipeline_state) const
    -> CommitGraphMaterialization {
  auto materialization                                  = BuildMaterializationBase(*this, state_);
  materialization.image_state.serialized_pipeline_state = std::move(serialized_pipeline_state);
  materialization.Validate();
  return materialization;
}

auto CommitGraph::CaptureMaterializationClearingSerializedPipelineState() const
    -> CommitGraphMaterialization {
  return CaptureMaterializationWithSerializedPipelineState(std::nullopt);
}

void CommitGraph::ApplyMaterializedState(const ImageEditState& materialized_state) {
  if (materialized_state.element_id != state_.element_id ||
      materialized_state.root_id != state_.root_id) {
    throw std::runtime_error("CommitGraph: materialization identity mismatch");
  }
  ValidateMaterializedAgreement(materialized_state);
  state_ = materialized_state;
}

}  // namespace alcedo
