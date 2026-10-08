//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/scope/final_display_frame_tap.hpp"

#include <utility>

namespace alcedo {
namespace {

auto ScopeResourceShapeMatches(const ScopeRequest& lhs, const ScopeRequest& rhs) -> bool {
  return lhs.histogram_bins == rhs.histogram_bins && lhs.waveform_width == rhs.waveform_width &&
         lhs.waveform_height == rhs.waveform_height &&
         lhs.vectorscope_size == rhs.vectorscope_size &&
         lhs.chromaticity_size == rhs.chromaticity_size &&
         lhs.analysis_downsample == rhs.analysis_downsample;
}

}  // namespace

FinalDisplayFrameTapSink::FinalDisplayFrameTapSink(IFrameSink*                     downstream_sink,
                                                   std::shared_ptr<IScopeAnalyzer> scope_analyzer)
    : downstream_sink_(downstream_sink), scope_analyzer_(std::move(scope_analyzer)) {}

void FinalDisplayFrameTapSink::SetDownstreamSink(IFrameSink* downstream_sink) {
  std::lock_guard<std::mutex> lock(mutex_);
  downstream_sink_ = downstream_sink;
}

auto FinalDisplayFrameTapSink::downstream_sink() const -> IFrameSink* {
  std::lock_guard<std::mutex> lock(mutex_);
  return downstream_sink_;
}

void FinalDisplayFrameTapSink::SetScopeRequest(const ScopeRequest& request) {
  FinalDisplayFrameView scope_frame;
  bool                  active           = false;
  bool                  deferred         = false;
  bool                  resize_resources = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    resize_resources = !ScopeResourceShapeMatches(scope_request_, request);
    scope_request_   = request;
    scope_frame      = scope_frame_;
    active           = scope_active_;
    deferred         = scope_analysis_deferred_;
  }
  if (scope_analyzer_) {
    // Changing only the enabled plot must retain the staged full-frame input.
    // Backend ResizeResources resets CUDA/Metal/OpenCL staging slots, which
    // otherwise leaves a newly selected plot blank until another edit frame.
    if (resize_resources) {
      scope_analyzer_->ResizeResources(request);
    }
    if (active && !deferred && scope_frame) {
      scope_analyzer_->SubmitFrame(scope_frame, request);
    }
  }
}

void FinalDisplayFrameTapSink::SetScopeActive(bool active) {
  ScopeRequest          request;
  FinalDisplayFrameView scope_frame;
  bool                  deferred = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (scope_active_ == active) {
      return;
    }
    scope_active_ = active;
    request       = scope_request_;
    scope_frame   = scope_frame_;
    deferred      = scope_analysis_deferred_;
  }
  if (!scope_analyzer_ || !active) {
    return;
  }

  if (!deferred && scope_frame) {
    scope_analyzer_->SubmitFrame(scope_frame, request);
  }
}

void FinalDisplayFrameTapSink::SetScopeAnalysisDeferred(bool deferred) {
  std::lock_guard<std::mutex> lock(mutex_);
  scope_analysis_deferred_ = deferred;
}

void FinalDisplayFrameTapSink::SetFrameIdentity(uint64_t image_identity, uint64_t session_epoch) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (image_identity_ != image_identity || session_epoch_ != session_epoch) {
    current_frame_          = {};
    scope_frame_            = {};
    bound_submission_       = {};
    bound_submission_valid_ = false;
  }
  image_identity_ = image_identity;
  session_epoch_  = session_epoch;
}

auto FinalDisplayFrameTapSink::GetScopeRequest() const -> ScopeRequest {
  std::lock_guard<std::mutex> lock(mutex_);
  return scope_request_;
}

auto FinalDisplayFrameTapSink::GetCurrentDisplayFrameView() const -> FinalDisplayFrameView {
  std::lock_guard<std::mutex> lock(mutex_);
  return current_frame_;
}

auto FinalDisplayFrameTapSink::GetCurrentScopeFrameView() const -> FinalDisplayFrameView {
  std::lock_guard<std::mutex> lock(mutex_);
  return scope_frame_;
}

auto FinalDisplayFrameTapSink::SubmitCurrentDisplayFrameToScope() -> bool {
  ScopeRequest          request;
  FinalDisplayFrameView scope_frame;
  bool                  active = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    request     = scope_request_;
    scope_frame = scope_frame_;
    active      = scope_active_;
  }
  if (!scope_analyzer_ || !active || !scope_frame) {
    return false;
  }
  scope_analyzer_->SubmitFrame(scope_frame, request);
  return true;
}

void FinalDisplayFrameTapSink::EnsureSize(int width, int height) {
  if (auto* sink = downstream_sink()) {
    sink->EnsureSize(width, height);
  }
}

