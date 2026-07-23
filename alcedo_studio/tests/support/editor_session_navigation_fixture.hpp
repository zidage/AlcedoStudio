//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

/// @file editor_session_navigation_fixture.hpp
/// @brief Focused fixture for EditorSessionNavigationController A-to-B save order.
///
/// Constructs a real navigation controller with lifecycle and save-checkpoint
/// collaborators. Records ordered events checkpoint_a / release_a / acquire_b.
/// Does not own DuckDB projects, QML engines, or public render infrastructure.

#include <memory>
#include <string>
#include <vector>

#include "app/editor_save_checkpoint_coordinator.hpp"
#include "app/editor_save_checkpoint_service.hpp"
#include "app/editor_session_edit_controller.hpp"
#include "app/editor_session_lifecycle.hpp"
#include "app/editor_session_navigation_controller.hpp"
#include "app/editor_session_render_controller.hpp"
#include "support/editor_session_test_ports.hpp"

namespace alcedo::test {

/// Navigation fixture with fixed image identities A and B.
class EditorSessionNavigationFixture {
 public:
  static constexpr sl_element_id_t kElementA = 1;
  static constexpr image_id_t      kImageA   = 2;
  static constexpr sl_element_id_t kElementB = 3;
  static constexpr image_id_t      kImageB   = 4;

  EditorSessionNavigationFixture()  = default;
  ~EditorSessionNavigationFixture() = default;

  EditorSessionNavigationFixture(const EditorSessionNavigationFixture&)            = delete;
  EditorSessionNavigationFixture& operator=(const EditorSessionNavigationFixture&) = delete;

  /// Build lifecycle, save service, navigation controller, and private stubs.
  void SetUp();

  /// Cancel save work and destroy owned collaborators.
  void TearDown();

  /// Open image A into Interactive through the lifecycle public API.
  void OpenA();

  /// Request a switch from A to B. Starts a save checkpoint when A is open.
  auto RequestSwitchToB() -> NavigationOutcome;

  /// Complete the in-flight save successfully (journal durable + materialize).
  void CompleteCheckpoint();

  /// Complete the in-flight save as a materialization failure.
  void FailCheckpoint(std::string error = "A materialization failed");

  [[nodiscard]] auto events() const -> const std::vector<std::string>& { return events_; }
  [[nodiscard]] auto nav() -> EditorSessionNavigationController& { return *nav_; }
  [[nodiscard]] auto lifecycle() -> EditorSessionLifecycle& { return *lifecycle_; }
  [[nodiscard]] auto save_service() -> EditorSaveCheckpointService& { return *save_service_; }
  [[nodiscard]] auto pipeline() -> FakeEditorPipelinePort& { return pipeline_->inner; }
  [[nodiscard]] auto history() -> FakeEditorHistoryPort& { return history_->inner; }
  [[nodiscard]] auto journal() -> FakeEditorJournalPort& { return *journal_; }
  [[nodiscard]] auto checkpoint_store() -> FakeEditorCheckpointStore& {
    return *checkpoint_store_;
  }

 private:
  void RecordEvent(std::string name);

  /// Pipeline port that records release_a / acquire_b into the event vector.
  class TrackingPipelinePort final : public IEditorPipelinePort {
   public:
    explicit TrackingPipelinePort(EditorSessionNavigationFixture* owner) : owner_(owner) {}
    auto Acquire(sl_element_id_t element_id, std::string* error)
        -> EditorPipelineGuardHandle override;
    void Release(const EditorPipelineGuardHandle& guard) override;

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

    FakeEditorHistoryPort inner;

   private:
    EditorSessionNavigationFixture* owner_ = nullptr;
  };

  std::shared_ptr<TrackingPipelinePort>              pipeline_;
  std::shared_ptr<TrackingHistoryPort>               history_;
  std::shared_ptr<FakeEditorTaskPort>                tasks_;
  std::shared_ptr<FakeEditorJournalPort>             journal_;
  std::shared_ptr<FakeEditorCheckpointStore>         checkpoint_store_;
  std::shared_ptr<FakeEditorRenderSubmitPort>        render_submit_;
  std::shared_ptr<EditorSaveCheckpointCoordinator>   save_coordinator_;
  std::unique_ptr<EditorSessionLifecycle>            lifecycle_;
  std::unique_ptr<EditorSaveCheckpointService>       save_service_;
  std::unique_ptr<EditorSessionRenderController>     render_;
  std::unique_ptr<EditorSessionEditController>       edit_;
  std::unique_ptr<EditorSessionNavigationController> nav_;
  std::vector<std::string>                           events_;
};

}  // namespace alcedo::test
