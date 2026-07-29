//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/editor_history_transfer.hpp"

#include <optional>
#include <utility>

#include "app/adjustment_transfer_service.hpp"
#include "app/editor_mini_git_materializer.hpp"
#include "edit/history/commit_graph.hpp"
#include "edit/history/mini_git_working_history.hpp"
#include "ui/alcedo_main/album_backend/editor_history_shared_helpers.hpp"
#include "ui/alcedo_main/album_backend/editor_history_state_detail.hpp"

namespace alcedo::ui {
namespace {

auto SetError(std::string* error, std::string message) -> bool {
  if (error != nullptr) *error = std::move(message);
  return false;
}

auto FindCandidate(HistoryWorkingState& state, const alcedo::EditorTransferCandidate& candidate,
                   std::string* error) -> HistoryTransferCandidate* {
  if (!candidate.valid()) return nullptr;
  const auto it = state.transfer_candidates.find(candidate.candidate_id);
  if (it == state.transfer_candidates.end()) {
    SetError(error, "Editor transfer candidate is no longer available");
    return nullptr;
  }
  return &it->second;
}

auto FindCandidateForPreview(HistoryWorkingState& state,
                             const alcedo::AdjustmentMergePreview& preview,
                             std::string* error) -> std::optional<alcedo::EditorTransferCandidate> {
  for (auto& [unused_id, candidate] : state.transfer_candidates) {
    (void)unused_id;
    if (!candidate.merge) continue;
    try {
      const auto& incoming = candidate.graph.GetVersionRef(preview.incoming_version_id);
      if (incoming.head_commit_hash == preview.incoming_head) {
        return candidate.public_candidate;
      }
    } catch (const std::exception&) {
      continue;
    }
  }
  SetError(error, "Editor merge preview candidate is no longer available");
  return std::nullopt;
}

auto CaptureBase(const HistoryWorkingState& state, HistoryTransferCandidate* candidate,
                 std::string* error) -> bool {
  if (candidate == nullptr || !state.pipeline_guard || !state.pipeline_guard->commit_graph_ ||
      !state.history || !state.journal) {
    return SetError(error, "Editor transfer candidate requires a complete history state");
  }
  const auto& graph = *state.pipeline_guard->commit_graph_;
  candidate->base_active_version_id = graph.GetActiveVersionId();
  candidate->base_working_head = state.history->working_head();
  candidate->base_working_transaction_chain_hash = state.history->transaction_chain_hash();
  const auto& image_state = graph.GetImageEditState();
  candidate->base_materialized_head = image_state.materialized_head_commit_hash;
  candidate->base_materialized_transaction_chain_hash =
      image_state.materialized_transaction_chain_hash;
  candidate->graph = graph;
  return true;
}

auto MakeCandidateId(HistoryWorkingState& state) -> std::uint64_t {
  auto id = state.next_transfer_candidate_id++;
  if (id == 0) id = state.next_transfer_candidate_id++;
  return id;
}

}  // namespace

EditorHistoryTransfer::EditorHistoryTransfer(EditorHistoryState& state) : state_(state) {}

auto EditorHistoryTransfer::PreparePaste(
    const alcedo::EditorHistoryGuardHandle& guard,
    const alcedo::AdjustmentTransferPackage& package, std::string version_display_name,
    alcedo::AdjustmentPasteResult* result, alcedo::EditorTransferCandidate* candidate,
    std::string* error) -> bool {
  auto state = state_.EnsureWorkingState(guard.element_id, error);
  if (!state) return false;
  if (candidate == nullptr) return SetError(error, "Paste candidate storage is required");
  if (package.Empty()) return SetError(error, "Adjustment transfer package is empty");

  HistoryTransferCandidate staged;
  if (!CaptureBase(*state, &staged, error)) return false;
  staged.public_candidate.candidate_id = MakeCandidateId(*state);
  staged.public_candidate.package       = package;
  staged.public_candidate.display_name  = std::move(version_display_name);
  staged.merge                         = false;

  const auto paste_result = alcedo::AdjustmentTransferService::PasteAsRootRelativeVersion(
      staged.graph, package, staged.public_candidate.display_name);
  if (!paste_result.pasted) return SetError(error, paste_result.error);
  if (!SnapshotAtHead(state->root_snapshot, staged.graph, paste_result.new_head,
                      &staged.adjustment_snapshot, error)) {
    return false;
  }
  staged.public_candidate.adjustment_snapshot = staged.adjustment_snapshot;
  state->transfer_candidates.emplace(staged.public_candidate.candidate_id, staged);
  *candidate = staged.public_candidate;
  if (result != nullptr) *result = paste_result;
  return true;
}

auto EditorHistoryTransfer::PrepareMerge(
    const alcedo::EditorHistoryGuardHandle& guard,
    const alcedo::AdjustmentTransferPackage& package, std::string incoming_version_display_name,
    alcedo::AdjustmentMergePreview* preview, alcedo::EditorTransferCandidate* candidate,
    std::string* error) -> bool {
  auto state = state_.EnsureWorkingState(guard.element_id, error);
  if (!state) return false;
  if (preview == nullptr) return SetError(error, "Merge preview storage is required");
  if (candidate == nullptr) return SetError(error, "Merge candidate storage is required");
  if (package.Empty()) return SetError(error, "Adjustment transfer package is empty");

  HistoryTransferCandidate staged;
  if (!CaptureBase(*state, &staged, error)) return false;
  staged.public_candidate.candidate_id = MakeCandidateId(*state);
  staged.public_candidate.package       = package;
  staged.public_candidate.display_name  = incoming_version_display_name;
  staged.public_candidate.adjustment_snapshot = state->committed_snapshot;
  staged.merge = true;

  auto merge_preview = alcedo::AdjustmentTransferService::InitiateMerge(
      staged.graph, package, state->committed_snapshot, std::move(incoming_version_display_name));
  if (!merge_preview.error.empty()) return SetError(error, merge_preview.error);
  merge_preview.source_package_fingerprint =
      alcedo::AdjustmentTransferService::PackageFingerprint(package);
  merge_preview.first_parent_head = staged.base_working_head;
  *preview = merge_preview;
  staged.adjustment_snapshot = state->committed_snapshot;
  state->transfer_candidates.emplace(staged.public_candidate.candidate_id, staged);
  *candidate = staged.public_candidate;
  return true;
}

auto EditorHistoryTransfer::ValidateMergeCandidate(
    const alcedo::EditorHistoryGuardHandle& guard,
    const alcedo::AdjustmentMergePreview& preview,
    const alcedo::EditorTransferCandidate& candidate, std::string* error) -> bool {
  auto state = state_.EnsureWorkingState(guard.element_id, error);
  if (!state) return false;
  auto* staged = FindCandidate(*state, candidate, error);
  if (staged == nullptr) return false;
  if (!staged->merge) return SetError(error, "Editor transfer candidate is not a Merge");
  if (preview.preview_id != alcedo::MergePreviewId{} &&
      candidate.preview_id != preview.preview_id) {
    return SetError(error, "Merge preview identity no longer matches the candidate");
  }
  if (preview.source_package_fingerprint !=
      alcedo::AdjustmentTransferService::PackageFingerprint(candidate.package)) {
    return SetError(error, "Merge source package no longer matches the preview");
  }
  if (!state->pipeline_guard || !state->pipeline_guard->commit_graph_ || !state->history) {
    return SetError(error, "Editor history graph is unavailable");
  }
  const auto& live_graph = *state->pipeline_guard->commit_graph_;
  if (live_graph.GetActiveVersionId() != staged->base_active_version_id ||
      state->history->working_head() != staged->base_working_head ||
      state->history->transaction_chain_hash() != staged->base_working_transaction_chain_hash) {
    return SetError(error, "Merge preview is stale because the active history changed");
  }
  if (preview.first_parent_head != staged->base_working_head) {
    return SetError(error, "Merge preview first parent no longer matches the active head");
  }
  try {
    const auto& incoming = staged->graph.GetVersionRef(preview.incoming_version_id);
    if (incoming.head_commit_hash != preview.incoming_head) {
      return SetError(error, "Merge preview incoming branch no longer matches");
    }
  } catch (const std::exception&) {
    return SetError(error, "Merge preview incoming branch is unavailable");
  }
  return true;
}

auto EditorHistoryTransfer::CompleteMergeCandidate(
    const alcedo::EditorHistoryGuardHandle& guard,
    const alcedo::AdjustmentMergePreview& preview,
    const std::vector<alcedo::AdjustmentMergeResolution>& resolutions,
    alcedo::EditorTransferCandidate* candidate, alcedo::AdjustmentMergeResult* result,
    std::string* error) -> bool {
  if (candidate == nullptr) return SetError(error, "Merge candidate storage is required");
  if (!ValidateMergeCandidate(guard, preview, *candidate, error)) return false;
  auto state = state_.EnsureWorkingState(guard.element_id, error);
  if (!state) return false;
  auto* staged = FindCandidate(*state, *candidate, error);
  if (staged == nullptr) return false;

  const auto preview_id = candidate->preview_id;
  const auto merge_result =
      alcedo::AdjustmentTransferService::CompleteMerge(staged->graph, preview, resolutions);
  if (!merge_result.merged) return SetError(error, merge_result.error);
  if (!SnapshotAtHead(state->root_snapshot, staged->graph,
                      staged->graph.GetActiveVersionRef().head_commit_hash,
                      &staged->adjustment_snapshot, error)) {
    return false;
  }
  staged->public_candidate.preview_id = preview_id;
  staged->public_candidate.adjustment_snapshot = staged->adjustment_snapshot;
  *candidate = staged->public_candidate;
  if (result != nullptr) *result = merge_result;
  return true;
}

auto EditorHistoryTransfer::CaptureTransferSaveCheckpoint(
    const alcedo::EditorHistoryGuardHandle& guard,
    const alcedo::EditorTransferCandidate& candidate, std::string* error)
    -> std::shared_ptr<const alcedo::EditorMiniGitSaveCapture> {
  auto state = state_.EnsureWorkingState(guard.element_id, error);
  if (!state) return nullptr;
  auto* staged = FindCandidate(*state, candidate, error);
  if (staged == nullptr) return nullptr;
  if (!state->pipeline_guard || !state->pipeline_guard->commit_graph_ || !state->history ||
      !state->journal) {
    if (error) *error = "Editor transfer save capture requires a complete history state";
    return nullptr;
  }

  const auto& live_graph = *state->pipeline_guard->commit_graph_;
  if (live_graph.GetActiveVersionId() != staged->base_active_version_id ||
      state->history->working_head() != staged->base_working_head ||
      state->history->transaction_chain_hash() != staged->base_working_transaction_chain_hash) {
    if (error) *error = "Editor transfer candidate became stale before save capture";
    return nullptr;
  }

  const auto journal_snapshot = state->journal->Snapshot();
  alcedo::EditorMiniGitSaveCapture capture;
  capture.element_id = guard.element_id;
  capture.version_id = staged->graph.GetActiveVersionId();
  capture.root_id = staged->graph.GetRootId();
  capture.working_head = staged->graph.GetActiveVersionRef().head_commit_hash;
  capture.transaction_chain_hash = staged->graph.ChainHashForHead(capture.working_head);
  capture.journal_records = journal_snapshot.records;
  capture.journal_path = state->journal->path();
  capture.first_journal_sequence = journal_snapshot.first_sequence;
  capture.last_journal_sequence = journal_snapshot.last_sequence;
  capture.candidate_publication = true;
  capture.base_active_version_id = staged->base_active_version_id;
  capture.base_working_head = staged->base_working_head;
  capture.base_working_transaction_chain_hash = staged->base_working_transaction_chain_hash;
  capture.base_materialized_head = staged->base_materialized_head;
  capture.base_materialized_transaction_chain_hash =
      staged->base_materialized_transaction_chain_hash;

  std::string params_error;
  const auto pipeline_params =
      MakePipelineParamsFromSnapshot(staged->adjustment_snapshot, &params_error);
  if (!pipeline_params.has_value()) {
    if (error) *error = params_error;
    return nullptr;
  }
  const auto serialized = alcedo::MakeEditorSerializedPipelineState(
      capture.root_id, capture.working_head, capture.transaction_chain_hash, *pipeline_params);
  try {
    capture.materialization =
        staged->graph.CaptureMaterializationWithSerializedPipelineState(serialized);
  } catch (const std::exception& ex) {
    if (error) *error = ex.what();
    return nullptr;
  }
  return std::make_shared<const alcedo::EditorMiniGitSaveCapture>(std::move(capture));
}

auto EditorHistoryTransfer::PublishTransferCandidate(
    const alcedo::EditorHistoryGuardHandle& guard,
    const alcedo::EditorTransferCandidate& candidate,
    const alcedo::AdjustmentMergePreview* preview,
    const std::vector<alcedo::AdjustmentMergeResolution>& /*resolutions*/,
    alcedo::AdjustmentPasteResult* paste, alcedo::AdjustmentMergeResult* merge,
    std::string* error) -> bool {
  auto state = state_.EnsureWorkingState(guard.element_id, error);
  if (!state) return false;
  auto* staged = FindCandidate(*state, candidate, error);
  if (staged == nullptr) return false;
  if ((preview != nullptr) != staged->merge) {
    return SetError(error, "Editor transfer candidate kind does not match publication");
  }
  if (!state->pipeline_guard || !state->pipeline_guard->commit_graph_ || !state->history) {
    return SetError(error, "Editor history graph is unavailable");
  }

  std::string params_error;
  const auto pipeline_params =
      MakePipelineParamsFromSnapshot(staged->adjustment_snapshot, &params_error);
  if (!pipeline_params.has_value()) return SetError(error, params_error);
  const auto final_head = staged->graph.GetActiveVersionRef().head_commit_hash;
  const auto final_chain = staged->graph.ChainHashForHead(final_head);
  const auto serialized = alcedo::MakeEditorSerializedPipelineState(
      staged->graph.GetRootId(), final_head, final_chain, *pipeline_params);

  alcedo::CommitGraph published_graph = staged->graph;
  try {
    const auto materialization =
        published_graph.CaptureMaterializationWithSerializedPipelineState(serialized);
    published_graph.ApplyMaterializedState(materialization.image_state);
  } catch (const std::exception& ex) {
    return SetError(error, ex.what());
  }

  *state->pipeline_guard->commit_graph_ = std::move(published_graph);
  state->history->PublishWorkingSelection({});
  state->pipeline_guard->working_head_commit_hash_ = state->history->working_head();
  state->pipeline_guard->transaction_chain_hash_ = state->history->transaction_chain_hash();
  state->pipeline_guard->dirty_ = false;
  state->pipeline_guard->serialized_state_needs_writeback_ = false;
  state->committed_snapshot = staged->adjustment_snapshot;
  state->pending_before.clear();
  state->recovered_head = false;

  if (staged->merge) {
    if (merge != nullptr) {
      merge->merged = true;
      merge->active_version_id = state->pipeline_guard->commit_graph_->GetActiveVersionId();
      merge->merge_commit_hash = state->history->working_head().value_or(commit_hash_t{});
    }
  } else if (paste != nullptr) {
    paste->pasted = true;
    paste->new_version_id = state->pipeline_guard->commit_graph_->GetActiveVersionId();
    paste->new_head = state->history->working_head().value_or(commit_hash_t{});
  }
  state->transfer_candidates.erase(candidate.candidate_id);
  return true;
}

auto EditorHistoryTransfer::DiscardTransferCandidate(
    const alcedo::EditorHistoryGuardHandle& guard,
    const alcedo::EditorTransferCandidate& candidate, std::string* error) -> bool {
  auto state = state_.EnsureWorkingState(guard.element_id, error);
  if (!state) return false;
  if (!candidate.valid()) return true;
  const auto it = state->transfer_candidates.find(candidate.candidate_id);
  if (it == state->transfer_candidates.end()) return true;
  state->transfer_candidates.erase(it);
  return true;
}

auto EditorHistoryTransfer::PasteAdjustments(const alcedo::EditorHistoryGuardHandle& guard,
                                             const alcedo::AdjustmentTransferPackage& package,
                                             std::string version_display_name,
                                             alcedo::AdjustmentPasteResult* result,
                                             std::string* error) -> bool {
  alcedo::EditorTransferCandidate candidate;
  if (!PreparePaste(guard, package, std::move(version_display_name), result, &candidate, error)) {
    return false;
  }
  if (!PublishTransferCandidate(guard, candidate, nullptr, {}, result, nullptr, error)) {
    (void)DiscardTransferCandidate(guard, candidate, nullptr);
    return false;
  }
  return true;
}

auto EditorHistoryTransfer::BeginMerge(const alcedo::EditorHistoryGuardHandle& guard,
                                       const alcedo::AdjustmentTransferPackage& package,
                                       std::string incoming_version_display_name,
                                       alcedo::AdjustmentMergePreview* preview,
                                       std::string* error) -> bool {
  alcedo::EditorTransferCandidate candidate;
  return PrepareMerge(guard, package, std::move(incoming_version_display_name), preview,
                      &candidate, error);
}

auto EditorHistoryTransfer::CompleteMerge(
    const alcedo::EditorHistoryGuardHandle& guard,
    const alcedo::AdjustmentMergePreview& preview,
    const std::vector<alcedo::AdjustmentMergeResolution>& resolutions,
    alcedo::AdjustmentMergeResult* result, std::string* error) -> bool {
  auto state = state_.EnsureWorkingState(guard.element_id, error);
  if (!state) return false;
  auto candidate = FindCandidateForPreview(*state, preview, error);
  if (!candidate.has_value()) return false;
  if (!CompleteMergeCandidate(guard, preview, resolutions, &*candidate, result, error)) {
    return false;
  }
  if (!PublishTransferCandidate(guard, *candidate, &preview, resolutions, nullptr, result,
                                 error)) {
    (void)DiscardTransferCandidate(guard, *candidate, nullptr);
    return false;
  }
  return true;
}

auto EditorHistoryTransfer::CancelMerge(const alcedo::EditorHistoryGuardHandle& guard,
                                        const alcedo::AdjustmentMergePreview& preview,
                                        std::string* error) -> bool {
  auto state = state_.EnsureWorkingState(guard.element_id, error);
  if (!state) return false;
  auto candidate = FindCandidateForPreview(*state, preview, error);
  if (!candidate.has_value()) return false;
  return DiscardTransferCandidate(guard, *candidate, error);
}

}  // namespace alcedo::ui
