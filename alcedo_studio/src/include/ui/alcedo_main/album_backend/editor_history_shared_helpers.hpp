//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "app/editor_history_types.hpp"
#include "app/editor_adjustment_pipeline.hpp"
#include "app/editor_session_types.hpp"
#include "edit/history/commit_graph.hpp"
#include "edit/history/commit_types.hpp"
#include "edit/history/edit_commit.hpp"
#include "edit/history/mini_git_working_history.hpp"
#include "json.hpp"

namespace alcedo {
struct MiniGitJournalRecord;
}  // namespace alcedo

namespace alcedo::ui {

/// Stable field names shared by the editor models and the adjustment-transfer /
/// history services.
extern const std::array<std::string_view, 22> kEditorSnapshotFields;

/// Upsert a committed adjustment patch into the snapshot map.
void UpsertCommittedSnapshot(alcedo::EditorRenderAdjustmentSnapshot* snapshot,
                             const std::string& field_key, const nlohmann::json& params,
                             bool enabled = true);

/// Build a complete immutable editor snapshot with one entry for every supported field. Empty
/// parameter objects are explicit defaults; a missing supported field is never inferred later.
auto MakeEmptyCompleteAdjustmentSnapshot() -> alcedo::EditorRenderAdjustmentSnapshot;

/// Decode the pipeline parameter document stored by a save checkpoint into the complete editor
/// snapshot used by queue reductions. This parser is pure and never touches an executor.
auto MakeAdjustmentSnapshotFromPipelineParams(
    const nlohmann::json& pipeline_params, alcedo::EditorRenderAdjustmentSnapshot* snapshot,
    std::string* error) -> bool;

/// Convert a complete committed snapshot into the serialized pipeline document
/// stored by one Mini-Git save capture. The conversion is pure and validates
/// every supported editor field before returning.
auto MakePipelineParamsFromSnapshot(
    const alcedo::EditorRenderAdjustmentSnapshot& snapshot, std::string* error)
    -> std::optional<nlohmann::json>;

/// Validate that a committed snapshot has exactly one supported entry for every editor field.
auto IsCompleteAdjustmentSnapshot(const alcedo::EditorRenderAdjustmentSnapshot& snapshot,
                                  std::string* error) -> bool;

/// Read one committed field from an immutable snapshot. Failure is explicit when the field is not
/// present or is not a supported editor field.
auto ReadCommittedAdjustmentState(const alcedo::EditorRenderAdjustmentSnapshot& snapshot,
                                  const std::string& field_key,
                                  alcedo::EditorAdjustmentOperatorState* state,
                                  std::string* error) -> bool;

/// Determine whether operator params indicate an enabled adjustment.
auto EnabledForAdjustmentParams(const nlohmann::json& params) -> bool;

/// Extract the resolved field key from an edit commit.
auto CommitFieldKey(const alcedo::EditCommit& commit) -> std::string;

/// Build a history presentation row from an edit commit.
auto CommitRowFromEdit(const alcedo::EditCommit& commit,
                       alcedo::EditorHistoryTimelinePosition position)
    -> alcedo::EditorHistoryCommit;

/// Check whether a Version display name already exists in the graph.
auto VersionNameExists(const alcedo::CommitGraph& graph, const std::string& name,
                       const alcedo::version_ref_id_t* ignored = nullptr) -> bool;

/// Return a unique Version display name based on a requested name.
auto UniqueVersionName(const alcedo::CommitGraph& graph, std::string requested,
                       const alcedo::version_ref_id_t* ignored = nullptr) -> std::string;

/// Apply one ordinary commit to an immutable adjustment snapshot. No journal, graph, or executor
/// state is changed by this function.
auto ApplyCommittedPayloadToSnapshot(alcedo::EditorRenderAdjustmentSnapshot* snapshot,
                                     const alcedo::OrdinaryEditPayload& payload,
                                     bool use_after_value, std::string* error) -> bool;

/// Apply one history commit to an immutable adjustment snapshot. Merge before-values are resolved
/// against the commit's first-parent chain, so the live executor is not consulted.
auto ApplyHistoryCommitToSnapshot(alcedo::EditorRenderAdjustmentSnapshot* snapshot,
                                  const alcedo::CommitGraph& graph,
                                  const alcedo::EditCommit& commit, bool use_after_value,
                                  std::string* error) -> bool;

/// Pure history projection: apply all traversed commits from a prepared head
/// move onto a snapshot copy. Production mutation paths use `SnapshotAtHead` +
/// live pipeline install instead; this helper remains for recovery/tests.
auto ApplyPreparedHeadMoveToSnapshot(alcedo::EditorRenderAdjustmentSnapshot* snapshot,
                                     const alcedo::CommitGraph& graph,
                                     const alcedo::MiniGitPreparedHeadMove& prepared,
                                     std::string* error) -> bool;

/// Apply one journal record to a snapshot and the replay graph's current working selection.
auto ApplyRecoveredRecordToSnapshot(alcedo::EditorRenderAdjustmentSnapshot* snapshot,
                                    alcedo::CommitGraph* replay_graph,
                                    const alcedo::MiniGitJournalRecord& record,
                                    std::string* error) -> bool;

/// Rebuild an immutable snapshot from a root snapshot and a first-parent head.
auto SnapshotAtHead(const alcedo::EditorRenderAdjustmentSnapshot& root_snapshot,
                    const alcedo::CommitGraph& graph, const alcedo::head_commit_hash_t& head,
                    alcedo::EditorRenderAdjustmentSnapshot* snapshot, std::string* error) -> bool;

/// Reverse a materialized first-parent chain to derive its immutable root snapshot.
auto RootSnapshotFromMaterialized(const alcedo::EditorRenderAdjustmentSnapshot& materialized,
                                  const alcedo::CommitGraph& graph,
                                  const alcedo::head_commit_hash_t& materialized_head,
                                  alcedo::EditorRenderAdjustmentSnapshot* root_snapshot,
                                  std::string* error) -> bool;

}  // namespace alcedo::ui
