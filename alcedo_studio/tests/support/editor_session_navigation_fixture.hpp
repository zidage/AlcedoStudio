//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

/// @file editor_session_navigation_fixture.hpp
/// @brief Focused fixture for EditorSessionNavigationController A-to-B save order.
///
/// Constructs a real navigation controller with lifecycle and save-checkpoint
/// collaborators. Records ordered events:
///   checkpoint_a → commit → truncate → thumbnail → release_a → acquire_b
/// Does not own DuckDB projects, QML engines, or public render infrastructure.

#include <memory>
#include <string>
#include <vector>

#include "app/editor_save_checkpoint_coordinator.hpp"
#include "app/editor_save_checkpoint_service.hpp"
#include "app/editor_session_command_queue.hpp"
#include "app/editor_session_edit_controller.hpp"
#include "app/editor_session_lifecycle.hpp"
#include "app/editor_session_navigation_controller.hpp"
#include "app/editor_session_render_controller.hpp"
#include "support/editor_session_test_ports.hpp"

namespace alcedo::test {

/// Navigation fixture with fixed image identities A and B.
class EditorSessionNavigationFixture {
 public:
  static constexpr sl_element_id_t kElementA                                       = 1;
  static constexpr image_id_t      kImageA                                         = 2;
  static constexpr sl_element_id_t kElementB                                       = 3;
  static constexpr image_id_t      kImageB                                         = 4;

  EditorSessionNavigationFixture()                                                 = default;
  ~EditorSessionNavigationFixture()                                                = default;

  EditorSessionNavigationFixture(const EditorSessionNavigationFixture&)            = delete;
  EditorSessionNavigationFixture& operator=(const EditorSessionNavigationFixture&) = delete;

  /// Build lifecycle, save service, navigation controller, and private stubs.
  void                            SetUp();

  /// Cancel save work and destroy owned collaborators.
  void                            TearDown();

  /// Open image A into Interactive through the lifecycle public API.
  void                            OpenA();

  /// Request a switch from A to B. Starts a save checkpoint when A is open.
  auto                            RequestSwitchToB() -> NavigationOutcome;

  /// Request Version checkout on the open image. Starts a save checkpoint first.
  auto RequestCheckoutVersion(const version_ref_id_t& version_id) -> NavigationOutcome;

  /// Complete the in-flight save successfully (journal durable + materialize).
  void CompleteCheckpoint();

  /// Complete the in-flight save as a materialization failure.
  void FailCheckpoint(std::string error = "A materialization failed");

  /// Run all save completions posted to the command executor. The fixture
  /// serializes save completion through the same manual executor the session
  /// queue uses, so a posted completion only reduces state when this is called.
  void Drain();

  [[nodiscard]] auto events() const -> const std::vector<std::string>& { return events_; }
  [[nodiscard]] auto nav() -> EditorSessionNavigationController& { return *nav_; }
  [[nodiscard]] auto lifecycle() -> EditorSessionLifecycle& { return *lifecycle_; }
  [[nodiscard]] auto render() -> EditorSessionRenderController& { return *render_; }
  [[nodiscard]] auto save_service() -> EditorSaveCheckpointService& { return *save_service_; }
  [[nodiscard]] auto pipeline() -> FakeEditorPipelinePort& { return pipeline_->inner; }
  [[nodiscard]] auto history() -> FakeEditorHistoryPort& { return history_->inner; }
  [[nodiscard]] auto journal() -> FakeEditorJournalPort& { return journal_->inner; }
  [[nodiscard]] auto checkpoint_store() -> FakeEditorCheckpointStore& {
    return checkpoint_store_->inner;
  }
  [[nodiscard]] auto thumbnails() -> FakeEditorThumbnailPort& { return thumbnails_->inner; }
  [[nodiscard]] auto tasks() -> FakeEditorTaskPort& { return *tasks_; }

 private:
  void                       RecordEvent(std::string name);

  /// Pipeline port that records release_a / acquire_b into the event vector.
  class TrackingPipelinePort final : public IEditorPipelinePort {
   public:
    explicit TrackingPipelinePort(EditorSessionNavigationFixture* owner) : owner_(owner) {}
    auto Acquire(sl_element_id_t element_id, std::string* error)
        -> EditorPipelineGuardHandle override;
    void                   Release(const EditorPipelineGuardHandle& guard) override;

    FakeEditorPipelinePort inner;

   private:
    EditorSessionNavigationFixture* owner_ = nullptr;
  };

