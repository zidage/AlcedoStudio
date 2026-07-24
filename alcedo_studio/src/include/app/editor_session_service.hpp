//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "app/editor_render_intent.hpp"
#include "app/editor_save_checkpoint_service.hpp"
#include "app/editor_session_edit_controller.hpp"
#include "app/editor_session_lifecycle.hpp"
#include "app/editor_session_navigation_controller.hpp"
#include "app/editor_session_ports.hpp"
#include "app/editor_session_render_controller.hpp"
#include "app/editor_session_types.hpp"

namespace alcedo {

/// Narrow backend surface for EditorSessionController tests. Production uses
/// EditorSessionService; tests may inject a recording fake.
class IEditorSessionBackend {
 public:
  virtual ~IEditorSessionBackend()                                     = default;

  using ChangeNotifier                                                 = std::function<void()>;

  [[nodiscard]] virtual auto state() const -> EditorSessionState       = 0;
  [[nodiscard]] virtual auto identity() const -> EditorSessionIdentity = 0;
  [[nodiscard]] virtual auto active() const -> bool                    = 0;
  [[nodiscard]] virtual auto has_image() const -> bool                 = 0;
  [[nodiscard]] virtual auto last_error() const -> std::string         = 0;
  /// Read-only snapshot of the committed editor adjustment state (panel values,
  /// not runtime pipeline handles). Returns the default-constructed empty
  /// snapshot when the backend has no image or the history port is unavailable.
  [[nodiscard]] virtual auto adjustment_snapshot() const -> EditorRenderAdjustmentSnapshot {
    return {};
  }


  /// Optional: notified after state/identity changes from async results.
  virtual void               SetChangeNotifier(ChangeNotifier notifier) {
    change_notifier_ = std::move(notifier);
  }

  virtual void SetPresentationSinkId(PresentationSinkId sink_id)                              = 0;
  virtual void SetPresentationSize(int width, int height)                                     = 0;
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
  virtual auto Close(bool persist_changes) -> EditorSessionResult                             = 0;
  virtual auto Shutdown() -> EditorSessionResult                                              = 0;
  virtual auto Discard() -> EditorSessionResult                                               = 0;
  virtual auto Undo() -> EditorSessionResult                                                  = 0;
  virtual auto Redo() -> EditorSessionResult                                                  = 0;
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
  virtual auto RecordFinalizedEdit(const EditTransaction& /*transaction*/, std::string* /*error*/)
      -> bool {
    return false;
  }
  virtual auto RecordHistoryCursorMove(std::uint64_t /*from_cursor*/, std::uint64_t /*to_cursor*/,
                                       std::string* /*error*/) -> bool {
    return false;
  }
  virtual auto RecordTimelineRewrite(const Hash128& /*expected_timeline_hash*/,
                                     const Hash128& /*discarded_tail_hash*/,
                                     std::uint64_t /*retained_cursor*/,
                                     const EditTransaction& /*replacement*/, std::string* /*error*/)
      -> bool {
    return false;
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

  ChangeNotifier change_notifier_;
};

/// Thin facade that owns five focused collaborators and routes typed intents.
/// Does not own any business state: no pending saves, no pending navigation, no
/// adjustment snapshot, no first-frame flags, no render-busy tracking. Each
/// collaborator owns its own state and mutex.
class EditorSessionService final : public IEditorSessionBackend {
 public:
  struct Dependencies {
    std::shared_ptr<IEditorPipelinePort>                 pipeline;
    std::shared_ptr<IEditorHistoryPort>                  history;
    std::shared_ptr<IEditorTaskPort>                     tasks;
    std::shared_ptr<IEditorJournalPort>                  journal;
    std::shared_ptr<IEditorCheckpointStore>              checkpoint_store;
    std::shared_ptr<IEditorThumbnailPort>                thumbnails;
    std::shared_ptr<IEditorRenderSubmitPort>             render;
    /// Project-owned global save lock shared with Mini-Git materialization.
    std::shared_ptr<EditorSaveCheckpointCoordinator>    save_coordinator;
  };

  using ResultObserver = std::function<void(const EditorSessionResult&)>;

  explicit EditorSessionService(Dependencies dependencies);
  ~EditorSessionService() override;

