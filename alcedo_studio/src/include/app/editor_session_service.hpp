//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "app/editor_render_intent.hpp"
#include "app/editor_session_ports.hpp"
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

  /// Optional: notified after state/identity changes from async results.
  virtual void               SetChangeNotifier(ChangeNotifier notifier) {
    change_notifier_ = std::move(notifier);
  }

  virtual void SetPresentationSinkId(PresentationSinkId sink_id)                              = 0;
  virtual void SetPresentationSize(int width, int height)                                     = 0;
  virtual auto Open(sl_element_id_t element_id, image_id_t image_id) -> EditorSessionResult   = 0;
  virtual auto Switch(sl_element_id_t element_id, image_id_t image_id) -> EditorSessionResult = 0;
  virtual auto Close(bool persist_changes) -> EditorSessionResult                             = 0;
  virtual auto Shutdown() -> EditorSessionResult                                              = 0;
  virtual auto Discard() -> EditorSessionResult                                               = 0;
  virtual auto Undo() -> EditorSessionResult                                                  = 0;
  virtual auto Redo() -> EditorSessionResult                                                  = 0;

 protected:
  void NotifyChange() {
    if (change_notifier_) {
      change_notifier_();
    }
  }

  ChangeNotifier change_notifier_;
};

/// Application-layer owner of the active image session (Phase 5A).
///
/// Acquires pipeline/history guards, sequences session/render generations, and
/// routes typed intents. UI modules (EditorSessionController) submit intents
/// only; they never receive pipeline, history, journal, or scheduler handles.
class EditorSessionService final : public IEditorSessionBackend {
 public:
  struct Dependencies {
    std::shared_ptr<IEditorPipelinePort>     pipeline;
    std::shared_ptr<IEditorHistoryPort>      history;
    std::shared_ptr<IEditorTaskPort>         tasks;
    std::shared_ptr<IEditorJournalPort>      journal;
    std::shared_ptr<IEditorRenderSubmitPort> render;
  };

  using ResultObserver = std::function<void(const EditorSessionResult&)>;

  explicit EditorSessionService(Dependencies dependencies);

  void               SetResultObserver(ResultObserver observer);
  void               SetChangeNotifier(ChangeNotifier notifier) override;

  [[nodiscard]] auto state() const -> EditorSessionState override {
    std::scoped_lock lock(mutex_);
    return state_;
  }
  [[nodiscard]] auto identity() const -> EditorSessionIdentity override {
    std::scoped_lock lock(mutex_);
    return identity_;
  }
  [[nodiscard]] auto active() const -> bool override {
    std::scoped_lock lock(mutex_);
    return state_ != EditorSessionState::NoImage && state_ != EditorSessionState::ShuttingDown;
  }
  [[nodiscard]] auto has_image() const -> bool override {
    std::scoped_lock lock(mutex_);
    return identity_.element_id > 0 && identity_.image_id > 0 && EditorSessionHasImage(state_);
  }
  [[nodiscard]] auto last_error() const -> std::string override {
    std::scoped_lock lock(mutex_);
    return last_error_;
  }
  [[nodiscard]] auto presentation_sink_id() const -> PresentationSinkId {
    std::scoped_lock lock(mutex_);
    return presentation_sink_id_;
  }
  [[nodiscard]] auto results() const -> std::vector<EditorSessionResult> {
    std::scoped_lock lock(mutex_);
    return results_;
  }
  [[nodiscard]] auto first_frame_request_id() const -> std::uint64_t {
    std::scoped_lock lock(mutex_);
    return first_frame_request_id_;
  }
  [[nodiscard]] auto adjustment_snapshot() const -> EditorRenderAdjustmentSnapshot {
    std::scoped_lock lock(mutex_);
    return adjustment_snapshot_;
  }

  /// Bind the presentation sink identity used on subsequent render intents.
  void SetPresentationSinkId(PresentationSinkId sink_id) override;
  void SetPresentationSize(int width, int height) override;

  /// Primary API: typed session intents.
  auto Submit(const EditorSessionIntent& intent) -> EditorSessionResult;