auto FinalDisplayFrameTapSink::MapResourceForWrite(FrameMemoryDomain preferred_domain)
    -> FrameWriteMapping {
  if (auto* sink = downstream_sink()) {
    return sink->MapResourceForWrite(preferred_domain);
  }
  return {};
}

void FinalDisplayFrameTapSink::UnmapResource() {
  if (auto* sink = downstream_sink()) {
    sink->UnmapResource();
  }
}

void FinalDisplayFrameTapSink::BindFrameSubmission(const FrameCompletionSubmission& submission) {
  IFrameSink* downstream = nullptr;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    bound_submission_       = submission;
    bound_submission_valid_ = true;
    downstream              = downstream_sink_;
  }
  // The tap decorates the presentation sink; it must not terminate submission
  // metadata propagation. DirectFrameSink needs the request id, frame role and
  // presentation mode before EnsureSize/MapResourceForWrite so DetailPatch
  // allocation cannot be mistaken for a full-frame render reference.
  if (downstream) {
    downstream->BindFrameSubmission(submission);
  }
}

void FinalDisplayFrameTapSink::NotifyFrameReady(const FrameCompletionSubmission& submission) {
  if (auto* sink = downstream_sink()) {
    sink->NotifyFrameReady(submission);
  }
}

void FinalDisplayFrameTapSink::SubmitHostFrame(const ViewerFrame& frame) {
  if (auto* sink = downstream_sink()) {
    sink->SubmitHostFrame(frame);
  }
}

#ifdef HAVE_METAL
void FinalDisplayFrameTapSink::SubmitMetalFrame(const ViewerMetalFrame& frame) {
  if (auto* sink = downstream_sink()) {
    sink->SubmitMetalFrame(frame);
  }
}
#endif

void FinalDisplayFrameTapSink::SubmitFinalDisplayFrame(const FinalDisplayFrameView& frame) {
  FinalDisplayFrameView stamped_frame = frame;
  ScopeRequest          request;
  bool                  scope_update_allowed = true;
  bool                  active               = false;
  bool                  deferred             = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (bound_submission_valid_) {
      if (bound_submission_.metadata.image_identity != 0 ||
          bound_submission_.metadata.session_epoch != 0) {
        stamped_frame.image_identity = bound_submission_.metadata.image_identity;
        stamped_frame.session_epoch  = bound_submission_.metadata.session_epoch;
      }
      stamped_frame.display_generation = bound_submission_.metadata.presentation_request_id != 0
                                             ? bound_submission_.metadata.presentation_request_id
                                             : bound_submission_.metadata.preview_generation;
      scope_update_allowed             = bound_submission_.metadata.scope_update_allowed;
      bound_submission_valid_          = false;
    }
    if (stamped_frame.image_identity == 0 && image_identity_ != 0) {
      stamped_frame.image_identity = image_identity_;
    }
    if (stamped_frame.session_epoch == 0 && session_epoch_ != 0) {
      stamped_frame.session_epoch = session_epoch_;
    }
    if (stamped_frame.frame_id == 0) {
      stamped_frame.frame_id = stamped_frame.display_generation != 0
                                   ? stamped_frame.display_generation
                                   : next_frame_id_++;
    }
    current_frame_ = stamped_frame;
    request        = scope_request_;
    active         = scope_active_;
    deferred       = scope_analysis_deferred_;
  }

  // Stage a stable, analyzer-owned copy NOW on the render thread, while the
  // pipeline source (result_ptr / stream) is still valid. The deferred poll
  // later analyzes this staged frame instead of the pipeline's reused scratch.
  FinalDisplayFrameView staged_frame;
  if (scope_analyzer_ && scope_update_allowed && stamped_frame) {
    staged_frame = scope_analyzer_->StageFrame(stamped_frame, request);
  }

  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (staged_frame) {
      scope_frame_ = std::move(staged_frame);
    }
    // Staging only failed (no idle slot / throttle / non-owning backend): keep
    // the previous staged frame rather than the pipeline's reused source.
  }

  if (scope_analyzer_ && active && !deferred && scope_update_allowed) {
    FinalDisplayFrameView to_submit;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      to_submit = scope_frame_;
    }
    if (to_submit) {
      scope_analyzer_->SubmitFrame(to_submit, request);
    }
  }
}

auto FinalDisplayFrameTapSink::GetWidth() const -> int {
  if (auto* sink = downstream_sink()) {
    return sink->GetWidth();
  }
  return 0;
}

auto FinalDisplayFrameTapSink::GetHeight() const -> int {
  if (auto* sink = downstream_sink()) {
    return sink->GetHeight();
  }
  return 0;
}

auto FinalDisplayFrameTapSink::GetViewportRenderRegion() const
    -> std::optional<ViewportRenderRegion> {
  if (auto* sink = downstream_sink()) {
    return sink->GetViewportRenderRegion();
  }
  return std::nullopt;
}

}  // namespace alcedo
