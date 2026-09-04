//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "app/adjustment_transfer_types.hpp"
#include "app/editor_action_policy.hpp"
#include "app/editor_session_request_ids.hpp"
#include "app/editor_render_intent.hpp"
#include "app/editor_save_checkpoint_service.hpp"
#include "app/editor_session_command_queue.hpp"
#include "app/editor_session_edit_controller.hpp"
#include "app/editor_session_lifecycle.hpp"
#include "app/editor_session_navigation_controller.hpp"
#include "app/editor_session_ports.hpp"
#include "app/editor_session_render_controller.hpp"
#include "app/editor_session_types.hpp"

namespace alcedo {

class PipelineDocument;

/// Background-task restrictions pushed by the controller or task port into the
/// session service for action evaluation.
struct EditorBackgroundActionRestrictions {
  bool blocks_select_image = false;
  bool blocks_paste        = false;
  bool blocks_checkout     = false;
  bool blocks_workspace    = false;
};

/// Presentation target queued with the active image-load operation.
struct EditorPendingPresentationTarget {
  sl_element_id_t    element_id         = 0;
  image_id_t         image_id           = 0;
  ImageLoadRequestId image_load_request{};
};

/// Narrow backend surface for EditorSessionController tests. Production uses
/// EditorSessionService; tests may inject a recording fake.
class IEditorSessionBackend {
 public:
  virtual ~IEditorSessionBackend() = default;

  using ChangeNotifier             = std::function<void()>;
  /// Optional: notified for every typed session result that crosses Emit /
  /// NotifyResult. Phase 7A R4 uses this so the controller can publish a
  /// correlated terminal HistoryOperationEvent after an async save checkpoint
  /// completes. Does not replace SetChangeNotifier.
  using ResultObserver             = std::function<void(const EditorSessionResult&)>;
  /// Optional: notified when evaluated editor action availability changes.
  using ActionAvailabilityObserver = std::function<void(const EditorActionAvailability&)>;

  [[nodiscard]] virtual auto state() const -> EditorSessionState       = 0;
  [[nodiscard]] virtual auto identity() const -> EditorSessionIdentity = 0;
  [[nodiscard]] virtual auto active() const -> bool                    = 0;
  [[nodiscard]] virtual auto has_image() const -> bool                 = 0;
  [[nodiscard]] virtual auto has_pending_recovery() const -> bool { return false; }
  [[nodiscard]] virtual auto has_unmaterialized_changes() -> bool { return false; }
  [[nodiscard]] virtual auto last_error() const -> std::string = 0;
  /// Read-only snapshot of the committed editor adjustment state (panel values,
  /// not runtime pipeline handles). Returns the default-constructed empty
  /// snapshot when the backend has no image or the history port is unavailable.
  [[nodiscard]] virtual auto adjustment_snapshot() const -> EditorRenderAdjustmentSnapshot {
    return {};
  }
  /// Monotonic counter incremented only when history-display-affecting state
  /// changes (active Version ID, a Version ref creation/removal/rename/head
  /// move, working head or redo suffix, visible commit set, recovered-head
  /// marker). Render-busy, frame-ready, progress, preview, viewport, and
  /// task-detail notifications do not increment it. The controller compares it
  /// across backend change notifications to emit one dedicated history signal
  /// instead of refreshing on every renderer event.
  [[nodiscard]] virtual auto history_revision() const -> std::uint64_t { return 0; }
  [[nodiscard]] virtual auto history_snapshot() -> EditorHistorySnapshot { return {}; }
  /// Live PipelineDocument for the Nodes-page projection. Null when the backend
  /// has no loaded image document. Default fakes return nullptr.
  [[nodiscard]] virtual auto pipeline_document() const -> const PipelineDocument* { return nullptr; }

  /// Optional: notified after state/identity changes from async results.
  virtual void               SetChangeNotifier(ChangeNotifier notifier) {
    change_notifier_ = std::move(notifier);
  }

