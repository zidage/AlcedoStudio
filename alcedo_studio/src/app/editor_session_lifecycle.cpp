//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/editor_session_lifecycle.hpp"

#include <utility>

namespace alcedo {

EditorSessionLifecycle::EditorSessionLifecycle(Dependencies dependencies)
    : deps_(std::move(dependencies)), owner_thread_(std::this_thread::get_id()) {}

auto EditorSessionLifecycle::BeginAcquire(sl_element_id_t element_id, image_id_t image_id,
                                          bool is_switch, IEditorCheckpointStore* checkpoint_store,
                                          std::string* error) -> bool {
  AssertOwnerThread();
  active_load_request_.value = next_load_request_id_++;
  identity_.element_id       = element_id;
  identity_.image_id         = image_id;
  state_ = is_switch ? EditorSessionState::Switching : EditorSessionState::Acquiring;
  last_error_.clear();

  if (checkpoint_store != nullptr) {
    std::string recover_error;
    const auto  recovered = checkpoint_store->RecoverAndMaterialize(
        element_id, active_load_request_.value, &recover_error);
    if (!recovered.accepted) {
      state_               = EditorSessionState::Failed;
      last_error_          = recover_error.empty() ? "Editor journal recovery failed" : recover_error;
      identity_.element_id = 0;
      identity_.image_id   = 0;
      active_load_request_ = {};
      if (error) {
        *error = last_error_;
      }
      return false;
    }
  }
  return true;
}

auto EditorSessionLifecycle::AcquireGuards(std::string* error) -> bool {
  AssertOwnerThread();
  if (!deps_.pipeline || !deps_.history) {
    if (error) {
      *error = "Pipeline or history port is missing";
    }
    state_      = EditorSessionState::Failed;
    last_error_ = error != nullptr ? *error : "Pipeline or history port is missing";
    return false;
  }
  pipeline_guard_ = deps_.pipeline->Acquire(identity_.element_id, error);
  if (!pipeline_guard_.valid) {
    state_      = EditorSessionState::Failed;
    last_error_ = error != nullptr ? *error : "Pipeline acquire failed";
    return false;
  }
  history_guard_ = deps_.history->Acquire(identity_.element_id, error);
  if (!history_guard_.valid) {
    deps_.pipeline->Release(pipeline_guard_);
    pipeline_guard_ = {};
    state_          = EditorSessionState::Failed;
    last_error_     = error != nullptr ? *error : "History acquire failed";
    return false;
  }
  return true;
}

auto EditorSessionLifecycle::MarkImageReady() -> EditorSessionIdentity {
  AssertOwnerThread();
  state_ = EditorSessionState::Loading;
  return identity_;
}

void EditorSessionLifecycle::KeepCurrentAfterCheckpointFailure(std::string message) {
  AssertOwnerThread();
  // Phase 7A repair: keep the image visible. RetainedImageFailure preserves
  // identity, guards, and the last presented frame so the viewport does not
  // fall back to the empty-editor placeholder. Recovery actions (Retry Save,
  // Discard and Continue, Cancel) resolve from here.
  state_      = EditorSessionState::RetainedImageFailure;
  last_error_ = std::move(message);
}

auto EditorSessionLifecycle::ReleaseAfterCheckpoint() -> ReleaseOutcome {
  AssertOwnerThread();
  ReleaseOutcome outcome;
  outcome.identity = identity_;
  if (deps_.history && history_guard_.valid) {
    deps_.history->Release(history_guard_);
  }
  history_guard_ = {};
  if (deps_.pipeline && pipeline_guard_.valid) {
    deps_.pipeline->Release(pipeline_guard_);
  }
  pipeline_guard_  = {};
  outcome.released = true;
  return outcome;
}

void EditorSessionLifecycle::ReleaseGuards() {
  AssertOwnerThread();
  if (deps_.history && history_guard_.valid) {
    deps_.history->Release(history_guard_);
  }
  history_guard_ = {};
  if (deps_.pipeline && pipeline_guard_.valid) {
    deps_.pipeline->Release(pipeline_guard_);
  }
  pipeline_guard_ = {};
}

void EditorSessionLifecycle::CompleteClose() {
  AssertOwnerThread();
  identity_.element_id = 0;
  identity_.image_id   = 0;
  active_load_request_ = {};
  state_               = EditorSessionState::NoImage;
}

void EditorSessionLifecycle::BeginShutdown() {
  AssertOwnerThread();
  identity_.element_id = 0;
  identity_.image_id   = 0;
  active_load_request_ = {};
  state_               = EditorSessionState::ShuttingDown;
}

auto EditorSessionLifecycle::MarkFirstFrameReady() -> std::optional<EditorSessionIdentity> {
  AssertOwnerThread();
  if (state_ != EditorSessionState::Loading && state_ != EditorSessionState::Acquiring &&
      state_ != EditorSessionState::Switching) {
    return std::nullopt;
  }
  state_ = EditorSessionState::Interactive;
  return identity_;
}

void EditorSessionLifecycle::BeginRetryFromDiscard() {
  AssertOwnerThread();
  state_ = EditorSessionState::Loading;
}

void EditorSessionLifecycle::BeginCheckpoint() {
  AssertOwnerThread();
  state_ = EditorSessionState::Saving;
}

void EditorSessionLifecycle::CompleteCheckpoint() {
  AssertOwnerThread();
  if (state_ == EditorSessionState::Saving) {
    state_ = EditorSessionState::Interactive;
  }
}

void EditorSessionLifecycle::ResumeInteractiveAfterFailure() {
  AssertOwnerThread();
  if (state_ == EditorSessionState::RetainedImageFailure) {
    state_ = EditorSessionState::Interactive;
    last_error_.clear();
  }
}

void EditorSessionLifecycle::Fail(std::string message) {
  AssertOwnerThread();
  state_      = EditorSessionState::Failed;
  last_error_ = std::move(message);
}

auto EditorSessionLifecycle::state() const -> EditorSessionState {
  AssertOwnerThread();
  return state_;
}

auto EditorSessionLifecycle::identity() const -> EditorSessionIdentity {
  AssertOwnerThread();
  return identity_;
}

auto EditorSessionLifecycle::active_image_load_request() const -> ImageLoadRequestId {
  AssertOwnerThread();
  return active_load_request_;
}

auto EditorSessionLifecycle::has_image() const -> bool {
  AssertOwnerThread();
  return identity_.element_id > 0 && identity_.image_id > 0 && EditorSessionHasImage(state_);
}

auto EditorSessionLifecycle::active() const -> bool {
  AssertOwnerThread();
  return state_ != EditorSessionState::NoImage && state_ != EditorSessionState::ShuttingDown;
}

auto EditorSessionLifecycle::last_error() const -> std::string {
  AssertOwnerThread();
  return last_error_;
}

auto EditorSessionLifecycle::history_guard() const -> EditorHistoryGuardHandle {
  AssertOwnerThread();
  return history_guard_;
}

auto EditorSessionLifecycle::has_history_guard() const -> bool {
  AssertOwnerThread();
  return history_guard_.valid;
}

auto EditorSessionLifecycle::MatchesIdentity(sl_element_id_t element_id, image_id_t image_id) const
    -> bool {
  AssertOwnerThread();
  return identity_.element_id == element_id && identity_.image_id == image_id;
}

auto EditorSessionLifecycle::MatchesImageLoadRequest(ImageLoadRequestId request) const -> bool {
  AssertOwnerThread();
  return request.valid() && request == active_load_request_;
}

}  // namespace alcedo
