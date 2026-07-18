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
#include "app/editor_session_ports.hpp"
#include "app/editor_session_types.hpp"

namespace alcedo {

/// Narrow backend surface for EditorSessionController tests. Production uses
/// EditorSessionService; tests may inject a recording fake.
class IEditorSessionBackend {
 public:
  virtual ~IEditorSessionBackend() = default;

  [[nodiscard]] virtual auto state() const -> EditorSessionState = 0;
  [[nodiscard]] virtual auto identity() const -> EditorSessionIdentity = 0;
  [[nodiscard]] virtual auto active() const -> bool = 0;
  [[nodiscard]] virtual auto has_image() const -> bool = 0;
  [[nodiscard]] virtual auto last_error() const -> std::string = 0;

  virtual void SetPresentationSinkId(PresentationSinkId sink_id) = 0;
  virtual auto Open(sl_element_id_t element_id, image_id_t image_id) -> EditorSessionResult = 0;
  virtual auto Switch(sl_element_id_t element_id, image_id_t image_id) -> EditorSessionResult = 0;
  virtual auto Shutdown() -> EditorSessionResult = 0;
  virtual auto Discard() -> EditorSessionResult = 0;
  virtual auto Undo() -> EditorSessionResult = 0;
  virtual auto Redo() -> EditorSessionResult = 0;
};

/// Application-layer owner of the active image session (Phase 5A).
///
/// Acquires pipeline/history guards, sequences session/render generations, and
/// routes typed intents. UI modules (EditorSessionController) submit intents
/// only; they never receive pipeline, history, journal, or scheduler handles.
/// Does not depend on AlbumBackend, QWidget, ApplicationModuleHost, or a service
/// locator.
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

  void SetResultObserver(ResultObserver observer);

  [[nodiscard]] auto state() const -> EditorSessionState override { return state_; }
  [[nodiscard]] auto identity() const -> EditorSessionIdentity override { return identity_; }
  [[nodiscard]] auto active() const -> bool override {
    return state_ != EditorSessionState::NoImage && state_ != EditorSessionState::ShuttingDown;
  }
  [[nodiscard]] auto has_image() const -> bool override {
    return identity_.element_id > 0 && identity_.image_id > 0 && EditorSessionHasImage(state_);
  }
  [[nodiscard]] auto last_error() const -> std::string override { return last_error_; }
  [[nodiscard]] auto presentation_sink_id() const -> PresentationSinkId {
    return presentation_sink_id_;
  }
  [[nodiscard]] auto results() const -> const std::vector<EditorSessionResult>& {
    return results_;
  }

  /// Bind the presentation sink identity used on subsequent render intents.
  void SetPresentationSinkId(PresentationSinkId sink_id) override;

  /// Primary API: typed session intents.
  auto Submit(const EditorSessionIntent& intent) -> EditorSessionResult;

  // Convenience wrappers used by the QML controller.
  auto Open(sl_element_id_t element_id, image_id_t image_id) -> EditorSessionResult override;
  auto Switch(sl_element_id_t element_id, image_id_t image_id) -> EditorSessionResult override;
  auto Patch(std::string patch_key) -> EditorSessionResult;
  auto GestureCommit(std::string patch_key) -> EditorSessionResult;
  auto Undo() -> EditorSessionResult override;
  auto Redo() -> EditorSessionResult override;
  auto Discard() -> EditorSessionResult override;
  auto Shutdown() -> EditorSessionResult override;

  /// Feed async completions that may arrive out of order relative to load/render/save.
  void NotifyImageAcquired(std::uint64_t session_generation, bool success, std::string message = {});
  void NotifySaveFinished(std::uint64_t session_generation, bool success, std::string message = {});
  void NotifyRenderResult(const EditorRenderResult& render_result);

  /// Build a fully-stamped render intent for the active session. Returns nullopt
  /// when no image is active.
  [[nodiscard]] auto MakeRenderIntent(EditorRenderReason reason) const
      -> std::optional<EditorRenderIntent>;

 private:
  auto TransitionTo(EditorSessionState next, EditorSessionResultKind kind,
                    std::string message = {}) -> EditorSessionResult;
  auto Reject(std::string message) -> EditorSessionResult;
  auto Emit(EditorSessionResult result) -> EditorSessionResult;
  void ReleaseGuards();
  auto AcquireGuards(sl_element_id_t element_id, std::string* error) -> bool;
  auto RouteInitialRender(EditorRenderReason reason) -> std::uint64_t;
  auto HandleOpenOrSwitch(const EditorSessionIntent& intent, bool is_switch)
      -> EditorSessionResult;
  auto HandlePatch(const EditorSessionIntent& intent, bool settled) -> EditorSessionResult;
  auto HandleUndoRedo(bool undo) -> EditorSessionResult;
  auto HandleDiscard() -> EditorSessionResult;
  auto HandleShutdown() -> EditorSessionResult;

  Dependencies            dependencies_;
  ResultObserver          observer_;
  EditorSessionState      state_ = EditorSessionState::NoImage;
  EditorSessionIdentity   identity_{};
  PresentationSinkId      presentation_sink_id_ = 0;
  EditorPipelineGuardHandle pipeline_guard_{};
  EditorHistoryGuardHandle  history_guard_{};
  std::string               last_error_;
  std::vector<EditorSessionResult> results_;
  /// Session generation of an in-flight save barrier while switching.
  std::uint64_t             pending_save_generation_ = 0;
  bool                      pending_save_            = false;
};

}  // namespace alcedo
