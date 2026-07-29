//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "support/editor_save_checkpoint_fixture.hpp"

#include <utility>

namespace alcedo::test {

void EditorSaveCheckpointFixture::SetUp() {
  tasks_            = std::make_shared<FakeEditorTaskPort>();
  journal_          = std::make_shared<FakeEditorJournalPort>();
  checkpoint_store_ = std::make_shared<FakeEditorCheckpointStore>();
  thumbnails_       = std::make_shared<FakeEditorThumbnailPort>();
  history_          = std::make_shared<FakeEditorHistoryPort>();
  coordinator_      = std::make_shared<EditorSaveCheckpointCoordinator>();

  EditorSaveCheckpointService::Dependencies deps;
  deps.journal          = journal_;
  deps.checkpoint_store = checkpoint_store_;
  deps.thumbnails       = thumbnails_;
  deps.tasks            = tasks_;
  deps.save_coordinator = coordinator_;
  service_              = std::make_unique<EditorSaveCheckpointService>(std::move(deps));
}

void EditorSaveCheckpointFixture::TearDown() {
  if (service_) {
    service_->CancelAndWait();
  }
  if (coordinator_) {
    coordinator_->Shutdown();
  }
  service_.reset();
  coordinator_.reset();
  history_.reset();
  thumbnails_.reset();
  checkpoint_store_.reset();
  journal_.reset();
  tasks_.reset();
}

auto EditorSaveCheckpointFixture::MakeCapture() const
    -> std::shared_ptr<const EditorMiniGitSaveCapture> {
  if (fail_capture) {
    return nullptr;
  }
  return MakeOpaqueSaveCapture();
}

auto EditorSaveCheckpointFixture::StartCheckpoint(sl_element_id_t         element_id,
                                                  std::uint64_t           session_generation,
                                                  SaveCheckpointCompletion completion)
    -> CheckpointTicket {
  tasks_->fail_begin                   = fail_task_start;
  journal_->async_commit               = true;
  journal_->fail_commit_start          = fail_journal_commit;
  checkpoint_store_->async_materialize = true;
  // Materialization failure is applied by CompleteDatabaseWrite; start failure is
  // an explicit switch on checkpoint_store_->fail_materialize_start.

  // Acquire the project-owned lock before building the request, matching the
  // production navigation path (lock before capture, hold until terminal).
  auto save_lock = service_->TryAcquireSaveLock(element_id);
  if (!save_lock.owns_lock() && !fail_task_start) {
    // Leave Start to report coordinator contention when the test needs a
    // failure path; still attempt Start so service diagnostics stay consistent.
  }

  SaveCheckpointRequest request;
  request.element_id         = element_id;
  request.image_load_request_id = ImageLoadRequestId{session_generation};
  request.capture            = MakeCapture();
  request.save_lock          = std::move(save_lock);
  return service_->Start(std::move(request), std::move(completion));
}

void EditorSaveCheckpointFixture::CompleteJournalTruncate(bool durable, std::string error) {
  if (fail_journal_commit) {
    durable = false;
    if (error.empty()) {
      error = "journal commit failed";
    }
  }
  journal_->CompleteCommit(durable, std::move(error));
}

void EditorSaveCheckpointFixture::CompleteDatabaseWrite(bool materialized, std::string error) {
  if (fail_materialization) {
    materialized = false;
    if (error.empty()) {
      error = "materialization failed";
    }
  }
  checkpoint_store_->CompleteMaterialization(materialized, std::move(error));
}

void EditorSaveCheckpointFixture::CancelAndWait() {
  if (service_) {
    service_->CancelAndWait();
  }
}

}  // namespace alcedo::test