  /// History port that records checkpoint_a when capturing A's save prefix.
  class TrackingHistoryPort final : public IEditorHistoryPort {
   public:
    explicit TrackingHistoryPort(EditorSessionNavigationFixture* owner) : owner_(owner) {}
    auto Acquire(sl_element_id_t element_id, std::string* error)
        -> EditorHistoryGuardHandle override;
    void Release(const EditorHistoryGuardHandle& guard) override;
    auto CaptureAdjustmentBeforePreview(const EditorHistoryGuardHandle& guard,
                                        const EditorAdjustmentPatch& patch, std::string* error)
        -> bool override;
    auto CommitAdjustment(const EditorHistoryGuardHandle& guard, const EditorAdjustmentPatch& patch,
                          std::string* error) -> bool override;
    auto Undo(const EditorHistoryGuardHandle& guard, std::string* error) -> bool override;
    auto Redo(const EditorHistoryGuardHandle& guard, std::string* error) -> bool override;
    auto ReadAdjustmentSnapshot(const EditorHistoryGuardHandle& guard,
                                EditorRenderAdjustmentSnapshot* snapshot, std::string* error)
        -> bool override;
    auto CaptureSaveCheckpoint(const EditorHistoryGuardHandle& guard, std::string* error)
        -> std::shared_ptr<const EditorMiniGitSaveCapture> override;
    auto CheckoutVersion(const EditorHistoryGuardHandle& guard, const Hash128& version_id,
                         std::string* error) -> bool override;
    auto CreateRootVersionAndCheckout(const EditorHistoryGuardHandle& guard,
                                      std::string display_name, version_ref_id_t* version_id,
                                      std::string* error) -> bool override;
    auto BranchFromCommitAndCheckout(const EditorHistoryGuardHandle& guard,
                                     const commit_hash_t& commit_id, std::string display_name,
                                     version_ref_id_t* version_id, std::string* error)
        -> bool override;

    FakeEditorHistoryPort inner;

   private:
    EditorSessionNavigationFixture* owner_ = nullptr;
  };

  /// Journal port that records "commit" when CommitJournalAsync is invoked.
  class TrackingJournalPort final : public IEditorJournalPort {
   public:
    explicit TrackingJournalPort(EditorSessionNavigationFixture* owner) : owner_(owner) {}
    auto FinalizeEdit(sl_element_id_t element_id, std::uint64_t session_generation,
                      std::string* error) -> bool override;
    auto CommitJournalAsync(sl_element_id_t element_id, std::uint64_t session_generation,
                            EditorJournalCommitCallback callback) -> bool override;
    auto DiscardUnflushed(sl_element_id_t element_id, std::string* error) -> bool override;

    FakeEditorJournalPort inner;

   private:
    EditorSessionNavigationFixture* owner_ = nullptr;
  };

  /// Checkpoint store that records "truncate" when MaterializeAsync is invoked
  /// (production truncate runs after DuckDB commit inside the materializer).
  class TrackingCheckpointStore final : public IEditorCheckpointStore {
   public:
    explicit TrackingCheckpointStore(EditorSessionNavigationFixture* owner) : owner_(owner) {}
    auto MaterializeAsync(std::shared_ptr<const EditorMiniGitSaveCapture> capture,
                          EditorMaterializeCallback callback) -> bool override;
    auto Materialize(std::shared_ptr<const EditorMiniGitSaveCapture> capture, std::string* error)
        -> EditorMaterializeOutcome override;
    auto RecoverAndMaterialize(sl_element_id_t element_id, std::uint64_t session_generation,
                               std::string* error) -> EditorMaterializeOutcome override;

    FakeEditorCheckpointStore inner;

   private:
    EditorSessionNavigationFixture* owner_ = nullptr;
  };

  /// Thumbnail port that records "thumbnail" when A is refreshed after save.
  class TrackingThumbnailPort final : public IEditorThumbnailPort {
   public:
    explicit TrackingThumbnailPort(EditorSessionNavigationFixture* owner) : owner_(owner) {}
    void                    RefreshAfterMaterialization(sl_element_id_t element_id) override;

    FakeEditorThumbnailPort inner;

   private:
    EditorSessionNavigationFixture* owner_ = nullptr;
  };

  std::shared_ptr<TrackingPipelinePort>               pipeline_;
  std::shared_ptr<TrackingHistoryPort>                history_;
  std::shared_ptr<FakeEditorTaskPort>                 tasks_;
  std::shared_ptr<TrackingJournalPort>                journal_;
  std::shared_ptr<TrackingCheckpointStore>            checkpoint_store_;
  std::shared_ptr<TrackingThumbnailPort>              thumbnails_;
  std::shared_ptr<FakeEditorRenderSubmitPort>         render_submit_;
  std::shared_ptr<EditorSaveCheckpointCoordinator>    save_coordinator_;
  std::shared_ptr<EditorSessionManualCommandExecutor> command_executor_;
  std::unique_ptr<EditorSessionLifecycle>             lifecycle_;
  std::unique_ptr<EditorSaveCheckpointService>        save_service_;
  std::unique_ptr<EditorSessionRenderController>      render_;
  std::unique_ptr<EditorSessionEditController>        edit_;
  std::unique_ptr<EditorSessionNavigationController>  nav_;
  std::vector<std::string>                            events_;
};

}  // namespace alcedo::test
