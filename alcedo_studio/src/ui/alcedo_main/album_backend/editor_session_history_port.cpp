//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/editor_session_history_port.hpp"

#include <functional>
#include <optional>

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

auto EditorSessionHistoryPort::CommitPipelineEditBatch(const alcedo::EditorHistoryGuardHandle& guard,
                                                       alcedo::PipelineEditBatch batch,
                                                       std::string* error) -> bool {
  return mutation_->CommitPipelineEditBatch(guard, std::move(batch), error);
}

auto EditorSessionHistoryPort::AddColorGrade(const alcedo::EditorHistoryGuardHandle& guard,
                                             const alcedo::NodeId& before_node_id,
                                             const alcedo::NodeId& new_id, std::string* error)
    -> bool {
  return mutation_->AddColorGrade(guard, before_node_id, new_id, error);
}

auto EditorSessionHistoryPort::RemoveColorGrade(const alcedo::EditorHistoryGuardHandle& guard,
                                                const alcedo::NodeId& node_id, std::string* error)
    -> bool {
  return mutation_->RemoveColorGrade(guard, node_id, error);
}

auto EditorSessionHistoryPort::ReconnectColorGrade(const alcedo::EditorHistoryGuardHandle& guard,
                                                   const alcedo::NodeId& node_id,
                                                   const alcedo::NodeId& new_predecessor_id,
                                                   const alcedo::NodeId& new_successor_id,
                                                   std::string* error) -> bool {
  return mutation_->ReconnectColorGrade(guard, node_id, new_predecessor_id, new_successor_id, error);
}

auto EditorSessionHistoryPort::EditNodeGraph(const alcedo::EditorHistoryGuardHandle& guard,
                                             alcedo::NodeGraphTopologyChange change,
                                             std::string* error) -> bool {
  return mutation_->EditNodeGraph(guard, std::move(change), error);
}

auto EditorSessionHistoryPort::RenameColorGrade(const alcedo::EditorHistoryGuardHandle& guard,
                                                const alcedo::NodeId& node_id,
                                                std::string display_name, std::string* error)
    -> bool {
  return mutation_->RenameColorGrade(guard, node_id, std::move(display_name), error);
}

auto EditorSessionHistoryPort::SetColorGradeEnabled(const alcedo::EditorHistoryGuardHandle& guard,
                                                    const alcedo::NodeId& node_id, bool enabled,
                                                    std::string* error) -> bool {
  return mutation_->SetColorGradeEnabled(guard, node_id, enabled, error);
}

auto EditorSessionHistoryPort::SetColorGradeMix(const alcedo::EditorHistoryGuardHandle& guard,
                                                const alcedo::NodeId& node_id, float mix,
                                                std::string* error) -> bool {
  return mutation_->SetColorGradeMix(guard, node_id, mix, error);
}

auto EditorSessionHistoryPort::AddMask(const alcedo::EditorHistoryGuardHandle& guard,
                                       const alcedo::NodeId& node_id, alcedo::MaskModel mask,
                                       std::uint32_t display_index, std::string* error) -> bool {
  return mutation_->AddMask(guard, node_id, std::move(mask), display_index, error);
}

auto EditorSessionHistoryPort::RemoveMask(const alcedo::EditorHistoryGuardHandle& guard,
                                          const alcedo::NodeId& node_id,
                                          const alcedo::MaskId& mask_id, std::string* error)
    -> bool {
  return mutation_->RemoveMask(guard, node_id, mask_id, error);
}

auto EditorSessionHistoryPort::ReplaceMaskSource(const alcedo::EditorHistoryGuardHandle& guard,
                                                 const alcedo::NodeId& node_id,
                                                 const alcedo::MaskId& mask_id,
                                                 nlohmann::json after_source, std::string* error)
    -> bool {
  return mutation_->ReplaceMaskSource(guard, node_id, mask_id, std::move(after_source), error);
}

auto EditorSessionHistoryPort::ReplaceMaskAsset(const alcedo::EditorHistoryGuardHandle& guard,
                                                const alcedo::NodeId& node_id,
                                                const alcedo::MaskId& mask_id,
                                                nlohmann::json after_source,
                                                alcedo::MaskStore& mask_store, std::string* error)
    -> bool {
  return mutation_->ReplaceMaskAsset(guard, node_id, mask_id, std::move(after_source), mask_store,
                                     error);
}

auto EditorSessionHistoryPort::SetMaskField(const alcedo::EditorHistoryGuardHandle& guard,
                                            const alcedo::NodeId& node_id,
                                            const alcedo::MaskId& mask_id, std::string field_key,
                                            nlohmann::json after_value, std::string* error) -> bool {
  return mutation_->SetMaskField(guard, node_id, mask_id, std::move(field_key),
                                 std::move(after_value), error);
}

auto EditorSessionHistoryPort::Undo(const alcedo::EditorHistoryGuardHandle& guard,
                                    std::string* error) -> bool {
  return mutation_->Undo(guard, error);
}

auto EditorSessionHistoryPort::Redo(const alcedo::EditorHistoryGuardHandle& guard,
                                    std::string* error) -> bool {
  return mutation_->Redo(guard, error);
}

auto EditorSessionHistoryPort::LastPublishedRenderReason() const
    -> std::optional<alcedo::EditorRenderReason> {
  return state_->LastPublishedRenderReason();
}

auto EditorSessionHistoryPort::MoveHeadToCommit(const alcedo::EditorHistoryGuardHandle& guard,
                                                const alcedo::commit_hash_t& commit_id,
                                                std::string* error) -> bool {
  return mutation_->MoveHeadToCommit(guard, commit_id, error);
}

auto EditorSessionHistoryPort::DiscardUnmaterializedChanges(
    const alcedo::EditorHistoryGuardHandle& guard, std::string* error) -> bool {
  return mutation_->DiscardUnmaterializedChanges(guard, error);
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

auto EditorSessionHistoryPort::HasUnmaterializedChanges(
    const alcedo::EditorHistoryGuardHandle& guard, std::string* error) -> bool {
  if (!guard.valid) {
    if (error) *error = "Editor history guard is invalid";
    return false;
  }
  return state_->HasUnmaterializedChanges(guard.element_id, error);
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

auto EditorSessionHistoryPort::PasteLiveRootRelativeVersion(
    const alcedo::EditorHistoryGuardHandle& guard,
    const alcedo::AdjustmentTransferPackage& package, std::string version_display_name,
    alcedo::AdjustmentPasteResult* result, std::string* error) -> bool {
  return transfer_->PasteLiveRootRelativeVersion(guard, package, std::move(version_display_name),
                                                 result, error);
}

auto EditorSessionHistoryPort::CancelLivePaste(const alcedo::EditorHistoryGuardHandle& guard,
                                               const alcedo::version_ref_id_t& prior_version_id,
                                               const alcedo::version_ref_id_t& paste_version_id,
                                               std::string* error) -> bool {
  return transfer_->CancelLivePaste(guard, prior_version_id, paste_version_id, error);
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

auto EditorSessionHistoryPort::SyncMaterializedStateAfterCheckpoint(
    const alcedo::EditorHistoryGuardHandle& guard, std::string* error) -> bool {
  return checkpoint_->SyncMaterializedStateAfterCheckpoint(guard, error);
}

}  // namespace alcedo::ui
