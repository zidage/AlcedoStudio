//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/editor_mask_coverage_preview.hpp"

#include <stdexcept>
#include <utility>

namespace alcedo {

EditorMaskCoveragePreview::EditorMaskCoveragePreview(std::shared_ptr<IEditorMaskCoveragePort> port,
                                                     OwnerPoster                              post)
    : port_(std::move(port)), post_(std::move(post)) {}

void EditorMaskCoveragePreview::UpdateCandidate(const ImageTarget&             target,
                                                MaskCoverageCandidateSelection candidate,
                                                MaskCoverageInvertOpacity      mask,
                                                std::uint64_t                  request_id,
                                                EditorMaskCoverageCompletion   on_complete) {
  // The candidate replaces a range probe (D11); its frame replaces the probe coverage.
  range_probe_.reset();
  candidate_active_ = true;
  Request request;
  request.port_request.element_id            = target.element_id;
  request.port_request.image_id              = target.image_id;
  request.port_request.image_load_request_id = target.image_load_request_id;
  request.port_request.request_id            = request_id;
  request.port_request.selection             = std::move(candidate);
  request.port_request.mask                  = mask;
  request.source                             = EditorMaskCoveragePreviewSource::Candidate;
  request.on_complete                        = std::move(on_complete);
  Enqueue(std::move(request));
}

void EditorMaskCoveragePreview::SelectCommitted(const ImageTarget& target, CommittedMask committed,
                                                std::uint64_t                request_id,
                                                EditorMaskCoverageCompletion on_complete) {
  if (candidate_active_) {
    throw std::logic_error("EditorMaskCoveragePreview: a committed selection needs no candidate");
  }
  // The committed AI Mask replaces a range probe (D11); its frame replaces the probe coverage.
  range_probe_.reset();
  committed_ = std::move(committed);
  Enqueue(MakeCommittedRequest(target, *committed_, request_id, std::move(on_complete)));
}

void EditorMaskCoveragePreview::EndCandidate(const ImageTarget& target, std::uint64_t request_id) {
  if (!candidate_active_) {
    return;
  }
  candidate_active_ = false;
  // A range probe that covers the candidate ends with the AI Mask edit.
  range_probe_.reset();
  if (committed_.has_value()) {
    Enqueue(MakeCommittedRequest(target, *committed_, request_id, {}));
    return;
  }
  Hide();
}

void EditorMaskCoveragePreview::EndCandidateAfterCommit(const ImageTarget& target,
                                                        CommittedMask      committed,
                                                        std::uint64_t      request_id) {
  const bool selected = committed_.has_value() && committed_->grade_id == committed.grade_id &&
                        committed_->mask_id == committed.mask_id;
  if (!selected) {
    EndCandidate(target, request_id);
    return;
  }
  // The selected Mask changed: present its new state, also when no candidate was presented.
  candidate_active_ = false;
  range_probe_.reset();
  committed_        = std::move(committed);
  Enqueue(MakeCommittedRequest(target, *committed_, request_id, {}));
}

auto EditorMaskCoveragePreview::Retry(std::uint64_t                request_id,
                                      EditorMaskCoverageCompletion on_complete) -> bool {
  if (status_ != EditorMaskCoveragePreviewStatus::Previous || !newest_.has_value()) {
    return false;
  }
  Request request                 = *newest_;
  request.port_request.request_id = request_id;
  request.on_complete             = std::move(on_complete);
  Enqueue(std::move(request));
  return true;
}

void EditorMaskCoveragePreview::ShowRangeProbe(MaskCoverageProbe probe) {
  if (CommitBlocked()) {
    throw std::logic_error(
        "EditorMaskCoveragePreview: a range probe cannot cover a candidate that failed to present");
  }
  if (candidate_active_) {
    // The probe covers the candidate: its jobs end, and Hide presents the newest one again.
    DiscardJobs();
  } else if (committed_.has_value() || newest_.has_value() || pending_.has_value() ||
             in_flight_.has_value()) {
    // End the AI coverage work; the port clears the channel after the job in flight.
    committed_.reset();
    Hide();
  }
  range_probe_ = std::move(probe);
  SetStatus(EditorMaskCoveragePreviewStatus::Current);
}

void EditorMaskCoveragePreview::HideRangeProbe(std::uint64_t request_id) {
  if (!range_probe_.has_value()) {
    return;
  }
  range_probe_.reset();
  if (candidate_active_ && newest_.has_value()) {
    Request request                 = *newest_;
    request.port_request.request_id = request_id;
    Enqueue(std::move(request));
    return;
  }
  Hide();
}

void EditorMaskCoveragePreview::DiscardJobs() {
  const bool             had_work = pending_.has_value() || in_flight_.has_value();
  std::optional<Request> pending  = std::move(pending_);
  pending_.reset();
  if (in_flight_.has_value()) {
    in_flight_->discarded = true;
  }
  if (pending.has_value()) {
    Finish(*pending, EditorMaskCoverageOutcome::Discarded,
           "The range coverage replaced the coverage preview");
  }
  // The port orders the clear after the job in flight, so its frame is never displayed.
  if (had_work && port_) {
    port_->ClearMaskCoverage();
  }
}

void EditorMaskCoveragePreview::Clear() {
  candidate_active_ = false;
  committed_.reset();
  range_probe_.reset();
  Hide();
}

void EditorMaskCoveragePreview::Hide() {
  const bool had_work = newest_.has_value() || pending_.has_value() || in_flight_.has_value() ||
                        status_ != EditorMaskCoveragePreviewStatus::Hidden;
  newest_.reset();
  std::optional<Request> pending = std::move(pending_);
  pending_.reset();
  if (in_flight_.has_value()) {
    in_flight_->discarded = true;
  }
  SetStatus(EditorMaskCoveragePreviewStatus::Hidden);
  if (pending.has_value()) {
    Finish(*pending, EditorMaskCoverageOutcome::Discarded, "The coverage preview was cleared");
  }
  // The port orders the clear after the job in flight, so its frame is never displayed.
  if (had_work && port_) {
    port_->ClearMaskCoverage();
  }
}

auto EditorMaskCoveragePreview::MakeCommittedRequest(const ImageTarget&           target,
                                                     const CommittedMask&         committed,
                                                     std::uint64_t                request_id,
                                                     EditorMaskCoverageCompletion on_complete)
    -> Request {
  Request request;
  request.port_request.element_id            = target.element_id;
  request.port_request.image_id              = target.image_id;
  request.port_request.image_load_request_id = target.image_load_request_id;
  request.port_request.request_id            = request_id;
  request.port_request.selection             = committed.source;
  request.port_request.mask                  = committed.mask;
  request.source                             = EditorMaskCoveragePreviewSource::CommittedMask;
  request.on_complete                        = std::move(on_complete);
  return request;
}

void EditorMaskCoveragePreview::Enqueue(Request request) {
  newest_                           = request;
  newest_->on_complete              = {};
  std::optional<Request> superseded = std::move(pending_);
  pending_                          = std::move(request);
  if (status_ != EditorMaskCoveragePreviewStatus::Previous) {
    SetStatus(EditorMaskCoveragePreviewStatus::Pending);
  } else {
    Publish();
  }
  if (superseded.has_value()) {
    Finish(*superseded, EditorMaskCoverageOutcome::Superseded,
           "A newer coverage request replaced this one");
  }
  if (!in_flight_.has_value()) {
    DispatchPending();
  }
}

void EditorMaskCoveragePreview::DispatchPending() {
  if (!pending_.has_value() || in_flight_.has_value()) {
    return;
  }
  Request request = std::move(*pending_);
  pending_.reset();
  if (!port_) {
    SetStatus(EditorMaskCoveragePreviewStatus::Previous,
              "Coverage presentation is not available in this editor");
    Finish(request, EditorMaskCoverageOutcome::Failed, error_);
    return;
  }
  const auto  dispatch_id = ++next_dispatch_id_;
  std::string error;
  // The completion runs on the render worker; the owner poster moves it to this thread. It never
  // runs before ScheduleMaskCoverage returns, because the poster never runs a task inline.
  const auto  job_id = port_->ScheduleMaskCoverage(
      request.port_request,
      [post = post_, this, dispatch_id](EditorMaskCoverageJobResult result) {
        post([this, dispatch_id, result = std::move(result)]() mutable {
          OnJobCompleted(dispatch_id, std::move(result));
        });
      },
      &error);
  if (job_id == 0) {
    SetStatus(EditorMaskCoveragePreviewStatus::Previous,
              error.empty() ? "The coverage presentation could not be scheduled" : error);
    Finish(request, EditorMaskCoverageOutcome::Failed, error_);
    return;
  }
  in_flight_ = InFlight{dispatch_id, std::move(request), false};
}

void EditorMaskCoveragePreview::OnJobCompleted(std::uint64_t               dispatch_id,
                                               EditorMaskCoverageJobResult result) {
  if (!in_flight_.has_value() || in_flight_->dispatch_id != dispatch_id) {
    return;
  }
  InFlight finished = std::move(*in_flight_);
  in_flight_.reset();
  if (finished.discarded) {
    Finish(finished.request, EditorMaskCoverageOutcome::Discarded,
           "The coverage preview ended before the frame was displayed");
    DispatchPending();
    return;
  }
  switch (result.status) {
    case EditorMaskCoverageJobStatus::Presented:
      // The newest request is presented only when no newer one waits.
      if (!pending_.has_value()) {
        SetStatus(EditorMaskCoveragePreviewStatus::Current);
      }
      Finish(finished.request, EditorMaskCoverageOutcome::Presented, "Coverage presented");
      break;
    case EditorMaskCoverageJobStatus::DiscardedForImageChange:
      Finish(finished.request, EditorMaskCoverageOutcome::Discarded,
             result.message.empty() ? "The viewport changed image before the coverage frame"
                                    : std::move(result.message));
      break;
    case EditorMaskCoverageJobStatus::Failed:
      SetStatus(EditorMaskCoveragePreviewStatus::Previous,
                result.message.empty() ? "The coverage presentation failed" : result.message);
      Finish(finished.request, EditorMaskCoverageOutcome::Failed, error_);
      break;
  }
  DispatchPending();
}

auto EditorMaskCoveragePreview::CommitBlocked() const -> bool {
  return candidate_active_ && status_ == EditorMaskCoveragePreviewStatus::Previous;
}

auto EditorMaskCoveragePreview::State() const -> EditorMaskCoveragePreviewState {
  std::scoped_lock lock(published_mutex_);
  return published_;
}

void EditorMaskCoveragePreview::Finish(Request& request, EditorMaskCoverageOutcome outcome,
                                       std::string message) {
  if (!request.on_complete) {
    return;
  }
  auto on_complete = std::move(request.on_complete);
  on_complete({outcome, std::move(message), State()});
}

void EditorMaskCoveragePreview::SetStatus(EditorMaskCoveragePreviewStatus status,
                                          std::string                     error) {
  status_ = status;
  error_  = status == EditorMaskCoveragePreviewStatus::Previous ? std::move(error) : std::string{};
  Publish();
}

void EditorMaskCoveragePreview::Publish() {
  EditorMaskCoveragePreviewState state;
  // A probe can cover the candidate (MR4); it is then the displayed source.
  state.source      = range_probe_.has_value() ? EditorMaskCoveragePreviewSource::RangeProbe
                      : candidate_active_      ? EditorMaskCoveragePreviewSource::Candidate
                      : committed_.has_value() ? EditorMaskCoveragePreviewSource::CommittedMask
                                               : EditorMaskCoveragePreviewSource::None;
  state.status      = status_;
  state.error       = error_;
  state.range_probe = range_probe_;
  std::scoped_lock lock(published_mutex_);
  published_ = std::move(state);
}

}  // namespace alcedo
