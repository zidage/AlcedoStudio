//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/editor_session_history_port.hpp"

#include <functional>
#include <optional>

#include "app/editor_session_types.hpp"
#include "app/pipeline_service.hpp"
#include "edit/history/commit_graph.hpp"
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
  std::scoped_lock lock(mutex_);
  EditorHistoryState::Services s;
  s.mini_git_journal_path = std::move(services.mini_git_journal_path);
  state_->SetServices(std::move(s));
}

void EditorSessionHistoryPort::SetPipelinePort(
    std::shared_ptr<EditorSessionPipelinePort> pipeline_port) {
  std::scoped_lock lock(mutex_);
  state_->SetPipelinePort(std::move(pipeline_port));
}

auto EditorSessionHistoryPort::Acquire(sl_element_id_t element_id, std::string* error)
    -> alcedo::EditorHistoryGuardHandle {
  std::scoped_lock lock(mutex_);
  auto journal_path = state_->JournalPathResolver();
  if (journal_path) {
    std::string prepare_error;
    if (!state_->AcquireWorkingState(element_id, &prepare_error)) {
      if (error)
        *error = prepare_error.empty() ? "Editor Mini-Git history initialization failed"
                                       : std::move(prepare_error);
      return {};
    }
    state_->PublishWorkingSnapshots(element_id);
  }
  return {element_id, true};
}

auto EditorSessionHistoryPort::PublishAfterWrite(const alcedo::EditorHistoryGuardHandle& guard,
                                                 bool result) -> bool {
  state_->PublishWorkingSnapshots(guard.element_id);
  return result;
}

void EditorSessionHistoryPort::Release(const alcedo::EditorHistoryGuardHandle& guard) {
  std::scoped_lock lock(mutex_);
  if (!guard.valid) return;
  state_->ReleaseState(guard.element_id);
}

auto EditorSessionHistoryPort::CaptureAdjustmentBeforePreview(
    const alcedo::EditorHistoryGuardHandle& guard, const alcedo::EditorAdjustmentPatch& patch,
    std::string* error) -> bool {
  std::scoped_lock lock(mutex_);
  return PublishAfterWrite(guard, mutation_->CaptureAdjustmentBeforePreview(guard, patch, error));
}

auto EditorSessionHistoryPort::RestoreUnsettledPreview(
    const alcedo::EditorHistoryGuardHandle& guard, bool* live_changed, std::string* error)
    -> bool {
  std::scoped_lock lock(mutex_);
  return PublishAfterWrite(guard, mutation_->RestoreUnsettledPreview(guard, live_changed, error));
}

auto EditorSessionHistoryPort::CommitAdjustment(const alcedo::EditorHistoryGuardHandle& guard,
                                                const alcedo::EditorAdjustmentPatch& patch,
                                                std::string* error) -> bool {
  std::scoped_lock lock(mutex_);
  return PublishAfterWrite(guard, mutation_->CommitAdjustment(guard, patch, error));
}

auto EditorSessionHistoryPort::CommitPipelineEditBatch(const alcedo::EditorHistoryGuardHandle& guard,
                                                       alcedo::PipelineEditBatch batch,
                                                       std::string* error) -> bool {
  std::scoped_lock lock(mutex_);
  return PublishAfterWrite(guard,
                           mutation_->CommitPipelineEditBatch(guard, std::move(batch), error));
}

auto EditorSessionHistoryPort::EditNodeGraph(const alcedo::EditorHistoryGuardHandle& guard,
                                             alcedo::NodeGraphTopologyChange change,
                                             std::string* error) -> bool {
  std::scoped_lock lock(mutex_);
  return PublishAfterWrite(guard, mutation_->EditNodeGraph(guard, std::move(change), error));
}

auto EditorSessionHistoryPort::RenameColorGrade(const alcedo::EditorHistoryGuardHandle& guard,
                                                const alcedo::NodeId& node_id,
                                                std::string display_name, std::string* error)
    -> bool {
  std::scoped_lock lock(mutex_);
  return PublishAfterWrite(
      guard, mutation_->RenameColorGrade(guard, node_id, std::move(display_name), error));
}

