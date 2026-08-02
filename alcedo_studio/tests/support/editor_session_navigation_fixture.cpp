//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "support/editor_session_navigation_fixture.hpp"

#include <stdexcept>
#include <utility>

namespace alcedo::test {

void EditorSessionNavigationFixture::SetUp() {
  events_.clear();
  pipeline_         = std::make_shared<TrackingPipelinePort>(this);
  history_          = std::make_shared<TrackingHistoryPort>(this);
  tasks_            = std::make_shared<FakeEditorTaskPort>();
  journal_          = std::make_shared<TrackingJournalPort>(this);
  checkpoint_store_ = std::make_shared<TrackingCheckpointStore>(this);
  thumbnails_       = std::make_shared<TrackingThumbnailPort>(this);
  render_submit_    = std::make_shared<FakeEditorRenderSubmitPort>();

  EditorSessionLifecycle::Dependencies life_deps;
  life_deps.pipeline = pipeline_;
  life_deps.history  = history_;
  lifecycle_         = std::make_unique<EditorSessionLifecycle>(std::move(life_deps));

  save_coordinator_  = std::make_shared<EditorSaveCheckpointCoordinator>();
  command_executor_  = std::make_shared<EditorSessionManualCommandExecutor>();
  EditorSaveCheckpointService::Dependencies save_deps;
  save_deps.journal          = journal_;
  save_deps.checkpoint_store = checkpoint_store_;
  save_deps.thumbnails       = thumbnails_;
  save_deps.tasks            = tasks_;
  save_deps.command_executor = command_executor_;
  save_deps.save_coordinator = save_coordinator_;
  save_service_              = std::make_unique<EditorSaveCheckpointService>(std::move(save_deps));

  // Render/edit exist only to satisfy the navigation constructor. They are not
  // part of this fixture's public surface and do not open GPU or DuckDB paths.
  EditorSessionRenderController::Dependencies render_deps{render_submit_,
                                                          [](const EditorRenderEvent&) {}};
  render_ = std::make_unique<EditorSessionRenderController>(std::move(render_deps));
  render_->SetPresentationSinkId(1);
  render_->SetPresentationSize(1920, 1080);

  EditorSessionEditController::Dependencies edit_deps{history_, journal_};
  edit_ = std::make_unique<EditorSessionEditController>(std::move(edit_deps));

  nav_  = std::make_unique<EditorSessionNavigationController>(
      *lifecycle_, *save_service_, *render_, journal_.get(), checkpoint_store_.get(),
      history_.get());
}

void EditorSessionNavigationFixture::TearDown() {
  if (save_service_) {
    save_service_->CancelAndWait();
  }
  if (save_coordinator_) {
    save_coordinator_->Shutdown();
  }
  nav_.reset();
  edit_.reset();
  render_.reset();
  save_service_.reset();
  lifecycle_.reset();
  save_coordinator_.reset();
  command_executor_.reset();
  render_submit_.reset();
  thumbnails_.reset();
  checkpoint_store_.reset();
  journal_.reset();
  tasks_.reset();
  history_.reset();
  pipeline_.reset();
  events_.clear();
}

void EditorSessionNavigationFixture::RecordEvent(std::string name) {
  events_.push_back(std::move(name));
}

void EditorSessionNavigationFixture::OpenA() {
  std::string error;
  if (!lifecycle_->BeginAcquire(kElementA, kImageA, false, nullptr, &error)) {
    throw std::runtime_error("OpenA BeginAcquire failed: " + error);
  }
  if (!lifecycle_->AcquireGuards(&error)) {
    throw std::runtime_error("OpenA AcquireGuards failed: " + error);
  }
  lifecycle_->MarkImageReady();
  lifecycle_->MarkFirstFrameReady();
}

auto EditorSessionNavigationFixture::RequestSwitchToB() -> NavigationOutcome {
  journal_->inner.async_commit               = true;
  checkpoint_store_->inner.async_materialize = true;
  return nav_->RequestOpenOrSwitch(kElementB, kImageB, true);
}

auto EditorSessionNavigationFixture::RequestCheckoutVersion(const version_ref_id_t& version_id)
    -> NavigationOutcome {
  journal_->inner.async_commit               = true;
  checkpoint_store_->inner.async_materialize = true;
  return nav_->RequestCheckoutVersion(version_id);
}

void EditorSessionNavigationFixture::CompleteCheckpoint() {
  journal_->inner.CompleteCommit(true);
  checkpoint_store_->inner.CompleteMaterialization(true);
  Drain();
}

void EditorSessionNavigationFixture::FailCheckpoint(std::string error) {
  journal_->inner.CompleteCommit(true);
  checkpoint_store_->inner.CompleteMaterialization(false, std::move(error));
  Drain();
}

void EditorSessionNavigationFixture::Drain() {
  if (command_executor_) {
    command_executor_->DrainAll();
  }
}

auto EditorSessionNavigationFixture::TrackingPipelinePort::Acquire(sl_element_id_t element_id,
                                                                   std::string*    error)
    -> EditorPipelineGuardHandle {
  auto handle = inner.Acquire(element_id, error);
  if (handle.valid && element_id == kElementB && owner_ != nullptr) {
    owner_->RecordEvent("acquire_b");
  }
  return handle;
}

void EditorSessionNavigationFixture::TrackingPipelinePort::Release(
    const EditorPipelineGuardHandle& guard) {
  if (guard.valid && guard.element_id == kElementA && owner_ != nullptr) {
    owner_->RecordEvent("release_a");
  }
  inner.Release(guard);
}

auto EditorSessionNavigationFixture::TrackingHistoryPort::Acquire(sl_element_id_t element_id,
                                                                  std::string*    error)
    -> EditorHistoryGuardHandle {
  return inner.Acquire(element_id, error);
}

void EditorSessionNavigationFixture::TrackingHistoryPort::Release(
    const EditorHistoryGuardHandle& guard) {
  inner.Release(guard);
}

auto EditorSessionNavigationFixture::TrackingHistoryPort::CaptureAdjustmentBeforePreview(
    const EditorHistoryGuardHandle& guard, const EditorAdjustmentPatch& patch, std::string* error)
    -> bool {
  return inner.CaptureAdjustmentBeforePreview(guard, patch, error);
}

auto EditorSessionNavigationFixture::TrackingHistoryPort::CommitAdjustment(
    const EditorHistoryGuardHandle& guard, const EditorAdjustmentPatch& patch, std::string* error)
    -> bool {
  return inner.CommitAdjustment(guard, patch, error);
}

auto EditorSessionNavigationFixture::TrackingHistoryPort::Undo(
    const EditorHistoryGuardHandle& guard, std::string* error) -> bool {
  return inner.Undo(guard, error);
}

auto EditorSessionNavigationFixture::TrackingHistoryPort::Redo(
    const EditorHistoryGuardHandle& guard, std::string* error) -> bool {
  return inner.Redo(guard, error);
}

auto EditorSessionNavigationFixture::TrackingHistoryPort::ReadAdjustmentSnapshot(
    const EditorHistoryGuardHandle& guard, EditorRenderAdjustmentSnapshot* snapshot,
    std::string* error) -> bool {
  return inner.ReadAdjustmentSnapshot(guard, snapshot, error);
}

auto EditorSessionNavigationFixture::TrackingHistoryPort::CaptureSaveCheckpoint(
    const EditorHistoryGuardHandle& guard, std::string* error)
    -> std::shared_ptr<const EditorMiniGitSaveCapture> {
  if (guard.valid && guard.element_id == kElementA && owner_ != nullptr) {
    owner_->RecordEvent("checkpoint_a");
  }
  return inner.CaptureSaveCheckpoint(guard, error);
}

auto EditorSessionNavigationFixture::TrackingHistoryPort::CheckoutVersion(
    const EditorHistoryGuardHandle& guard, const Hash128& version_id, std::string* error) -> bool {
  if (owner_ != nullptr) {
    owner_->RecordEvent("checkout_version");
  }
  return inner.CheckoutVersion(guard, version_id, error);
}

auto EditorSessionNavigationFixture::TrackingHistoryPort::CreateRootVersionAndCheckout(
    const EditorHistoryGuardHandle& guard, std::string display_name, version_ref_id_t* version_id,
    std::string* error) -> bool {
  if (owner_ != nullptr) {
    owner_->RecordEvent("create_root_version");
  }
  return inner.CreateRootVersionAndCheckout(guard, std::move(display_name), version_id, error);
}

auto EditorSessionNavigationFixture::TrackingHistoryPort::BranchFromCommitAndCheckout(
    const EditorHistoryGuardHandle& guard, const commit_hash_t& commit_id, std::string display_name,
    version_ref_id_t* version_id, std::string* error) -> bool {
  if (owner_ != nullptr) {
    owner_->RecordEvent("branch_from_commit");
  }
  return inner.BranchFromCommitAndCheckout(guard, commit_id, std::move(display_name), version_id,
                                           error);
}

auto EditorSessionNavigationFixture::TrackingJournalPort::FinalizeEdit(
    sl_element_id_t element_id, std::uint64_t session_generation, std::string* error) -> bool {
  return inner.FinalizeEdit(element_id, session_generation, error);
}

auto EditorSessionNavigationFixture::TrackingJournalPort::CommitJournalAsync(
    sl_element_id_t element_id, std::uint64_t session_generation,
    EditorJournalCommitCallback callback) -> bool {
  if (owner_ != nullptr) {
    owner_->RecordEvent("commit");
  }
  return inner.CommitJournalAsync(element_id, session_generation, std::move(callback));
}

auto EditorSessionNavigationFixture::TrackingJournalPort::DiscardUnflushed(
    sl_element_id_t element_id, std::string* error) -> bool {
  return inner.DiscardUnflushed(element_id, error);
}

auto EditorSessionNavigationFixture::TrackingCheckpointStore::MaterializeAsync(
    std::shared_ptr<const EditorMiniGitSaveCapture> capture, EditorMaterializeCallback callback)
    -> bool {
  if (owner_ != nullptr) {
    owner_->RecordEvent("truncate");
  }
  return inner.MaterializeAsync(std::move(capture), std::move(callback));
}

auto EditorSessionNavigationFixture::TrackingCheckpointStore::Materialize(
    std::shared_ptr<const EditorMiniGitSaveCapture> capture, std::string* error)
    -> EditorMaterializeOutcome {
  if (owner_ != nullptr) {
    owner_->RecordEvent("truncate");
  }
  return inner.Materialize(std::move(capture), error);
}

auto EditorSessionNavigationFixture::TrackingCheckpointStore::RecoverAndMaterialize(
    sl_element_id_t element_id, std::uint64_t session_generation, std::string* error)
    -> EditorMaterializeOutcome {
  return inner.RecoverAndMaterialize(element_id, session_generation, error);
}

void EditorSessionNavigationFixture::TrackingThumbnailPort::RefreshAfterMaterialization(
    sl_element_id_t element_id) {
  if (element_id == kElementA && owner_ != nullptr) {
    owner_->RecordEvent("thumbnail");
  }
  inner.RefreshAfterMaterialization(element_id);
}

}  // namespace alcedo::test
