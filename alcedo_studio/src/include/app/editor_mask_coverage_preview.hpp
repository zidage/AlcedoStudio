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

#include "app/editor_mask_coverage_port.hpp"
#include "app/editor_session_request_ids.hpp"
#include "edit/graph/graph_ids.hpp"
#include "edit/mask/mask_model.hpp"
#include "edit/runtime/mask_coverage_selection.hpp"
#include "type/type.hpp"

namespace alcedo {

/// The source that the coverage preview of the editor shows (SM8 C8): one at a time.
enum class EditorMaskCoveragePreviewSource : std::uint8_t {
  /// No coverage; the photo is displayed alone.
  None,
  /// The candidate of the open AI Mask edit.
  Candidate,
  /// One committed AI Mask of a Color Grade.
  CommittedMask,
};

/// Whether the displayed coverage is the coverage that the editor asked for last (section 4.3).
enum class EditorMaskCoveragePreviewStatus : std::uint8_t {
  /// No coverage is selected.
  Hidden,
  /// The newest request is not presented yet; the viewport keeps the coverage it displayed.
  Pending,
  /// The newest request is presented.
  Current,
  /// A presentation failed after the last Current state: the viewport shows an earlier result,
  /// not the newest request. Stays until the newest request is presented by a Retry or a later
  /// request. While the candidate is in this state, the AI Mask edit cannot commit.
  Previous,
};

/// Published state of the coverage preview.
struct EditorMaskCoveragePreviewState {
  EditorMaskCoveragePreviewSource source = EditorMaskCoveragePreviewSource::None;
  EditorMaskCoveragePreviewStatus status = EditorMaskCoveragePreviewStatus::Hidden;
  /// The real error of the failed presentation; set only in the Previous status.
  std::string                     error;
};

/// Terminal outcome of one coverage preview request.
enum class EditorMaskCoverageOutcome : std::uint8_t {
  /// The GPU work of the request completed and its frame replaces the displayed coverage.
  Presented,
  /// A newer request replaced it before it ran. The newer request carries the complete state
  /// (pixels and edge values), so no session state is lost.
  Superseded,
  /// The presentation failed; the viewport keeps the coverage it displayed (status Previous).
  Failed,
  /// An image change, the end of the candidate, or a clear ended it before it was displayed.
  Discarded,
  /// The session did not accept the request; the message says why. The preview is unchanged.
  Rejected,
};

/// Outcome of one request and the preview state after it.
struct EditorMaskCoverageResult {
  EditorMaskCoverageOutcome      outcome = EditorMaskCoverageOutcome::Rejected;
  std::string                    message;
  EditorMaskCoveragePreviewState state;
};

/// Receives the outcome of one coverage preview request, once, on the session owner thread.
using EditorMaskCoverageCompletion = std::function<void(EditorMaskCoverageResult result)>;

/**
 * @brief Owner of the coverage preview selection and of its presentation requests (SM8.7,
 *        sections 4.2 and 4.3).
 *
 * The preview shows one source: the candidate of the open AI Mask edit, else the selected
 * committed AI Mask, else nothing. Every request carries the complete coverage of its source (the
 * shared original R8 or the committed source, the edge values, invert, and opacity), so one
 * request in flight plus the newest pending request are enough: a newer request supersedes the
 * pending one and loses no state. A request runs on the editor render executor through
 * IEditorMaskCoveragePort; its frame replaces the displayed coverage only after its GPU work
 * completes, and a failure keeps the displayed coverage and labels it as the previous result.
 *
 * The preview holds the shared read of the newest candidate pixels for Retry; it never copies
 * them. It does not change the document or the history.
 *
 * Threading: the session owner thread for every method except @ref State. Port completions reach
 * it through the poster given at construction, which runs them on the owner thread.
 */
class EditorMaskCoveragePreview {
 public:
  /// Runs a task on the session owner thread, never inline.
  using OwnerPoster = std::function<void(std::function<void()> task)>;

  /// The image that a request presents on.
  struct ImageTarget {
    sl_element_id_t    element_id = 0;
    image_id_t         image_id   = 0;
    ImageLoadRequestId image_load_request_id{};
  };

  /// One committed AI Mask and its Mask fields, read from the working document by the caller.
  struct CommittedMask {
    NodeId                    grade_id;
    MaskId                    mask_id;
    AiSegmentationMaskSource  source;
    MaskCoverageInvertOpacity mask{};
  };

  /// @p port may be null: every request then fails with the real reason.
  EditorMaskCoveragePreview(std::shared_ptr<IEditorMaskCoveragePort> port, OwnerPoster post);

  EditorMaskCoveragePreview(const EditorMaskCoveragePreview&)                    = delete;
  auto operator=(const EditorMaskCoveragePreview&) -> EditorMaskCoveragePreview& = delete;