auto EditorSessionHistoryPort::SetColorGradeDeletionProtected(
    const alcedo::EditorHistoryGuardHandle& guard, const alcedo::NodeId& node_id,
    bool deletion_protected, std::string* error, bool* changed) -> bool {
  std::scoped_lock lock(mutex_);
  return PublishAfterWrite(guard, mutation_->SetColorGradeDeletionProtected(
                                      guard, node_id, deletion_protected, error, changed));
}

auto EditorSessionHistoryPort::InsertColorGradeAtTop(const alcedo::EditorHistoryGuardHandle& guard,
                                                     const alcedo::NodeId&                   new_id,
                                                     const alcedo::NodeId& expected_predecessor_id,
                                                     std::string*          error) -> bool {
  std::scoped_lock lock(mutex_);
  return PublishAfterWrite(
      guard, mutation_->InsertColorGradeAtTop(guard, new_id, expected_predecessor_id, error));
}

auto EditorSessionHistoryPort::RemoveColorGradeAndBridge(
    const alcedo::EditorHistoryGuardHandle& guard, const alcedo::NodeId& node_id,
    std::string* error) -> bool {
  std::scoped_lock lock(mutex_);
  return PublishAfterWrite(guard, mutation_->RemoveColorGradeAndBridge(guard, node_id, error));
}

auto EditorSessionHistoryPort::SetColorGradeEnabled(const alcedo::EditorHistoryGuardHandle& guard,
                                                    const alcedo::NodeId& node_id, bool enabled,
                                                    std::string* error) -> bool {
  std::scoped_lock lock(mutex_);
  return PublishAfterWrite(guard, mutation_->SetColorGradeEnabled(guard, node_id, enabled, error));
}

auto EditorSessionHistoryPort::SetColorGradeMix(const alcedo::EditorHistoryGuardHandle& guard,
                                                const alcedo::NodeId& node_id, float mix,
                                                std::string* error) -> bool {
  std::scoped_lock lock(mutex_);
  return PublishAfterWrite(guard, mutation_->SetColorGradeMix(guard, node_id, mix, error));
}

auto EditorSessionHistoryPort::AddMask(const alcedo::EditorHistoryGuardHandle& guard,
                                       const alcedo::NodeId& node_id, alcedo::MaskModel mask,
                                       std::uint32_t display_index, std::string* error) -> bool {
  std::scoped_lock lock(mutex_);
  return PublishAfterWrite(
      guard, mutation_->AddMask(guard, node_id, std::move(mask), display_index, error));
}

auto EditorSessionHistoryPort::RemoveMask(const alcedo::EditorHistoryGuardHandle& guard,
                                          const alcedo::NodeId& node_id,
                                          const alcedo::MaskId& mask_id, std::string* error)
    -> bool {
  std::scoped_lock lock(mutex_);
  return PublishAfterWrite(guard, mutation_->RemoveMask(guard, node_id, mask_id, error));
}

auto EditorSessionHistoryPort::ReplaceMaskSource(const alcedo::EditorHistoryGuardHandle& guard,
                                                 const alcedo::NodeId& node_id,
                                                 const alcedo::MaskId& mask_id,
                                                 nlohmann::json after_source, std::string* error)
    -> bool {
  std::scoped_lock lock(mutex_);
  return PublishAfterWrite(
      guard, mutation_->ReplaceMaskSource(guard, node_id, mask_id, std::move(after_source), error));
}

auto EditorSessionHistoryPort::SetMaskField(const alcedo::EditorHistoryGuardHandle& guard,
                                            const alcedo::NodeId& node_id,
                                            const alcedo::MaskId& mask_id, std::string field_key,
                                            nlohmann::json after_value, std::string* error) -> bool {
  std::scoped_lock lock(mutex_);
  return PublishAfterWrite(guard,
                           mutation_->SetMaskField(guard, node_id, mask_id, std::move(field_key),
                                                   std::move(after_value), error));
}