  void               SetResultObserver(ResultObserver observer);
  void               SetChangeNotifier(ChangeNotifier notifier) override;

  [[nodiscard]] auto state() const -> EditorSessionState override { return lifecycle_.state(); }
  [[nodiscard]] auto identity() const -> EditorSessionIdentity override {
    return lifecycle_.identity();
  }
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
  [[nodiscard]] auto adjustment_snapshot() const -> EditorRenderAdjustmentSnapshot override {
    return edit_.adjustment_snapshot();
  }
  [[nodiscard]] auto presentation_sink_id() const -> PresentationSinkId {
    return render_.presentation_sink_id();
  }

  void SetPresentationSinkId(PresentationSinkId sink_id) override {
    render_.SetPresentationSinkId(sink_id);
    NotifyChange();
  }
  void SetPresentationSize(int width, int height) override {
    render_.SetPresentationSize(width, height);
    NotifyChange();
  }

  auto Open(sl_element_id_t element_id, image_id_t image_id) -> EditorSessionResult override;
  auto Switch(sl_element_id_t element_id, image_id_t image_id) -> EditorSessionResult override;
  auto CheckoutVersion(const version_ref_id_t& version_id) -> EditorSessionResult override;
  auto Close(bool persist_changes) -> EditorSessionResult override;
  auto Patch(EditorAdjustmentPatch patch) -> EditorSessionResult override;
  auto CommitAdjustment(EditorAdjustmentPatch patch) -> EditorSessionResult override;
  auto Patch(std::string patch_key) -> EditorSessionResult;
  auto CommitAdjustment(std::string patch_key) -> EditorSessionResult;
  auto Undo() -> EditorSessionResult override;
  auto Redo() -> EditorSessionResult override;
  auto RecordFinalizedEdit(const EditTransaction& transaction, std::string* error) -> bool override;
  auto RecordHistoryCursorMove(std::uint64_t from_cursor, std::uint64_t to_cursor,
                               std::string* error) -> bool override;
  auto RecordTimelineRewrite(const Hash128& expected_timeline_hash,
                             const Hash128& discarded_tail_hash, std::uint64_t retained_cursor,
                             const EditTransaction& replacement, std::string* error)
      -> bool override;
  auto Discard() -> EditorSessionResult override;
  auto Shutdown() -> EditorSessionResult override;
  auto RequestViewChange(EditorRenderReason reason, std::optional<ViewportRenderRegion> region)
      -> EditorSessionResult override;
  [[nodiscard]] auto render_busy() const -> bool override { return render_.render_busy(); }
  [[nodiscard]] auto render_diagnostics() const -> EditorRenderCoordinatorDiagnostics override {
    return render_.render_diagnostics();
  }
  [[nodiscard]] auto first_frame_time_ms() const -> double override {
    return render_.first_frame_time_ms();
  }

  /// Accept an asynchronous image-acquisition completion for the active
  /// session generation. Stale generations and states outside image loading
  /// are ignored. A failed acquisition releases guards and publishes failure;
  /// success keeps the session loading until its first frame is presented.
  void NotifyImageAcquired(std::uint64_t session_generation, bool success,
                           std::string message = {});
  void NotifyRenderResult(const EditorRenderResult& render_result) {
    render_.NotifyRenderResult(render_result, lifecycle_.identity(), lifecycle_.state());
  }

 private:
  /// Publish a result to the observer and change-notifier. The only state the
  /// facade owns is the result history and observer registration.
  auto                              Emit(EditorSessionResult result) -> EditorSessionResult;
  auto                              Reject(std::string message) -> EditorSessionResult;
  /// Transition lifecycle to Failed and emit a Failed result. Used when a
  /// navigation or save failure requires the session to enter the Failed state.
  auto                              Fail(std::string message) -> EditorSessionResult;

  Dependencies                      dependencies_;
  ResultObserver                    observer_;
  EditorSessionLifecycle            lifecycle_;
  EditorSaveCheckpointService       save_service_;
  EditorSessionRenderController     render_;
  EditorSessionEditController       edit_;
  EditorSessionNavigationController navigation_;
  std::vector<EditorSessionResult>  results_;
  mutable std::mutex                results_mutex_;
};

}  // namespace alcedo