  /// Show @p candidate of the open AI Mask edit with @p mask. The candidate stays the source until
  /// @ref EndCandidate, @ref EndCandidateAfterCommit, or @ref Clear.
  void UpdateCandidate(const ImageTarget& target, MaskCoverageCandidateSelection candidate,
                       MaskCoverageInvertOpacity mask, std::uint64_t request_id,
                       EditorMaskCoverageCompletion on_complete);

  /// Select and present @p committed; @ref EndCandidate restores it later.
  /// @pre No candidate is the source (the session refuses a selection during an AI Mask edit).
  /// @throws std::logic_error when a candidate is the source.
  void SelectCommitted(const ImageTarget& target, CommittedMask committed, std::uint64_t request_id,
                       EditorMaskCoverageCompletion on_complete);

  /**
   * @brief End the candidate (Cancel): restore the selected committed AI Mask, or hide the
   *        coverage when none is selected. The candidate frame stays displayed until the committed
   *        frame replaces it. No effect without a candidate.
   */
  void EndCandidate(const ImageTarget& target, std::uint64_t request_id);

  /**
   * @brief End the candidate after its Commit (SM9 #516). @p committed is the state of the
   *        committed Mask after the Commit.
   *
   * The Commit does not select the committed Mask: the coverage stays shown only when the user
   * selected a committed Mask before the edit. When that selection is the committed Mask, its new
   * state replaces the candidate frame after its GPU work completes; any other selection is
   * restored as in @ref EndCandidate; without a selection the coverage is hidden.
   */
  void EndCandidateAfterCommit(const ImageTarget& target, CommittedMask committed,
                               std::uint64_t request_id);

  /**
   * @brief Present the newest request again after a failure. Reuses its shared pixels and edge
   *        values; no model call.
   * @return False, with @p on_complete not called, when the status is not Previous.
   */
  auto Retry(std::uint64_t request_id, EditorMaskCoverageCompletion on_complete) -> bool;

  /**
   * @brief Forget the candidate and the committed selection and hide the coverage after the job in
   *        flight (ClearMaskCoveragePreview, image change, close). Ends the pending request and the
   *        job in flight as Discarded. Idempotent.
   */
  void Clear();

  /// Handle the completion of the job of dispatch @p dispatch_id. Owner thread.
  void OnJobCompleted(std::uint64_t dispatch_id, EditorMaskCoverageJobResult result);

  /// True while the candidate is the source and its newest request is not presented after a
  /// failure (status Previous): the AI Mask edit must not commit.
  [[nodiscard]] auto CommitBlocked() const -> bool;
  [[nodiscard]] auto HasCandidate() const -> bool { return candidate_active_; }
  [[nodiscard]] auto Committed() const -> const std::optional<CommittedMask>& { return committed_; }
  /// Published state. Any thread.
  [[nodiscard]] auto State() const -> EditorMaskCoveragePreviewState;

 private:
  struct Request {
    EditorMaskCoverageRequest       port_request;
    EditorMaskCoveragePreviewSource source = EditorMaskCoveragePreviewSource::None;
    EditorMaskCoverageCompletion    on_complete;
  };
  struct InFlight {
    std::uint64_t dispatch_id = 0;
    Request       request;
    /// Set when a clear, image change, or candidate end made the job obsolete.
    bool          discarded = false;
  };

  [[nodiscard]] static auto MakeCommittedRequest(const ImageTarget&           target,
                                                 const CommittedMask&         committed,
                                                 std::uint64_t                request_id,
                                                 EditorMaskCoverageCompletion on_complete)
      -> Request;
  /// Make @p request the newest one and dispatch it when no job runs.
  void Enqueue(Request request);
  void DispatchPending();
  /// Discard the pending request and the job in flight, forget the newest request, and hide the
  /// coverage through the port.
  void Hide();
  void Finish(Request& request, EditorMaskCoverageOutcome outcome, std::string message);
  void SetStatus(EditorMaskCoveragePreviewStatus status, std::string error = {});
  void Publish();

  std::shared_ptr<IEditorMaskCoveragePort> port_;
  OwnerPoster                              post_;
  bool                                     candidate_active_ = false;
  std::optional<CommittedMask>             committed_;
  /// The newest request without its completion; Retry presents it again.
  std::optional<Request>                   newest_;
  std::optional<Request>                   pending_;
  std::optional<InFlight>                  in_flight_;
  std::uint64_t                            next_dispatch_id_ = 0;
  EditorMaskCoveragePreviewStatus          status_ = EditorMaskCoveragePreviewStatus::Hidden;
  std::string                              error_;

  mutable std::mutex                       published_mutex_;
  EditorMaskCoveragePreviewState           published_;
};

}  // namespace alcedo
