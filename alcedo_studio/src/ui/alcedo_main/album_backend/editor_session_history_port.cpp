//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/editor_session_history_port.hpp"

#include <functional>

#include "app/editor_session_types.hpp"
#include "ui/alcedo_main/album_backend/editor_history_checkpoint.hpp"
#include "ui/alcedo_main/album_backend/editor_history_mutation.hpp"
#include "ui/alcedo_main/album_backend/editor_history_projection.hpp"
#include "ui/alcedo_main/album_backend/editor_history_state_detail.hpp"
#include "ui/alcedo_main/album_backend/editor_history_transfer.hpp"
#include "ui/alcedo_main/album_backend/editor_history_version_refs.hpp"
#include "ui/alcedo_main/album_backend/editor_session_pipeline_port.hpp"

namespace alcedo::ui {

EditorSessionHistoryPort::EditorSessionHistoryPort()
    : state_(std::make_unique<EditorHistoryState>()),
      projection_(std::make_unique<EditorHistoryProjection>(*state_)),
      mutation_(std::make_unique<EditorHistoryMutation>(*state_)),
      version_refs_(std::make_unique<EditorHistoryVersionRefs>(*state_)),
      transfer_(std::make_unique<EditorHistoryTransfer>(*state_)),
      checkpoint_(std::make_unique<EditorHistoryCheckpoint>(*state_)) {}

EditorSessionHistoryPort::~EditorSessionHistoryPort() = default;

void EditorSessionHistoryPort::SetServices(Services services) {
  EditorHistoryState::Services s;
  s.mini_git_journal_path = std::move(services.mini_git_journal_path);
  state_->SetServices(std::move(s));
}

void EditorSessionHistoryPort::SetPipelinePort(
    std::shared_ptr<EditorSessionPipelinePort> pipeline_port) {
  state_->SetPipelinePort(std::move(pipeline_port));
}

auto EditorSessionHistoryPort::Acquire(sl_element_id_t element_id, std::string* error)
    -> alcedo::EditorHistoryGuardHandle {
  auto journal_path = state_->JournalPathResolver();
  if (journal_path) {
    std::string prepare_error;
    if (!state_->EnsureWorkingState(element_id, &prepare_error)) {
      if (error)
        *error = prepare_error.empty() ? "Editor Mini-Git history initialization failed"
                                       : std::move(prepare_error);
      return {};
    }
  }
  return {element_id, true};
}

void EditorSessionHistoryPort::Release(const alcedo::EditorHistoryGuardHandle& guard) {
  if (!guard.valid) return;
  state_->ReleaseState(guard.element_id);
}

auto EditorSessionHistoryPort::CaptureAdjustmentBeforePreview(
    const alcedo::EditorHistoryGuardHandle& guard, const alcedo::EditorAdjustmentPatch& patch,
    std::string* error) -> bool {
  return mutation_->CaptureAdjustmentBeforePreview(guard, patch, error);
}

auto EditorSessionHistoryPort::CommitAdjustment(const alcedo::EditorHistoryGuardHandle& guard,
                                                const alcedo::EditorAdjustmentPatch& patch,
                                                std::string* error) -> bool {
  return mutation_->CommitAdjustment(guard, patch, error);
}

auto EditorSessionHistoryPort::Undo(const alcedo::EditorHistoryGuardHandle& guard,
                                    std::string* error) -> bool {
  return mutation_->Undo(guard, error);
}

auto EditorSessionHistoryPort::Redo(const alcedo::EditorHistoryGuardHandle& guard,
                                    std::string* error) -> bool {
  return mutation_->Redo(guard, error);
}

auto EditorSessionHistoryPort::MoveHeadToCommit(const alcedo::EditorHistoryGuardHandle& guard,
                                                const alcedo::commit_hash_t& commit_id,
                                                std::string* error) -> bool {
  return mutation_->MoveHeadToCommit(guard, commit_id, error);
}

auto EditorSessionHistoryPort::CheckoutVersion(const alcedo::EditorHistoryGuardHandle& guard,
                                               const alcedo::Hash128& version_id,
                                               std::string* error) -> bool {
  return mutation_->CheckoutVersion(guard, version_id, error);
}

auto EditorSessionHistoryPort::ReadHistorySnapshot(const alcedo::EditorHistoryGuardHandle& guard,
                                                   alcedo::EditorHistorySnapshot* snapshot,
                                                   std::string* error) -> bool {
  return projection_->ReadHistorySnapshot(guard, snapshot, error);
}

auto EditorSessionHistoryPort::CreateRootVersionAndCheckout(
    const alcedo::EditorHistoryGuardHandle& guard, std::string display_name,
    alcedo::version_ref_id_t* version_id, std::string* error) -> bool {
  return version_refs_->CreateRootVersionAndCheckout(guard, std::move(display_name), version_id,
                                                     error);
}

auto EditorSessionHistoryPort::BranchFromCommitAndCheckout(
    const alcedo::EditorHistoryGuardHandle& guard, const alcedo::commit_hash_t& commit_id,
    std::string display_name, alcedo::version_ref_id_t* version_id, std::string* error) -> bool {
  return version_refs_->BranchFromCommitAndCheckout(guard, commit_id, std::move(display_name),
                                                    version_id, error);
}

auto EditorSessionHistoryPort::RenameVersion(const alcedo::EditorHistoryGuardHandle& guard,
                                             const alcedo::Hash128& version_id,
                                             std::string display_name, std::string* error)
    -> bool {
  return version_refs_->RenameVersion(guard, version_id, std::move(display_name), error);
}

auto EditorSessionHistoryPort::RemoveVersion(const alcedo::EditorHistoryGuardHandle& guard,
                                             const alcedo::Hash128& version_id,
                                             std::string* error) -> bool {
  return version_refs_->RemoveVersion(guard, version_id, error);
}

auto EditorSessionHistoryPort::PasteAdjustments(
    const alcedo::EditorHistoryGuardHandle& guard,
    const alcedo::AdjustmentTransferPackage& package, std::string version_display_name,
    alcedo::AdjustmentPasteResult* result, std::string* error) -> bool {
  return transfer_->PasteAdjustments(guard, package, std::move(version_display_name), result,
                                     error);
}

auto EditorSessionHistoryPort::BeginMerge(const alcedo::EditorHistoryGuardHandle& guard,
                                          const alcedo::AdjustmentTransferPackage& package,
                                          std::string incoming_version_display_name,
                                          alcedo::AdjustmentMergePreview* preview,
                                          std::string* error) -> bool {
  return transfer_->BeginMerge(guard, package, std::move(incoming_version_display_name), preview,
                               error);
}

auto EditorSessionHistoryPort::CompleteMerge(
    const alcedo::EditorHistoryGuardHandle& guard, const alcedo::AdjustmentMergePreview& preview,
    const std::vector<alcedo::AdjustmentMergeResolution>& resolutions,
    alcedo::AdjustmentMergeResult* result, std::string* error) -> bool {
  return transfer_->CompleteMerge(guard, preview, resolutions, result, error);
}

auto EditorSessionHistoryPort::CancelMerge(const alcedo::EditorHistoryGuardHandle& guard,
                                           const alcedo::AdjustmentMergePreview& preview,
                                           std::string* error) -> bool {
  return transfer_->CancelMerge(guard, preview, error);
}

auto EditorSessionHistoryPort::ReadAdjustmentSnapshot(
    const alcedo::EditorHistoryGuardHandle& guard, alcedo::EditorRenderAdjustmentSnapshot* snapshot,
    std::string* error) -> bool {
  return projection_->ReadAdjustmentSnapshot(guard, snapshot, error);
}

auto EditorSessionHistoryPort::CaptureSaveCheckpoint(const alcedo::EditorHistoryGuardHandle& guard,
                                                     std::string* error)
    -> std::shared_ptr<const alcedo::EditorMiniGitSaveCapture> {
  return checkpoint_->CaptureSaveCheckpoint(guard, error);
}

auto EditorSessionHistoryPort::DiscardMaterializedJournalThrough(
    const alcedo::EditorHistoryGuardHandle& guard, std::uint64_t last_sequence,
    std::string* error) -> bool {
  return checkpoint_->DiscardMaterializedJournalThrough(guard, last_sequence, error);
}

}  // namespace alcedo::ui