  /// Optional: notified for each typed EditorSessionResult published by the
  /// backend. Production installs this from EditorSessionController. Fakes may
  /// call NotifyResult from CompletePendingVersionOp-style helpers.
  virtual void SetResultObserver(ResultObserver observer) {
    result_observer_ = std::move(observer);
  }

  virtual void SetPresentationSinkId(PresentationSinkId sink_id) = 0;
  virtual void SetPresentationSize(int width, int height)        = 0;
  /// Keep geometry editing on the source-frame overlay until the panel closes.
  /// Backends that do not render through the unified session path may ignore it.
  virtual void SetGeometryOverlayActive(bool /*active*/) {}
  virtual auto Open(sl_element_id_t element_id, image_id_t image_id) -> EditorSessionResult   = 0;
  virtual auto Switch(sl_element_id_t element_id, image_id_t image_id) -> EditorSessionResult = 0;
  /// Check out another Version on the open image after a save checkpoint.
  virtual auto CheckoutVersion(const version_ref_id_t& /*version_id*/) -> EditorSessionResult {
    EditorSessionResult result;
    result.kind    = EditorSessionResultKind::Rejected;
    result.state   = EditorSessionState::NoImage;
    result.message = "Version checkout is not supported by this backend";
    return result;
  }
  /// Phase 7A: create a new Version at the image root and check it out after
  /// a save checkpoint. Replaces the ambiguous active-head CreateVersion.
  virtual auto CreateRootVersion(std::string /*display_name*/) -> EditorSessionResult {
    EditorSessionResult result;
    result.kind     = EditorSessionResultKind::Rejected;
    result.state    = state();
    result.identity = identity();
    result.message  = "Root Version creation is not supported by this backend";
    return result;
  }
  /// Phase 7A: create a new Version at an explicit commit and check it out
  /// after a save checkpoint.
  virtual auto BranchFromCommit(const commit_hash_t& /*commit_id*/, std::string /*display_name*/)
      -> EditorSessionResult {
    EditorSessionResult result;
    result.kind     = EditorSessionResultKind::Rejected;
    result.state    = state();
    result.identity = identity();
    result.message  = "Branch creation is not supported by this backend";
    return result;
  }
  /// Phase 7A: retry the save checkpoint after a retained-image failure.
  virtual auto RetrySave() -> EditorSessionResult {
    EditorSessionResult result;
    result.kind     = EditorSessionResultKind::Rejected;
    result.state    = state();
    result.identity = identity();
    result.message  = "Retry Save is not supported by this backend";
    return result;
  }
  /// Phase 7A: discard unflushed changes and continue the pending navigation.
  virtual auto DiscardAndContinue() -> EditorSessionResult {
    EditorSessionResult result;
    result.kind     = EditorSessionResultKind::Rejected;
    result.state    = state();
    result.identity = identity();
    result.message  = "Discard and continue is not supported by this backend";
    return result;
  }
  /// Phase 7A: cancel the pending navigation and resume Interactive.
  virtual auto CancelPendingNavigation() -> EditorSessionResult {
    EditorSessionResult result;
    result.kind     = EditorSessionResultKind::Rejected;
    result.state    = state();
    result.identity = identity();
    result.message  = "Cancel pending navigation is not supported by this backend";
    return result;
  }
  virtual auto RenameVersion(const version_ref_id_t& /*version_id*/, std::string /*display_name*/)
      -> EditorSessionResult {
    EditorSessionResult result;
    result.kind     = EditorSessionResultKind::Rejected;
    result.state    = state();
    result.identity = identity();
    result.message  = "Version rename is not supported by this backend";
    return result;
  }
  virtual auto RemoveVersion(const version_ref_id_t& /*version_id*/) -> EditorSessionResult {
    EditorSessionResult result;
    result.kind     = EditorSessionResultKind::Rejected;
    result.state    = state();
    result.identity = identity();
    result.message  = "Version removal is not supported by this backend";
    return result;
  }
  virtual auto PasteAdjustments(const AdjustmentTransferPackage& /*package*/,
                                std::string /*version_display_name*/) -> EditorSessionResult {
    EditorSessionResult result;
    result.kind     = EditorSessionResultKind::Rejected;
    result.state    = state();
    result.identity = identity();
    result.message  = "Editor Paste is not supported by this backend";
    return result;
  }
  virtual auto Close(bool persist_changes) -> EditorSessionResult = 0;
  virtual auto Shutdown() -> EditorSessionResult                  = 0;
  virtual auto Discard() -> EditorSessionResult                   = 0;
  virtual auto Undo() -> EditorSessionResult                      = 0;
  virtual auto Redo() -> EditorSessionResult                      = 0;
  /// Phase 7A P1: move the working head to an explicit commit in one operation.
  /// The target must be an ancestor of the working head or a member of the
  /// in-memory redo suffix. Default rejects so fakes opt in.
  virtual auto MoveHeadToCommit(const commit_hash_t& /*commit_id*/) -> EditorSessionResult {
    EditorSessionResult result;
    result.kind     = EditorSessionResultKind::Rejected;
    result.state    = state();
    result.identity = identity();
    result.message  = "Editor head move is not supported by this backend";
    return result;
  }
  virtual auto Patch(EditorAdjustmentPatch /*patch*/) -> EditorSessionResult {
    EditorSessionResult result;
    result.kind    = EditorSessionResultKind::Rejected;
    result.state   = EditorSessionState::NoImage;
    result.message = "Patch not supported by this backend";
    return result;
  }
  virtual auto CommitAdjustment(EditorAdjustmentPatch /*patch*/) -> EditorSessionResult {
    EditorSessionResult result;
    result.kind    = EditorSessionResultKind::Rejected;
    result.state   = EditorSessionState::NoImage;
    result.message = "Adjustment commit not supported by this backend";
    return result;
  }
  virtual auto RequestViewChange(EditorRenderReason /*reason*/,
                                 std::optional<ViewportRenderRegion> /*region*/)
      -> EditorSessionResult {
    EditorSessionResult result;
    result.kind    = EditorSessionResultKind::Rejected;
    result.state   = EditorSessionState::NoImage;
    result.message = "View change not supported by this backend";
    return result;
  }
  [[nodiscard]] virtual auto render_busy() const -> bool { return false; }
  [[nodiscard]] virtual auto action_availability() const -> EditorActionAvailability {
    return {};
  }
  [[nodiscard]] virtual auto active_image_load_request() const -> ImageLoadRequestId {
    return {};
  }
  [[nodiscard]] virtual auto pending_presentation_target() const
      -> std::optional<EditorPendingPresentationTarget> {
    return std::nullopt;
  }
  virtual void SetCopiedPackageAvailable(bool /*available*/) {}
  virtual void SetBackgroundActionRestrictions(
      const EditorBackgroundActionRestrictions& /*restrictions*/) {}
  virtual void SetActionAvailabilityObserver(ActionAvailabilityObserver /*observer*/) {}
  [[nodiscard]] virtual auto render_diagnostics() const -> EditorRenderCoordinatorDiagnostics {
    return {};
  }
  [[nodiscard]] virtual auto first_frame_time_ms() const -> double { return -1.0; }

