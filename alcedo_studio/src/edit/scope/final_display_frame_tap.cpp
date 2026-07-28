//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/scope/final_display_frame_tap.hpp"

#include <utility>

namespace alcedo {

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
  bool                  active   = false;
  bool                  deferred = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    scope_request_ = request;
    scope_frame    = scope_frame_;
    active         = scope_active_;
    deferred       = scope_analysis_deferred_;
  }
  if (scope_analyzer_) {
    scope_analyzer_->ResizeResources(request);
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

  scope_analyzer_->ResizeResources(request);
  if (!deferred && scope_frame) {
    scope_analyzer_->SubmitFrame(scope_frame, request);
  }
}

void FinalDisplayFrameTapSink::SetScopeAnalysisDeferred(bool deferred) {
  std::lock_guard<std::mutex> lock(mutex_);
  scope_analysis_deferred_ = deferred;
}

void FinalDisplayFrameTapSink::SetFrameIdentity(uint64_t image_identity,
                                                uint64_t image_generation) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (image_identity_ != image_identity || image_generation_ != image_generation) {
    current_frame_          = {};
    scope_frame_            = {};
    pending_metadata_       = {};
    pending_metadata_valid_ = false;
  }
  image_identity_   = image_identity;
  image_generation_ = image_generation;
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

void FinalDisplayFrameTapSink::NotifyFrameReady() {
  if (auto* sink = downstream_sink()) {
    sink->NotifyFrameReady();
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
    if (pending_metadata_valid_) {
      if (pending_metadata_.image_identity != 0 || pending_metadata_.image_generation != 0) {
        stamped_frame.image_identity   = pending_metadata_.image_identity;
        stamped_frame.image_generation = pending_metadata_.image_generation;
      }
      stamped_frame.display_generation = pending_metadata_.preview_generation;
      scope_update_allowed             = pending_metadata_.scope_update_allowed;
      pending_metadata_valid_          = false;
    }
    if (stamped_frame.image_identity == 0 && image_identity_ != 0) {
      stamped_frame.image_identity = image_identity_;
    }
    if (stamped_frame.image_generation == 0 && image_generation_ != 0) {
      stamped_frame.image_generation = image_generation_;
    }
    if (stamped_frame.frame_id == 0) {
      stamped_frame.frame_id = stamped_frame.display_generation != 0
                                   ? stamped_frame.display_generation
                                   : next_frame_id_++;
    }
    current_frame_ = stamped_frame;
    if (scope_update_allowed && stamped_frame) {
      scope_frame_ = stamped_frame;
    }
    request  = scope_request_;
    active   = scope_active_;
    deferred = scope_analysis_deferred_;
  }

  if (scope_analyzer_ && active && !deferred && scope_update_allowed && stamped_frame) {
    scope_analyzer_->SubmitFrame(stamped_frame, request);
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

void FinalDisplayFrameTapSink::SetNextFramePresentationMode(FramePresentationMode mode) {
  if (auto* sink = downstream_sink()) {
    sink->SetNextFramePresentationMode(mode);
  }
}

void FinalDisplayFrameTapSink::SetNextFramePreviewMetadata(const FramePreviewMetadata& metadata) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    pending_metadata_       = metadata;
    pending_metadata_valid_ = true;
  }
  if (auto* sink = downstream_sink()) {
    sink->SetNextFramePreviewMetadata(metadata);
  }
}

auto FinalDisplayFrameTapSink::GetViewerSurface() -> IEditViewerSurface* {
  if (auto* sink = downstream_sink()) {
    return sink->GetViewerSurface();
  }
  return nullptr;
}

auto FinalDisplayFrameTapSink::GetViewerSurface() const -> const IEditViewerSurface* {
  if (auto* sink = downstream_sink()) {
    return sink->GetViewerSurface();
  }
  return nullptr;
}

}  // namespace alcedo
