//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <array>
#include <memory>
#include <string>
#include <string_view>

#include "app/editor_history_types.hpp"
#include "app/editor_session_types.hpp"
#include "edit/history/commit_graph.hpp"
#include "edit/history/commit_types.hpp"
#include "edit/history/edit_commit.hpp"
#include "edit/history/mini_git_working_history.hpp"
#include "json.hpp"

namespace alcedo {
struct PipelineGuard;
class PipelineMgmtService;
struct MiniGitJournalRecord;
}  // namespace alcedo

namespace alcedo::ui {

/// Stable field names shared by the editor models and the adjustment-transfer /
/// history services.
extern const std::array<std::string_view, 22> kEditorSnapshotFields;

/// Upsert a committed adjustment patch into the snapshot map.
void UpsertCommittedSnapshot(alcedo::EditorRenderAdjustmentSnapshot* snapshot,
                             const std::string& field_key, const nlohmann::json& params);

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

/// Read the current pipeline snapshot into a RenderAdjustmentSnapshot.
auto ReadPipelineSnapshot(alcedo::PipelineGuard& guard,
                          alcedo::EditorRenderAdjustmentSnapshot* snapshot, std::string* error)
    -> bool;

/// Initialize the committed snapshot from the live pipeline.
auto InitializeCommittedSnapshotFromPipeline(alcedo::PipelineGuard& guard,
                                             alcedo::EditorRenderAdjustmentSnapshot* snapshot,
                                             std::string* error) -> bool;

/// Apply one ordinary edit payload to the pipeline and snapshot.
auto ApplyCommittedPayload(alcedo::PipelineGuard& guard,
                           alcedo::EditorRenderAdjustmentSnapshot* snapshot,
                           const alcedo::OrdinaryEditPayload& payload, bool use_after_value,
                           std::string* error) -> bool;

/// Apply one first-parent history commit (ordinary or merge) to the live pipeline
/// and committed snapshot. When `use_after_value` is true, ordinary after-values and
/// merge resolved fields are applied. When false, ordinary before-values are applied
/// and each merge field is restored from the first-parent chain before that merge.
auto ApplyHistoryCommit(alcedo::PipelineGuard& guard,
                        alcedo::EditorRenderAdjustmentSnapshot* snapshot,
                        const alcedo::CommitGraph& graph, const alcedo::EditCommit& commit,
                        bool use_after_value, std::string* error) -> bool;

/// Apply every commit in a prepared head-move traversal. Does not touch the
/// journal, graph head, or redo selection.
auto ApplyPreparedHeadMovePipeline(alcedo::PipelineGuard& guard,
                                   alcedo::EditorRenderAdjustmentSnapshot* snapshot,
                                   const alcedo::CommitGraph& graph,
                                   const alcedo::MiniGitPreparedHeadMove& prepared,
                                   std::string* error) -> bool;

/// Apply a recovered journal record to the pipeline and snapshot.
auto ApplyRecoveredRecord(alcedo::PipelineGuard& guard,
                          alcedo::EditorRenderAdjustmentSnapshot* snapshot,
                          alcedo::CommitGraph* replay_graph,
                          const alcedo::MiniGitJournalRecord& record, std::string* error) -> bool;

/// Best-effort restore used only when a named-ref path mutated a live graph
/// copy after preparation. Prefer prepared candidate publication so this is
/// unused on the success path.
void RestoreGraphAndPipeline(alcedo::CommitGraph& graph, const alcedo::CommitGraph& prior_graph,
                             alcedo::PipelineMgmtService& pipeline_service,
                             const std::shared_ptr<alcedo::PipelineGuard>& pipeline_guard,
                             const alcedo::head_commit_hash_t& prior_head,
                             const alcedo::transaction_chain_hash_t& prior_chain,
                             bool prior_dirty, bool prior_serialized_state_needs_writeback);

}  // namespace alcedo::ui