 protected:
  void NotifyChange() {
    if (change_notifier_) {
      change_notifier_();
    }
  }

  /// Deliver a typed session result to the installed observer. Does not call
  /// NotifyChange — callers that also mutate visible state should NotifyChange
  /// separately (or go through EditorSessionService::Emit).
  void NotifyResult(const EditorSessionResult& result) {
    if (result_observer_) {
      result_observer_(result);
    }
  }

  ChangeNotifier change_notifier_;
  ResultObserver result_observer_;
};

/// Thin facade that owns five focused collaborators and routes typed intents.
/// Does not own any business state: no pending saves, no pending navigation, no
/// adjustment snapshot, no first-frame flags, no render-busy tracking. Each
/// collaborator owns its own state and mutex.
class EditorSessionService final : public IEditorSessionBackend {
 public:
  struct Dependencies {
    std::shared_ptr<IEditorPipelinePort>             pipeline;
    std::shared_ptr<IEditorHistoryPort>              history;
    std::shared_ptr<IEditorTaskPort>                 tasks;
    std::shared_ptr<IEditorJournalPort>              journal;
    std::shared_ptr<IEditorCheckpointStore>          checkpoint_store;
    std::shared_ptr<IEditorThumbnailPort>            thumbnails;
    std::shared_ptr<IEditorRenderSubmitPort>         render;
    /// Project-owned global save lock shared with Mini-Git materialization.
    std::shared_ptr<EditorSaveCheckpointCoordinator> save_coordinator;
    /// Delivery port for the thread that owns the session reducer. When null,
    /// the runtime uses a deterministic manual executor.
    std::shared_ptr<IEditorSessionCommandExecutor>   command_executor;
  };

