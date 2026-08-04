//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/editor_history_transfer.hpp"

#include <mutex>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "app/adjustment_transfer_service.hpp"
#include "app/editor_adjustment_pipeline.hpp"
#include "app/editor_mini_git_materializer.hpp"
#include "app/pipeline_service.hpp"
#include "edit/history/commit_graph.hpp"
#include "edit/history/mini_git_working_history.hpp"
#include "edit/operators/op_base.hpp"
#include "ui/alcedo_main/album_backend/editor_history_shared_helpers.hpp"
#include "ui/alcedo_main/album_backend/editor_history_state_detail.hpp"

namespace alcedo::ui {
namespace {

auto SetError(std::string* error, std::string message) -> bool {
  if (error != nullptr) *error = std::move(message);
  return false;
}

}  // namespace

EditorHistoryTransfer::EditorHistoryTransfer(EditorHistoryState& state) : state_(state) {}

namespace {

struct LivePastePriorState {
  alcedo::CommitGraph graph;  // includes logical head on active Version
  alcedo::MiniGitWorkingSelection selection;
  bool dirty = false;
  bool serialized = false;
  alcedo::EditorRenderAdjustmentSnapshot snapshot;
  std::unordered_map<std::string, alcedo::EditorAdjustmentOperatorState> pending;
  bool recovered = false;
};

auto CaptureLivePastePrior(HistoryWorkingState& state) -> LivePastePriorState {
  LivePastePriorState prior;
  prior.graph = *state.pipeline_guard->commit_graph_;
  prior.selection = state.history->WorkingSelection();
  prior.dirty = state.pipeline_guard->dirty_;
  prior.serialized = state.pipeline_guard->serialized_state_needs_writeback_;
  prior.snapshot = state.committed_snapshot;
  prior.pending = state.pending_before;
  prior.recovered = state.recovered_head;
  return prior;
}

void RestoreLivePastePrior(HistoryWorkingState& state, const LivePastePriorState& prior) {
  *state.pipeline_guard->commit_graph_ = prior.graph;
  state.history->PublishWorkingSelection(prior.selection);
  state.pipeline_guard->dirty_ = prior.dirty;
  state.pipeline_guard->serialized_state_needs_writeback_ = prior.serialized;
  state.committed_snapshot = prior.snapshot;
  state.pending_before = prior.pending;
  state.recovered_head = prior.recovered;
}

/// Append paste package entries to the live CommitGraph + WAL only. Do not
/// mutate committed_snapshot here; the caller regenerates it from history after
/// applying the package to the live pipeline.
auto AppendPackageEntriesToLiveHistory(HistoryWorkingState& state,
                                       const alcedo::AdjustmentTransferPackage& package,
                                       std::string* error) -> bool {
  for (const auto& entry : package.operators_) {
    if (entry.stage_ == alcedo::PipelineStageName::Stage_Count ||
        entry.operator_type_ == alcedo::OperatorType::UNKNOWN ||
        entry.operator_type_ == alcedo::OperatorType::RESIZE) {
      continue;
    }
    alcedo::OrdinaryEditPayload payload;
    payload.operator_type = entry.operator_type_;
    payload.stage_name = entry.stage_;
    payload.field_name = "$operator_params";
    payload.before_value = nlohmann::json(nullptr);
    payload.before_enabled = false;
    payload.after_value = entry.params_;
    payload.after_enabled = entry.enabled_;
    const auto prepared = state.history->PrepareAppendEdit(std::move(payload));
    if (!prepared.ready) {
      return SetError(error, prepared.error.empty() ? "Paste edit prepare failed" : prepared.error);
    }
    const auto appended = state.history->PublishPreparedEdit(prepared);
    if (!appended.committed) {
      return SetError(error, appended.error.empty() ? "Paste WAL append failed" : appended.error);
    }
  }
  return true;
}

}  // namespace

auto EditorHistoryTransfer::PasteLiveRootRelativeVersion(
    const alcedo::EditorHistoryGuardHandle& guard,
    const alcedo::AdjustmentTransferPackage& package, std::string version_display_name,
    alcedo::AdjustmentPasteResult* result, std::string* error) -> bool {
  auto state = state_.EnsureWorkingState(guard.element_id, error);
  if (!state) return false;
  if (result == nullptr) return SetError(error, "Paste result storage is required");
  *result = {};
  if (package.Empty()) return SetError(error, "Adjustment transfer package is empty");
  if (!state->pipeline_guard || !state->pipeline_guard->commit_graph_ || !state->history ||
      !state->journal) {
    return SetError(error, "Editor live paste requires a complete history state");
  }
  if (!state->pipeline_guard->pipeline_) {
    return SetError(error, "Editor live paste requires a live pipeline executor");
  }

  auto& graph = *state->pipeline_guard->commit_graph_;
  const auto prior_version_id = graph.GetActiveVersionId();
  const auto prior = CaptureLivePastePrior(*state);
  const auto expected_materialized = prior.graph.GetImageEditState();

  alcedo::version_ref_id_t new_version_id{};
  try {
    new_version_id =
        graph.CreateVersionRefAtRoot(UniqueVersionName(graph, std::move(version_display_name)));
    graph.SetActiveVersionId(new_version_id);
  } catch (const std::exception& ex) {
    return SetError(error, ex.what());
  }
  if (!state->history->SelectVersion(new_version_id, error)) {
    RestoreLivePastePrior(*state, prior);
    return false;
  }

  // Persist the empty paste Version so crash recovery can Replay WAL onto it
  // before the next ordinary DuckDB journal materialization.
  if (auto pipeline_service = state_.PipelineService()) {
    std::string persistence_error;
    if (!pipeline_service->PersistEditorHistoryState(state->pipeline_guard, expected_materialized,
                                                     &persistence_error)) {
      RestoreLivePastePrior(*state, prior);
      return SetError(error, persistence_error.empty() ? "Paste Version persistence failed"
                                                      : persistence_error);
    }
  }

  state->committed_snapshot = state->root_snapshot;
  state->pending_before.clear();
  if (!AppendPackageEntriesToLiveHistory(*state, package, error)) {
    RestoreLivePastePrior(*state, prior);
    return false;
  }

  {
    std::unique_lock<std::mutex> render_lock(state->pipeline_guard->pipeline_->GetRenderLock());
    (void)alcedo::AdjustmentTransferService::Apply(*state->pipeline_guard->pipeline_, package);
  }

  alcedo::EditorRenderAdjustmentSnapshot next_snapshot;
  if (!SnapshotAtHead(state->root_snapshot, graph, state->history->working_head(), &next_snapshot,
                      error)) {
    RestoreLivePastePrior(*state, prior);
    return false;
  }
  state->committed_snapshot = std::move(next_snapshot);
  state->pipeline_guard->dirty_ = true;
  state->pipeline_guard->serialized_state_needs_writeback_ = true;
  state->recovered_head = false;

  result->pasted = true;
  result->new_version_id = new_version_id;
  result->prior_version_id = prior_version_id;
  result->new_head = state->history->working_head().value_or(alcedo::commit_hash_t{});
  return true;
}

auto EditorHistoryTransfer::CancelLivePaste(const alcedo::EditorHistoryGuardHandle& guard,
                                            const alcedo::version_ref_id_t& prior_version_id,
                                            const alcedo::version_ref_id_t& paste_version_id,
                                            std::string* error) -> bool {
  auto state = state_.EnsureWorkingState(guard.element_id, error);
  if (!state) return false;
  if (!state->pipeline_guard || !state->pipeline_guard->commit_graph_ || !state->history) {
    return SetError(error, "Editor history graph is unavailable");
  }
  if (prior_version_id == alcedo::version_ref_id_t{}) {
    return SetError(error, "Paste cancel requires the prior Version id");
  }
  if (paste_version_id == alcedo::version_ref_id_t{}) {
    return SetError(error, "Paste cancel requires the paste Version id");
  }

  auto& graph = *state->pipeline_guard->commit_graph_;
  const auto graph_before = graph;
  const auto prior_select = state->history->WorkingSelection();
  const bool prior_dirty = state->pipeline_guard->dirty_;
  const bool prior_serialized = state->pipeline_guard->serialized_state_needs_writeback_;
  const auto prior_snapshot = state->committed_snapshot;
  const auto prior_pending = state->pending_before;
  const bool prior_recovered = state->recovered_head;

  auto restore_working = [&] {
    // Restoring the graph restores logical head; no separate head field on the guard.
    graph = graph_before;
    state->history->PublishWorkingSelection(prior_select);
    state->pipeline_guard->dirty_ = prior_dirty;
    state->pipeline_guard->serialized_state_needs_writeback_ = prior_serialized;
    state->committed_snapshot = prior_snapshot;
    state->pending_before = prior_pending;
    state->recovered_head = prior_recovered;
  };

  try {
    graph.SetActiveVersionId(prior_version_id);
  } catch (const std::exception& ex) {
    return SetError(error, ex.what());
  }
  if (!state->history->SelectVersion(prior_version_id, error)) {
    restore_working();
    return false;
  }

  alcedo::EditorRenderAdjustmentSnapshot restored_snapshot;
  if (!SnapshotAtHead(state->root_snapshot, graph, graph.GetActiveVersionRef().head_commit_hash,
                      &restored_snapshot, error)) {
    restore_working();
    return false;
  }

  if (!graph.RemoveVersionRef(paste_version_id)) {
    restore_working();
    return SetError(error, "Paste Version could not be removed after cancel");
  }

  if (auto pipeline_service = state_.PipelineService()) {
    std::string persistence_error;
    if (!pipeline_service->PersistEditorHistoryState(state->pipeline_guard,
                                                     graph_before.GetImageEditState(),
                                                     &persistence_error)) {
      restore_working();
      return SetError(error, persistence_error.empty() ? "Paste cancel persistence failed"
                                                      : persistence_error);
    }
  }

  if (state->pipeline_guard->pipeline_) {
    std::unique_lock<std::mutex> render_lock(state->pipeline_guard->pipeline_->GetRenderLock());
    if (!alcedo::ApplyEditorAdjustmentSnapshot(*state->pipeline_guard->pipeline_, restored_snapshot,
                                               error)) {
      restore_working();
      return false;
    }
  }

  // Paste commits were journaled against the abandoned Version; drop the WAL so
  // the next ordinary materialize does not try to fold them onto the prior head.
  if (state->journal && !state->journal->TruncateMaterialized(error)) {
    restore_working();
    return false;
  }
  state->history->PublishWorkingSelection({});

  state->committed_snapshot = std::move(restored_snapshot);
  state->pipeline_guard->dirty_ = false;
  state->pipeline_guard->serialized_state_needs_writeback_ = false;
  state->pending_before.clear();
  state->recovered_head = false;
  return true;
}

namespace {

auto InferLiveMergeChoice(const alcedo::AdjustmentMergeResolution& resolution,
                          const alcedo::AdjustmentMergeConflict& conflict)
    -> alcedo::OperatorMergeChoice {
  if (resolution.choice.has_value()) {
    return *resolution.choice;
  }
  if (resolution.resolved_value == conflict.incoming_value) {
    return alcedo::OperatorMergeChoice::kTakeIncoming;
  }
  return alcedo::OperatorMergeChoice::kKeepCurrent;
}

}  // namespace

auto EditorHistoryTransfer::BeginLiveMerge(const alcedo::EditorHistoryGuardHandle& guard,
                                           const alcedo::AdjustmentTransferPackage& package,
                                           alcedo::AdjustmentMergePreview* preview,
                                           std::string* error) -> bool {
  auto state = state_.EnsureWorkingState(guard.element_id, error);
  if (!state) return false;
  if (preview == nullptr) return SetError(error, "Merge preview storage is required");
  *preview = {};
  if (package.Empty()) return SetError(error, "Adjustment transfer package is empty");
  if (!state->pipeline_guard || !state->pipeline_guard->commit_graph_ || !state->history) {
    return SetError(error, "Editor live merge requires a complete history state");
  }
  if (!state->pipeline_guard->pipeline_) {
    return SetError(error, "Editor live merge requires a live pipeline executor");
  }

  preview->first_parent_head = state->history->working_head();
  preview->source_package_fingerprint =
      alcedo::AdjustmentTransferService::PackageFingerprint(package);

  {
    std::unique_lock<std::mutex> render_lock(state->pipeline_guard->pipeline_->GetRenderLock());
    if (!alcedo::AdjustmentTransferService::DetectMergeConflicts(
            *state->pipeline_guard->pipeline_, package, &preview->conflicts, error)) {
      return false;
    }
  }
  preview->has_conflicts = !preview->conflicts.empty();
  // No shadow Version / incoming head until CompleteLiveMerge inserts ancestry commits.
  preview->incoming_version_id = {};
  preview->incoming_head       = {};
  return true;
}

auto EditorHistoryTransfer::CompleteLiveMerge(
    const alcedo::EditorHistoryGuardHandle& guard,
    const alcedo::AdjustmentTransferPackage& package,
    const alcedo::AdjustmentMergePreview& preview,
    const std::vector<alcedo::AdjustmentMergeResolution>& resolutions,
    alcedo::AdjustmentMergeResult* result, std::string* error) -> bool {
  auto state = state_.EnsureWorkingState(guard.element_id, error);
  if (!state) return false;
  if (result == nullptr) return SetError(error, "Merge result storage is required");
  *result = {};
  if (!preview.error.empty()) {
    return SetError(error, "Cannot complete a merge that failed to initiate: " + preview.error);
  }
  if (package.Empty()) return SetError(error, "Adjustment transfer package is empty");
  if (!state->pipeline_guard || !state->pipeline_guard->commit_graph_ || !state->history ||
      !state->journal) {
    return SetError(error, "Editor live merge requires a complete history state");
  }
  if (!state->pipeline_guard->pipeline_) {
    return SetError(error, "Editor live merge requires a live pipeline executor");
  }
  if (preview.source_package_fingerprint !=
      alcedo::AdjustmentTransferService::PackageFingerprint(package)) {
    return SetError(error, "Merge source package no longer matches the preview");
  }
  if (state->history->working_head() != preview.first_parent_head) {
    return SetError(error, "Merge preview is stale because the active history changed");
  }
  if (preview.has_conflicts && resolutions.empty()) {
    return SetError(error, "Merge has conflicts but no resolutions were provided");
  }

  auto& graph = *state->pipeline_guard->commit_graph_;
  const auto prior = CaptureLivePastePrior(*state);
  const auto expected_materialized = prior.graph.GetImageEditState();

  // Insert incoming ancestry commits (no user-visible Version ref). Shared
  // core: AdjustmentTransferService builds and inserts the root-relative chain.
  alcedo::commit_hash_t incoming_head_value;
  try {
    auto incoming_head =
        alcedo::AdjustmentTransferService::InsertIncomingAncestryCommits(graph, package);
    if (!incoming_head.has_value()) {
      return SetError(error, "No valid adjustments in merge package");
    }
    incoming_head_value = *incoming_head;
  } catch (const std::exception& ex) {
    RestoreLivePastePrior(*state, prior);
    return SetError(error, ex.what());
  }
  const auto incoming_head = incoming_head_value;

  // Persist ancestry commits so WAL recovery of the merge commit can resolve the
  // second parent after a crash (materialized head remains pre-merge).
  if (auto pipeline_service = state_.PipelineService()) {
    std::string persistence_error;
    if (!pipeline_service->PersistEditorHistoryState(state->pipeline_guard, expected_materialized,
                                                     &persistence_error)) {
      RestoreLivePastePrior(*state, prior);
      return SetError(error, persistence_error.empty() ? "Merge ancestry persistence failed"
                                                      : persistence_error);
    }
  }

  // Live parameter-table apply: one SetOperator/enable per resolved field. These intermediate
  // mutations do not advance history or fold transaction_chain_hash. Below, PrepareAppendMerge +
  // PublishPreparedEdit record a single merge commit and fold the chain hash once.
  alcedo::MergeEditPayload merge_payload;
  std::unordered_set<std::string> resolved_keys;
  {
    std::unique_lock<std::mutex> render_lock(state->pipeline_guard->pipeline_->GetRenderLock());
    auto& pipeline = *state->pipeline_guard->pipeline_;
    for (const auto& resolution : resolutions) {
      if (!resolved_keys.insert(resolution.field_key).second) continue;
      const alcedo::AdjustmentMergeConflict* conflict = nullptr;
      for (const auto& c : preview.conflicts) {
        if (c.field_key == resolution.field_key) {
          conflict = &c;
          break;
        }
      }
      if (conflict == nullptr) continue;

      const auto choice = InferLiveMergeChoice(resolution, *conflict);
      auto delta = alcedo::AdjustmentTransferService::BuildMergeFieldDelta(pipeline, *conflict,
                                                                            choice);
      merge_payload.fields.push_back(delta);

      // Live parameter-table apply (session-specific): one SetOperator/enable per
      // resolved field. These intermediate mutations do not advance history;
      // PrepareAppendMerge + PublishPreparedEdit below record one merge commit.
      auto&      stage   = pipeline.GetStage(conflict->stage);
      auto&      globals = pipeline.GetGlobalParams();
      if (!delta.resolved_value.is_null() && delta.resolved_value.is_object()) {
        stage.SetOperator(conflict->operator_type, delta.resolved_value, globals);
      }
      stage.EnableOperator(conflict->operator_type, delta.resolved_enabled, globals);
    }
  }

  if (preview.has_conflicts && merge_payload.fields.size() != preview.conflicts.size()) {
    RestoreLivePastePrior(*state, prior);
    return SetError(error, "Not all merge conflicts were resolved");
  }

  const auto prepared = state->history->PrepareAppendMerge(incoming_head, std::move(merge_payload));
  if (!prepared.ready) {
    RestoreLivePastePrior(*state, prior);
    return SetError(error, prepared.error.empty() ? "Merge prepare failed" : prepared.error);
  }
  const auto appended = state->history->PublishPreparedEdit(prepared);
  if (!appended.committed) {
    RestoreLivePastePrior(*state, prior);
    return SetError(error, appended.error.empty() ? "Merge WAL append failed" : appended.error);
  }

  alcedo::EditorRenderAdjustmentSnapshot next_snapshot;
  if (!SnapshotAtHead(state->root_snapshot, graph, state->history->working_head(), &next_snapshot,
                      error)) {
    RestoreLivePastePrior(*state, prior);
    return false;
  }
  state->committed_snapshot = std::move(next_snapshot);
  state->pipeline_guard->dirty_ = true;
  state->pipeline_guard->serialized_state_needs_writeback_ = true;
  state->pending_before.clear();
  state->recovered_head = false;

  result->merged            = true;
  result->active_version_id = graph.GetActiveVersionId();
  result->merge_commit_hash = appended.commit->GetCommitHash();
  return true;
}

auto EditorHistoryTransfer::CancelMerge(const alcedo::EditorHistoryGuardHandle& /*guard*/,
                                        const alcedo::AdjustmentMergePreview& /*preview*/,
                                        std::string* /*error*/) -> bool {
  // BeginLiveMerge does not stage a shadow graph or temporary Version; cancel is
  // owned by the session (clear package / preview ids). History port is a no-op.
  return true;
}

}  // namespace alcedo::ui
