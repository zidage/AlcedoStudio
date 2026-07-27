//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <mutex>
#include <string>

#include "app/editor_session_ports.hpp"
#include "app/editor_session_types.hpp"
#include "type/type.hpp"

namespace alcedo {

/// Outcome of an image acquire attempt. Returned by Lifecycle::AcquireImage
/// so the facade can decide how to publish the result without reading private
/// state. Only the success path contains a usable identity snapshot.
struct AcquireImageOutcome {
  bool                  accepted = false;
  EditorSessionIdentity identity{};
  std::string           error;
};

/// Outcome of a save-seal operation. Returned by Lifecycle::SealForCheckpoint
/// so the facade can publish the SaveStarted result.
struct SealOutcome {
  bool                  accepted = false;
  EditorSessionIdentity identity{};
  std::string           error;
};

/// Outcome of a release-after-checkpoint. Returned by Lifecycle so the
/// navigation controller knows whether to continue to B or keep A.
struct ReleaseOutcome {
  bool                  released = false;
  EditorSessionIdentity identity{};
};

/// Owns the current editor session image, identity, state, pipeline/history
/// guards, and the last error. All state transitions are semantic: callers
/// name the transition they want; this type validates and applies it. The
/// facade and other controllers never access the fields or the mutex.
class EditorSessionLifecycle final {
 public:
  struct Dependencies {
    std::shared_ptr<IEditorPipelinePort> pipeline;
    std::shared_ptr<IEditorHistoryPort>  history;
  };

  explicit EditorSessionLifecycle(Dependencies dependencies);

  /// Begin acquiring a new image: advance the session generation, transition to
  /// Acquiring (open) or Switching (switch), and recover the journal. On
  /// recovery failure, transitions to Failed and returns false. The caller is
  /// responsible for acquiring guards after this succeeds.
  auto               BeginAcquire(sl_element_id_t element_id, image_id_t image_id, bool is_switch,
                                  IEditorCheckpointStore* checkpoint_store, std::string* error) -> bool;

  /// Acquire pipeline and history guards for the current image. Returns false
  /// and transitions to Failed on failure. Must be called after BeginAcquire.
  auto               AcquireGuards(std::string* error) -> bool;

  /// Mark the image ready after guards succeed. Stays in Loading until the
  /// first frame is presented. Returns the identity snapshot for event
  /// publication.
  auto               MarkImageReady() -> EditorSessionIdentity;

  /// Keep the current image after a save-checkpoint failure. Transitions to
  /// `RetainedImageFailure` (Phase 7A repair) — a non-fatal state where the
  /// image identity, guards, and last frame remain valid so the viewport keeps
  /// showing the retained frame. Recovery actions resolve back to Interactive,
  /// Saving, or the pending navigation target. Guards are retained.
  void               KeepCurrentAfterCheckpointFailure(std::string message);

  /// Release the current image's guards after a successful checkpoint. Returns
  /// the released identity for diagnostics.
  auto               ReleaseAfterCheckpoint() -> ReleaseOutcome;

  /// Release the current image's guards immediately (discard / no-save paths).
  void               ReleaseGuards();

  /// Complete a close after a successful checkpoint: clear identity and
  /// transition to NoImage.
  void               CompleteClose();

  /// Begin shutdown: the caller seals first, then calls this to clear state
  /// and transition to ShuttingDown.
  void               BeginShutdown();

  /// Mark the first frame presented and transition to Interactive. Returns
  /// the identity snapshot if the transition happened, nullopt otherwise.
  auto               MarkFirstFramePresented() -> std::optional<EditorSessionIdentity>;

  /// Retry from Failed or RetainedImageFailure after a discard: transition to
  /// Loading so the pending target can re-acquire. From RetainedImageFailure
  /// the guards stay held; the caller continues the navigation.
  void               BeginRetryFromDiscard();

  /// Transition to Saving. Called by the save controller when a checkpoint
  /// starts, including the Retry Save recovery path from
  /// RetainedImageFailure.
  void               BeginCheckpoint();

  /// Transition back to Interactive after a successful checkpoint with no
  /// pending navigation.
  void               CompleteCheckpoint();

  /// Resume the Interactive state from RetainedImageFailure after the user
  /// cancels the pending navigation (Phase 7A repair). The prior image
  /// identity, guards, and last frame stay published. No-op from other states.
  void               ResumeInteractiveAfterFailure();

  /// Fail the current session with an error message. Used by render and save
  /// controllers when a failure should transition to the fatal `Failed` state
  /// (no retained image). For save/checkpoint failures that should keep the
  /// image visible, use `KeepCurrentAfterCheckpointFailure` instead.
  void               Fail(std::string message);

  /// Read-only snapshot of the current state. Thread-safe.
  [[nodiscard]] auto state() const -> EditorSessionState;
  /// Read-only snapshot of the current identity. Thread-safe.
  [[nodiscard]] auto identity() const -> EditorSessionIdentity;
  /// True when the session has an image (Acquiring/Loading/Interactive/Saving/
  /// Switching).
  [[nodiscard]] auto has_image() const -> bool;
  /// True when the session is active (not NoImage and not ShuttingDown).
  [[nodiscard]] auto active() const -> bool;
  /// Last error message. Thread-safe.
  [[nodiscard]] auto last_error() const -> std::string;
  /// The active history guard handle. Used by the edit controller and save
  /// path to commit adjustments and capture checkpoints. Returns an invalid
  /// handle when no image is acquired.
  [[nodiscard]] auto history_guard() const -> EditorHistoryGuardHandle;
  /// True when the active history guard is valid.
  [[nodiscard]] auto has_history_guard() const -> bool;

  /// Advance the render generation for a content-changing edit or geometry
  /// change. Returns the new render generation.
  auto               AdvanceRenderGeneration() -> std::uint64_t;
  /// Advance the view generation for a pure view transform or detail refresh.
  /// Returns the new view generation.
  auto               AdvanceViewGeneration() -> std::uint64_t;

  /// True when the element/image matches the current identity. Used by the
  /// render controller to filter stale results.
  [[nodiscard]] auto MatchesIdentity(sl_element_id_t element_id, image_id_t image_id,
                                     std::uint64_t session_generation) const -> bool;

 private:
  Dependencies                 deps_;
  mutable std::recursive_mutex mutex_;
  EditorSessionState           state_ = EditorSessionState::NoImage;
  EditorSessionIdentity        identity_{};
  EditorPipelineGuardHandle    pipeline_guard_{};
  EditorHistoryGuardHandle     history_guard_{};
  std::string                  last_error_;
};

}  // namespace alcedo