  explicit EditorSessionService(Dependencies dependencies);
  ~EditorSessionService() override;

  void               SetResultObserver(ResultObserver observer) override;
  void               SetChangeNotifier(ChangeNotifier notifier) override;
  void               SetActionAvailabilityObserver(ActionAvailabilityObserver observer) override;

  [[nodiscard]] auto state() const -> EditorSessionState override { return lifecycle_.state(); }
  [[nodiscard]] auto identity() const -> EditorSessionIdentity override {
    return lifecycle_.identity();
  }
  [[nodiscard]] auto active_image_load_request() const -> ImageLoadRequestId override {
    return lifecycle_.active_image_load_request();
  }
  [[nodiscard]] auto action_availability() const -> EditorActionAvailability override {
    return published_availability_;
  }
  [[nodiscard]] auto pending_presentation_target() const
      -> std::optional<EditorPendingPresentationTarget> override {
    return pending_presentation_target_;
  }
  void SetCopiedPackageAvailable(bool available) override;
  void SetBackgroundActionRestrictions(
      const EditorBackgroundActionRestrictions& restrictions) override;
  [[nodiscard]] auto active() const -> bool override { return lifecycle_.active(); }
  [[nodiscard]] auto has_image() const -> bool override { return lifecycle_.has_image(); }
  [[nodiscard]] auto last_error() const -> std::string override { return lifecycle_.last_error(); }
  [[nodiscard]] auto results() const -> std::vector<EditorSessionResult> {
    std::scoped_lock lock(results_mutex_);
    return results_;
  }
  [[nodiscard]] auto first_frame_request_id() const -> std::uint64_t {
    return render_.first_frame_request_id();
  }
  [[nodiscard]] auto adjustment_snapshot() const -> EditorRenderAdjustmentSnapshot override;
  [[nodiscard]] auto history_revision() const -> std::uint64_t override {
    return history_revision_.load(std::memory_order_acquire);
  }
  [[nodiscard]] auto history_snapshot() -> EditorHistorySnapshot override;
  [[nodiscard]] auto pipeline_document() const -> const PipelineDocument* override;
  [[nodiscard]] auto presentation_sink_id() const -> PresentationSinkId {
    return render_.presentation_sink_id();
  }
  /// Test/runtime diagnostics for posted worker completions.
  [[nodiscard]] auto command_executor() const
      -> const std::shared_ptr<IEditorSessionCommandExecutor>& {
    return command_queue_.executor();
  }
  void DrainCommandQueueForTests();

  void SetPresentationSinkId(PresentationSinkId sink_id) override;
  void SetPresentationSize(int width, int height) override;
  void SetGeometryOverlayActive(bool active) override;

