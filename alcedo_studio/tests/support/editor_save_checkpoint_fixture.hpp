//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

/// @file editor_save_checkpoint_fixture.hpp
/// @brief Focused fixture for EditorSaveCheckpointService unit tests.
///
/// Provides history capture, journal, checkpoint store, task, thumbnail, and
/// coordinator test doubles plus helpers that drive the async save path without
/// DuckDB, QML, or navigation collaborators.

#include <memory>
#include <string>

#include "app/editor_save_checkpoint_coordinator.hpp"
#include "app/editor_save_checkpoint_service.hpp"
#include "support/editor_session_test_ports.hpp"

namespace alcedo::test {

/// Fixture that owns only save-checkpoint collaborators and failure switches.
class EditorSaveCheckpointFixture {
 public:
  EditorSaveCheckpointFixture()  = default;
  ~EditorSaveCheckpointFixture() = default;

  EditorSaveCheckpointFixture(const EditorSaveCheckpointFixture&)            = delete;
  EditorSaveCheckpointFixture& operator=(const EditorSaveCheckpointFixture&) = delete;

  /// Construct fake ports, a real project-owned coordinator, and the real
  /// EditorSaveCheckpointService.
  ///
  /// Side effects: creates shared_ptr fakes and wires service dependencies.
  void SetUp();

  /// Cancel outstanding callbacks and release owned service state.
  void TearDown();

  /// Start one save checkpoint with the configured capture and completion sink.
  /// Acquires the project-owned SaveCheckpointLock before Start (production path).
  ///
  /// @param element_id         Image element under save.
  /// @param session_generation Session generation stamped on the request.
  /// @param completion         Invoked exactly once on terminal success/failure.
  /// @return Ticket from EditorSaveCheckpointService::Start (invalid on start failure).
  auto StartCheckpoint(sl_element_id_t element_id, std::uint64_t session_generation,
                       SaveCheckpointCompletion completion) -> CheckpointTicket;

  /// Complete the pending DuckDB materialization step for the active async save.
  ///
  /// Preconditions: journal durability already completed (CompleteJournalTruncate).
  void CompleteDatabaseWrite(bool materialized = true, std::string error = {});

  /// Complete the pending journal durability step that enables later truncation.
  ///
  /// @param durable  True when the journal commit is durable and materialize may begin.
  void CompleteJournalTruncate(bool durable = true, std::string error = {});

  /// Stop accepting new callbacks and wait for in-flight save work to drain.
  void CancelAndWait();

  [[nodiscard]] auto service() -> EditorSaveCheckpointService& { return *service_; }
  [[nodiscard]] auto tasks() -> FakeEditorTaskPort& { return *tasks_; }
  [[nodiscard]] auto journal() -> FakeEditorJournalPort& { return *journal_; }
  [[nodiscard]] auto checkpoint_store() -> FakeEditorCheckpointStore& {
    return *checkpoint_store_;
  }
  [[nodiscard]] auto thumbnails() -> FakeEditorThumbnailPort& { return *thumbnails_; }
  [[nodiscard]] auto history() -> FakeEditorHistoryPort& { return *history_; }
  [[nodiscard]] auto coordinator() -> EditorSaveCheckpointCoordinator& { return *coordinator_; }

  /// When true, StartCheckpoint builds a request with a null capture.
  bool fail_capture = false;
  /// When true, task BeginTask returns 0 and Start returns an invalid ticket.
  bool fail_task_start = false;
  /// When true, journal CommitJournalAsync fails to start or Completes non-durable.
  bool fail_journal_commit = false;
  /// When true, materialization reports failure after a durable journal commit.
  bool fail_materialization = false;

 private:
  auto MakeCapture() const -> std::shared_ptr<const EditorMiniGitSaveCapture>;

  std::shared_ptr<FakeEditorTaskPort>                tasks_;
  std::shared_ptr<FakeEditorJournalPort>             journal_;
  std::shared_ptr<FakeEditorCheckpointStore>         checkpoint_store_;
  std::shared_ptr<FakeEditorThumbnailPort>           thumbnails_;
  std::shared_ptr<FakeEditorHistoryPort>             history_;
  std::shared_ptr<EditorSaveCheckpointCoordinator>   coordinator_;
  std::unique_ptr<EditorSaveCheckpointService>       service_;
};

}  // namespace alcedo::test