auto EditorSessionHistoryPort::Undo(const alcedo::EditorHistoryGuardHandle& guard,
                                    std::string* error) -> bool {
  std::scoped_lock lock(mutex_);
  return PublishAfterWrite(guard, mutation_->Undo(guard, error));
}

auto EditorSessionHistoryPort::Redo(const alcedo::EditorHistoryGuardHandle& guard,
                                    std::string* error) -> bool {
  std::scoped_lock lock(mutex_);
  return PublishAfterWrite(guard, mutation_->Redo(guard, error));
}

auto EditorSessionHistoryPort::LastPublishedRenderReason() const
    -> std::optional<alcedo::EditorRenderReason> {
  std::scoped_lock lock(mutex_);
  return state_->LastPublishedRenderReason();
}

auto EditorSessionHistoryPort::MoveHeadToCommit(const alcedo::EditorHistoryGuardHandle& guard,
                                                const alcedo::commit_hash_t& commit_id,
                                                std::string* error) -> bool {
  std::scoped_lock lock(mutex_);
  return PublishAfterWrite(guard, mutation_->MoveHeadToCommit(guard, commit_id, error));
}

auto EditorSessionHistoryPort::DiscardUnmaterializedChanges(
    const alcedo::EditorHistoryGuardHandle& guard, std::string* error) -> bool {
  std::scoped_lock lock(mutex_);
  return PublishAfterWrite(guard, mutation_->DiscardUnmaterializedChanges(guard, error));
}

auto EditorSessionHistoryPort::CheckoutVersion(const alcedo::EditorHistoryGuardHandle& guard,
                                               const alcedo::Hash128& version_id,
                                               std::string* error) -> bool {
  std::scoped_lock lock(mutex_);
  return PublishAfterWrite(guard, mutation_->CheckoutVersion(guard, version_id, error));
}

auto EditorSessionHistoryPort::ReadActiveVersionId(
    const alcedo::EditorHistoryGuardHandle& guard, alcedo::version_ref_id_t* version_id,
    std::string* error) -> bool {
  std::scoped_lock lock(mutex_);
  return projection_->ReadActiveVersionId(guard, version_id, error);
}

auto EditorSessionHistoryPort::ReadHistorySnapshot(const alcedo::EditorHistoryGuardHandle& guard,
                                                   alcedo::EditorHistorySnapshot* snapshot,
                                                   std::string* error) -> bool {
  std::scoped_lock lock(mutex_);
  return projection_->ReadHistorySnapshot(guard, snapshot, error);
}

auto EditorSessionHistoryPort::SnapshotHistorySource(
    const alcedo::EditorHistoryGuardHandle&          guard,
    std::shared_ptr<const alcedo::CommitGraph>*      graph,
    std::shared_ptr<const alcedo::PipelineDocument>* root_document, std::string* error) -> bool {
  if (graph == nullptr || root_document == nullptr) {
    if (error) *error = "History source snapshot requires output storage";
    return false;
  }
  std::scoped_lock lock(mutex_);
  auto             state = state_->EnsureWorkingState(guard.element_id, error);
  if (!state) return false;
  *graph = std::make_shared<const alcedo::CommitGraph>(*state->graph);
  // The root is immutable; the caller shares it rather than copying it.
  *root_document =
      std::shared_ptr<const alcedo::PipelineDocument>(state->root, &state->root->document);
  return true;
}

auto EditorSessionHistoryPort::HasUnmaterializedChanges(
    const alcedo::EditorHistoryGuardHandle& guard, std::string* error) -> bool {
  std::scoped_lock lock(mutex_);
  if (!guard.valid) {
    if (error) *error = "Editor history guard is invalid";
    return false;
  }
  return state_->HasUnmaterializedChanges(guard.element_id, error);
}

auto EditorSessionHistoryPort::CreateRootVersionAndCheckout(
    const alcedo::EditorHistoryGuardHandle& guard, std::string display_name,
    alcedo::version_ref_id_t* version_id, std::string* error) -> bool {
  std::scoped_lock lock(mutex_);
  return PublishAfterWrite(guard, version_refs_->CreateRootVersionAndCheckout(
                                      guard, std::move(display_name), version_id, error));
}

