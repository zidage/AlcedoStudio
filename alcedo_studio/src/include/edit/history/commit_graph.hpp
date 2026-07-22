//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include "edit/history/commit_types.hpp"
#include "edit/history/edit_commit.hpp"
#include "edit/history/version_ref.hpp"
#include "json.hpp"
#include "type/type.hpp"

namespace alcedo {

/**
 * @brief One immutable capture of the values materialization will write to DuckDB together.
 *
 * Working-head moves must not use this structure. Callers build it only when the checked-out
 * Version head, recomputed chain hash, and serialized pipeline state are known to agree.
 */
struct CommitGraphMaterialization {
  ImageEditState          image_state{};
  std::vector<VersionRef> version_refs;
  std::vector<EditCommit> commits;

  /// Validate mutual agreement of active Version, head, chain hash, and commit structure.
  void                    Validate() const;
};

/**
 * @brief In-memory immutable commit graph plus Version refs for one image.
 *
 * Commit objects are stored once by hash. Multiple Version refs may share one head and
 * ancestry without duplicating rows. Production edits advance only a working head here;
 * materialized state advances later through an explicit checkpoint capture.
 *
 * Working heads live on VersionRef. ImageEditState.materialized_* advances only via an explicit
 * materialization capture, never by MoveWorkingHead alone.
 */
class CommitGraph {
 public:
  CommitGraph() = default;

  /// Infrastructure helper: one empty image edit state, root identity, and default Version at root.
  /// Production immutable-root creation after import metadata remains a later package.
  static auto CreateEmpty(sl_element_id_t element_id, std::string default_display_name = "Default")
      -> CommitGraph;

  /// Rebuild from stored parts with full structural and materialized-state validation.
  static auto FromParts(ImageEditState state, std::vector<VersionRef> version_refs,
                        std::vector<EditCommit> commits) -> CommitGraph;

  auto        GetElementId() const -> sl_element_id_t { return state_.element_id; }
  auto        GetRootId() const -> root_id_t { return state_.root_id; }
  auto        GetImageEditState() const -> const ImageEditState& { return state_; }
  auto        GetActiveVersionId() const -> version_ref_id_t { return state_.active_version_id; }

  auto        GetVersionRef(const version_ref_id_t& version_id) const -> const VersionRef&;
  auto        GetVersionRef(const version_ref_id_t& version_id) -> VersionRef&;
  auto        GetActiveVersionRef() const -> const VersionRef&;
  auto        GetActiveVersionRef() -> VersionRef&;
  auto        GetAllVersionRefs() const -> const std::unordered_map<version_ref_id_t, VersionRef>& {
    return version_refs_;
  }

  auto GetCommit(const commit_hash_t& commit_hash) const -> const EditCommit&;
  auto FindCommit(const commit_hash_t& commit_hash) const -> const EditCommit*;
  auto CommitCount() const -> std::size_t { return commits_.size(); }
  auto GetAllCommits() const -> const std::unordered_map<commit_hash_t, EditCommit>& {
    return commits_;
  }

  /// Insert a finalized commit object. An already-present identical hash is ignored (shared).
  auto InsertCommit(EditCommit commit) -> bool;

  /// Create a Version whose head is the image root (null). Unambiguous even when active is
  /// non-root.
  auto CreateVersionRefAtRoot(std::string display_name, std::time_t created_at = 0)
      -> version_ref_id_t;

  /// Create a Version at an explicit head. nullopt always means root, never "copy active".
  auto CreateVersionRefAtHead(std::string display_name, head_commit_hash_t head,
                              std::time_t created_at = 0) -> version_ref_id_t;

  /// Create a Version whose head equals the active Version's current working head.
  auto CreateVersionRefAtActiveHead(std::string display_name, std::time_t created_at = 0)
      -> version_ref_id_t;

  /// Move only the in-memory working head. Does not advance ImageEditState materialized fields.
  void MoveWorkingHead(const version_ref_id_t& version_id, head_commit_hash_t new_head,
                       std::time_t updated_at = 0);

  void SetActiveVersionId(const version_ref_id_t& version_id);

  /// Walk first parents from head to root, then reverse to root→head order for replay.
  auto FirstParentChain(const head_commit_hash_t& head) const -> std::vector<commit_hash_t>;

  auto ChainHashForHead(const head_commit_hash_t& head) const -> transaction_chain_hash_t;
  auto ChainHashForVersion(const version_ref_id_t& version_id) const -> transaction_chain_hash_t;

  /// Capture materialization keeping ImageEditState.serialized_pipeline_state unchanged.
  auto CaptureMaterialization() const -> CommitGraphMaterialization;

  /// Capture materialization with an explicit serialized pipeline state value.
  auto CaptureMaterializationWithSerializedPipelineState(
      std::optional<nlohmann::json> serialized_pipeline_state) const
      -> CommitGraphMaterialization;

  /// Capture materialization and clear any serialized pipeline state.
  auto CaptureMaterializationClearingSerializedPipelineState() const
      -> CommitGraphMaterialization;

  /// Apply a successful materialization result to the in-memory ImageEditState only.
  void ApplyMaterializedState(const ImageEditState& materialized_state);

 private:
  auto           CreateVersionRefInternal(std::string display_name, head_commit_hash_t head,
                                          std::time_t created_at) -> version_ref_id_t;
  void           ValidateCommitAgainstGraph(const EditCommit& commit) const;
  void           ValidateReachableStructure() const;
  void           ValidateMaterializedAgreement(const ImageEditState& candidate) const;

  ImageEditState state_{};
  std::unordered_map<commit_hash_t, EditCommit>    commits_;
  std::unordered_map<version_ref_id_t, VersionRef> version_refs_;
};

}  // namespace alcedo