  auto Open(sl_element_id_t element_id, image_id_t image_id) -> EditorSessionResult override;
  auto Switch(sl_element_id_t element_id, image_id_t image_id) -> EditorSessionResult override;
  auto CheckoutVersion(const version_ref_id_t& version_id) -> EditorSessionResult override;
  /// Phase 7A: create a root Version and check it out.
  auto CreateRootVersion(std::string display_name) -> EditorSessionResult override;
  /// Phase 7A: branch from a commit and check it out.
  auto BranchFromCommit(const commit_hash_t& commit_id, std::string display_name)
      -> EditorSessionResult override;
  /// Phase 7A: retry the save checkpoint after a retained-image failure.
  auto RetrySave() -> EditorSessionResult override;
  /// Phase 7A: discard unflushed changes and continue the pending navigation.
  auto DiscardAndContinue() -> EditorSessionResult override;
  /// Phase 7A: cancel the pending navigation and resume Interactive.
  auto CancelPendingNavigation() -> EditorSessionResult override;
  auto RenameVersion(const version_ref_id_t& version_id, std::string display_name)
      -> EditorSessionResult override;
  auto RemoveVersion(const version_ref_id_t& version_id) -> EditorSessionResult override;
  auto PasteAdjustments(const AdjustmentTransferPackage& package, std::string version_display_name)
      -> EditorSessionResult override;
  auto               Close(bool persist_changes) -> EditorSessionResult override;
  [[nodiscard]] auto render_busy() const -> bool override { return render_.render_busy(); }
  /// Phase 7A: true when the session is awaiting save-failure recovery.
  [[nodiscard]] auto has_pending_recovery() const -> bool override {
    return navigation_.has_pending_recovery();
  }
  [[nodiscard]] auto has_unmaterialized_changes() -> bool override;
  auto               Patch(EditorAdjustmentPatch patch) -> EditorSessionResult override;
  auto               CommitAdjustment(EditorAdjustmentPatch patch) -> EditorSessionResult override;
  auto               Patch(std::string patch_key) -> EditorSessionResult;
  auto               CommitAdjustment(std::string patch_key) -> EditorSessionResult;
  auto               Undo() -> EditorSessionResult override;
  auto               Redo() -> EditorSessionResult override;
  auto MoveHeadToCommit(const commit_hash_t& commit_id) -> EditorSessionResult override;
  auto Discard() -> EditorSessionResult override;
  auto Shutdown() -> EditorSessionResult override;
  auto RequestViewChange(EditorRenderReason reason, std::optional<ViewportRenderRegion> region)
      -> EditorSessionResult override;
  [[nodiscard]] auto render_diagnostics() const -> EditorRenderCoordinatorDiagnostics override {
    return render_.render_diagnostics();
  }
  [[nodiscard]] auto first_frame_time_ms() const -> double override {
    return render_.first_frame_time_ms();
  }

  /// Accept an asynchronous image-acquisition completion for the active
  /// session generation. Stale generations and states outside image loading
  /// are ignored. A failed acquisition releases guards and publishes failure;
  /// success keeps the session loading until its first frame is ready.
  void NotifyImageAcquired(ImageLoadRequestId image_load_request, bool success,
                           std::string message = {});
  void NotifyRenderResult(const EditorRenderResult& render_result);

 private:
  using CommandReducer = std::function<EditorSessionResult(const EditorSessionCommand&)>;