auto EditorSessionHistoryPort::BranchFromCommitAndCheckout(
    const alcedo::EditorHistoryGuardHandle& guard, const alcedo::commit_hash_t& commit_id,
    std::string display_name, alcedo::version_ref_id_t* version_id, std::string* error) -> bool {
  std::scoped_lock lock(mutex_);
  return PublishAfterWrite(
      guard, version_refs_->BranchFromCommitAndCheckout(guard, commit_id, std::move(display_name),
                                                        version_id, error));
}

auto EditorSessionHistoryPort::RenameVersion(const alcedo::EditorHistoryGuardHandle& guard,
                                             const alcedo::Hash128& version_id,
                                             std::string display_name, std::string* error)
    -> bool {
  std::scoped_lock lock(mutex_);
  return version_refs_->RenameVersion(guard, version_id, std::move(display_name), error);
}

auto EditorSessionHistoryPort::RemoveVersion(const alcedo::EditorHistoryGuardHandle& guard,
                                             const alcedo::Hash128& version_id,
                                             std::string* error) -> bool {
  std::scoped_lock lock(mutex_);
  return PublishAfterWrite(guard, version_refs_->RemoveVersion(guard, version_id, error));
}

auto EditorSessionHistoryPort::PasteLiveRootRelativeVersion(
    const alcedo::EditorHistoryGuardHandle& guard,
    const alcedo::AdjustmentTransferPackage& package, std::string version_display_name,
    alcedo::AdjustmentPasteResult* result, std::string* error) -> bool {
  std::scoped_lock lock(mutex_);
  return PublishAfterWrite(guard,
                           transfer_->PasteLiveRootRelativeVersion(
                               guard, package, std::move(version_display_name), result, error));
}

auto EditorSessionHistoryPort::ReadPanelProjection(const alcedo::EditorHistoryGuardHandle& guard,
                                                   alcedo::EditorPanelProjection* projection,
                                                   std::string* error) -> bool {
  std::scoped_lock lock(mutex_);
  return projection_->ReadPanelProjection(guard, projection, error);
}

auto EditorSessionHistoryPort::SetPanelProjectionNode(const alcedo::EditorHistoryGuardHandle& guard,
                                                      const alcedo::NodeId& node_id,
                                                      std::uint64_t session_generation,
                                                      std::string* error) -> bool {
  std::scoped_lock lock(mutex_);
  return mutation_->SetPanelProjectionNode(guard, node_id, session_generation, error);
}

auto EditorSessionHistoryPort::WithWorkingDocument(
    const alcedo::EditorHistoryGuardHandle&           guard,
    const alcedo::IEditorHistoryPort::MaskDocumentOp& op, std::string* error) -> bool {
  std::scoped_lock lock(mutex_);
  return PublishAfterWrite(guard, mutation_->WithWorkingDocument(guard, op, error));
}

auto EditorSessionHistoryPort::CaptureSaveCheckpoint(const alcedo::EditorHistoryGuardHandle& guard,
                                                     std::string* error)
    -> std::shared_ptr<const alcedo::EditorMiniGitSaveCapture> {
  std::scoped_lock lock(mutex_);
  return checkpoint_->CaptureSaveCheckpoint(guard, error);
}

auto EditorSessionHistoryPort::DiscardMaterializedJournalThrough(
    const alcedo::EditorHistoryGuardHandle& guard, std::uint64_t last_sequence,
    std::string* error) -> bool {
  std::scoped_lock lock(mutex_);
  return checkpoint_->DiscardMaterializedJournalThrough(guard, last_sequence, error);
}

auto EditorSessionHistoryPort::SyncMaterializedStateAfterCheckpoint(
    const alcedo::EditorHistoryGuardHandle& guard, std::string* error) -> bool {
  std::scoped_lock lock(mutex_);
  return checkpoint_->SyncMaterializedStateAfterCheckpoint(guard, error);
}

}  // namespace alcedo::ui