  // Convenience wrappers used by the QML controller.
  auto Open(sl_element_id_t element_id, image_id_t image_id) -> EditorSessionResult override;
  auto Switch(sl_element_id_t element_id, image_id_t image_id) -> EditorSessionResult override;
  auto Close(bool persist_changes) -> EditorSessionResult override;
  auto Patch(EditorAdjustmentPatch patch) -> EditorSessionResult;
  auto GestureCommit(EditorAdjustmentPatch patch) -> EditorSessionResult;
  /// Legacy convenience: field key only.
  auto Patch(std::string patch_key) -> EditorSessionResult;
  auto GestureCommit(std::string patch_key) -> EditorSessionResult;
  auto Undo() -> EditorSessionResult override;
  auto Redo() -> EditorSessionResult override;
  auto Discard() -> EditorSessionResult override;
  auto Shutdown() -> EditorSessionResult override;

  /// Feed async completions that may arrive out of order relative to load/render/save.
  void NotifyImageAcquired(std::uint64_t session_generation, bool success,
                           std::string message = {});
  void NotifySaveFinished(std::uint64_t session_generation, bool success, std::string message = {});
  void NotifyRenderResult(const EditorRenderResult& render_result);

  /// Build a fully-stamped render intent for the active session. Returns nullopt
  /// when no image is active.
  [[nodiscard]] auto MakeRenderIntent(EditorRenderReason reason) const
      -> std::optional<EditorRenderIntent>;

 private:
  struct PendingSave {
    std::uint64_t session_generation = 0;
    std::uint64_t task_id            = 0;
  };

  auto TransitionTo(EditorSessionState next, EditorSessionResultKind kind, std::string message = {})
      -> EditorSessionResult;
  auto Reject(std::string message) -> EditorSessionResult;
  auto Emit(EditorSessionResult result) -> EditorSessionResult;
  void ReleaseGuards();
  auto AcquireGuards(sl_element_id_t element_id, std::string* error) -> bool;
  auto RouteInitialRender(EditorRenderReason reason) -> std::uint64_t;
  /// After the InteractivePrimary first frame is presented, enqueue the normal
  /// QualityBase follow-up. Never a prerequisite for leaving Loading.
  auto RouteQualityBaseFollowUp() -> std::uint64_t;
  auto HandleOpenOrSwitch(const EditorSessionIntent& intent, bool is_switch) -> EditorSessionResult;
  auto HandleClose(bool persist_changes) -> EditorSessionResult;
  auto HandlePatch(const EditorSessionIntent& intent, bool settled) -> EditorSessionResult;
  auto HandleUndoRedo(bool undo) -> EditorSessionResult;
  auto HandleDiscard() -> EditorSessionResult;
  auto HandleShutdown() -> EditorSessionResult;
  auto BeginSaveForSession(std::uint64_t session_generation, sl_element_id_t element_id,
                           std::string* error) -> bool;
  auto SealCurrentSession(bool persist_changes, bool start_background_save, std::string* error)
      -> bool;
  void               ResetActiveImageState();
  void               RoutePendingInitialRender();
  [[nodiscard]] auto PresentationTargetReady() const -> bool;
  [[nodiscard]] auto MatchesActiveFirstFrame(const EditorRenderResult& render_result) const -> bool;
  void               TryEnterInteractiveFromFirstFrame(const EditorRenderResult& render_result);
  /// Mark the image ready to render after pipeline/history guards succeed.
  /// First-frame Interactive still requires complete→submit→present.
  void               MarkImageAcquiredAfterGuards();

  Dependencies       dependencies_;
  ResultObserver     observer_;
  EditorSessionState state_ = EditorSessionState::NoImage;
  EditorSessionIdentity             identity_{};
  PresentationSinkId                presentation_sink_id_ = 0;
  int                               presentation_width_   = 0;
  int                               presentation_height_  = 0;
  EditorPipelineGuardHandle         pipeline_guard_{};
  EditorHistoryGuardHandle          history_guard_{};
  std::string                       last_error_;
  std::vector<EditorSessionResult>  results_;
  /// Concurrent in-flight saves keyed by the sealed session generation (A→B→C).
  std::vector<PendingSave>          pending_saves_;
  EditorRenderAdjustmentSnapshot    adjustment_snapshot_{};
  std::uint64_t                     first_frame_request_id_ = 0;
  std::uint64_t                     quality_base_request_id_ = 0;
  bool                              image_acquired_         = false;
  bool                              first_frame_completed_  = false;
  bool                              first_frame_submitted_  = false;
  bool                              first_frame_presented_  = false;
  bool                              quality_base_routed_    = false;
  std::optional<EditorRenderReason> pending_initial_reason_;
  mutable std::recursive_mutex      mutex_;
};

}  // namespace alcedo
