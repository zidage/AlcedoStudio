//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <memory>
#include <mutex>

#include "edit/scope/scope_analyzer.hpp"
#include "ui/edit_viewer/frame_sink.hpp"

namespace alcedo {

class FinalDisplayFrameTapSink final : public IFrameSink, public IFinalDisplayFrameProvider {
 public:
  FinalDisplayFrameTapSink(IFrameSink*                     downstream_sink,
                           std::shared_ptr<IScopeAnalyzer> scope_analyzer);

  void               SetDownstreamSink(IFrameSink* downstream_sink);
  void               SetScopeRequest(const ScopeRequest& request);
  void               SetScopeActive(bool active);
  void               SetScopeAnalysisDeferred(bool deferred);
  void               SetFrameIdentity(uint64_t image_identity, uint64_t image_generation);
  auto               GetScopeRequest() const -> ScopeRequest;
  auto               SubmitCurrentDisplayFrameToScope() -> bool;

  [[nodiscard]] auto downstream_sink() const -> IFrameSink*;

  auto               GetCurrentDisplayFrameView() const -> FinalDisplayFrameView override;
  auto               GetCurrentScopeFrameView() const -> FinalDisplayFrameView;

  void               EnsureSize(int width, int height) override;
  auto MapResourceForWrite(FrameMemoryDomain preferred_domain = FrameMemoryDomain::CudaDevice)
      -> FrameWriteMapping override;
  void UnmapResource() override;
  void NotifyFrameReady(const FrameCompletionSubmission& submission) override;
  void BindFrameSubmission(const FrameCompletionSubmission& submission) override;
  void SubmitHostFrame(const ViewerFrame& frame) override;
#ifdef HAVE_METAL
  void SubmitMetalFrame(const ViewerMetalFrame& frame) override;
#endif
  void SubmitFinalDisplayFrame(const FinalDisplayFrameView& frame) override;
  auto GetWidth() const -> int override;
  auto GetHeight() const -> int override;
  auto GetViewportRenderRegion() const -> std::optional<ViewportRenderRegion> override;
  auto GetViewerSurface() -> IEditViewerSurface* override;
  auto GetViewerSurface() const -> const IEditViewerSurface* override;

 private:
  IFrameSink*                     downstream_sink_ = nullptr;
  std::shared_ptr<IScopeAnalyzer> scope_analyzer_  = {};
  mutable std::mutex              mutex_{};
  FinalDisplayFrameView           current_frame_{};
  FinalDisplayFrameView           scope_frame_{};
  ScopeRequest                    scope_request_{};
  FrameCompletionSubmission       bound_submission_{};
  bool                            bound_submission_valid_ = false;
  bool                            scope_active_            = false;
  bool                            scope_analysis_deferred_ = false;
  uint64_t                        image_identity_          = 0;
  uint64_t                        image_generation_        = 0;
  uint64_t                        next_frame_id_           = 1;
};

}  // namespace alcedo