  /// Submit a typed command and reduce it on the session owner thread. The
  /// result is available immediately for owner-thread callers and is a queued
  /// acknowledgement for callers from other threads.
  auto SubmitCommand(EditorSessionCommand command, CommandReducer reducer) -> EditorSessionResult;
  void BeginPublication();
  void EndPublication();
  void PostCompletion(EditorSessionCompletion completion);
  void HandleRenderEvent(const EditorRenderEvent& event);
  void HandleNavigationCompletion(const NavigationCompletion& completion);
  void HandleImageAcquiredCompletion(const EditorSessionCompletion& completion);
  void HandleSaveCheckpointCompletion(const EditorSessionCompletion& completion);
  void PublishActionAvailabilityIfChanged();
  [[nodiscard]] auto BuildActionInputs() -> EditorActionInputs;
  void AcquireLease(EditorOperationLeaseKind kind, std::uint64_t command_id,
                    sl_element_id_t element_id, image_id_t image_id,
                    ImageLoadRequestId image_load_request, std::string blocking_reason = {});
  void ReleaseLeaseByCommandId(std::uint64_t command_id);
  void ReleaseLeasesByKind(EditorOperationLeaseKind kind);
  void SetPendingPresentationTarget(sl_element_id_t element_id, image_id_t image_id,
                                    ImageLoadRequestId image_load_request);
  void ClearPendingPresentationTarget();

  /// Publish a result to the observer and change-notifier. The only state the
  /// facade owns is the result history and observer registration.
  auto Emit(EditorSessionResult result) -> EditorSessionResult;
  auto Reject(std::string message) -> EditorSessionResult;
  /// Transition lifecycle to Failed and emit a Failed result. Used when a
  /// navigation or save failure requires the session to enter the Failed state.
  auto Fail(std::string message) -> EditorSessionResult;
  auto FinishVersionNavigation(const NavigationOutcome& outcome) -> EditorSessionResult;
  /// Persist a graph mutation through the same journal/materialization path as
  /// ordinary editor saves. The terminal outcome is delivered as a typed
  /// SaveCheckpointFinished completion and reduced on the owner thread; the
  /// publish flavor is remembered in `pending_history_checkpoint_`.
  auto StartHistoryCheckpoint(std::string success_message, bool route_render)
      -> EditorSessionResult;
  /// Shared machinery behind every history save checkpoint: precondition
  /// checks, FinalizeEdit, save-lock acquisition, immutable capture, and the
  /// save-service start whose result is posted back as SaveCheckpointFinished.
  /// Publishes SaveStarted (or Rejected/Failed) for the current operation.
  auto StartHistoryCheckpointSave() -> EditorSessionResult;
  /// Increment the history revision so the controller emits one dedicated
  /// history signal on the next change notification. Call only at points where
  /// history-display-affecting state actually changed (settled commit, head
  /// move, Version ref change, image open/close, recovery). Never call for
  /// interactive preview, render routing, view changes, or presentation size.
  void BumpHistoryRevision() { history_revision_.fetch_add(1, std::memory_order_acq_rel); }

  /// Queue-owned publish flavor for one in-flight history save checkpoint.
  /// Set by StartHistoryCheckpoint and consumed by the matching
  /// SaveCheckpointFinished completion.
  struct PendingHistoryCheckpoint {
    bool        route_render = false;
    std::string success_message;
  };

  Dependencies                            dependencies_;
  EditorSessionCommandQueue               command_queue_;
  EditorSessionNavigationState            navigation_state_;
  EditorSessionLifecycle                  lifecycle_;
  EditorSaveCheckpointService             save_service_;
  EditorSessionRenderController           render_;
  EditorSessionEditController             edit_;
  EditorSessionNavigationController       navigation_;
  bool                                    reducing_command_     = false;
  std::uint64_t                           current_operation_id_ = 0;
  std::size_t                             publication_depth_    = 0;
  bool                                    publication_dirty_    = false;
  std::vector<EditorSessionResult>        results_;
  mutable std::mutex                      results_mutex_;
  std::optional<PendingHistoryCheckpoint>          pending_history_checkpoint_;
  std::vector<EditorOperationLease>       active_leases_;
  EditorActionAvailability                published_availability_{};
  ActionAvailabilityObserver              action_availability_observer_;
  std::optional<EditorPendingPresentationTarget> pending_presentation_target_;
  bool                                    package_available_ = false;
  EditorBackgroundActionRestrictions      background_restrictions_{};
  std::atomic<std::uint64_t>              history_revision_{0};
};

}  // namespace alcedo
