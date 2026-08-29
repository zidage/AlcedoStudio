//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>

#include "edit/operators/op_base.hpp"
#include "edit/pipeline/pipeline_accelerator.hpp"
#include "edit/pipeline/pipeline_stage.hpp"
#include "image/image_buffer.hpp"
#include "pipeline.hpp"
#include "pipeline_stage.hpp"
#include "type/type.hpp"
#include "ui/edit_viewer/frame_sink.hpp"

#if defined(HAVE_CUDA) || defined(HAVE_METAL) || defined(HAVE_OPENCL)
#include "edit/graph/pipeline_document.hpp"
#endif

namespace alcedo {
class PipelineDocument;
#if defined(HAVE_CUDA) || defined(HAVE_METAL) || defined(HAVE_OPENCL)
template <class Backend>
class Renderer;
#endif
#ifdef HAVE_CUDA
class CudaBackend;
using CudaRenderer        = Renderer<CudaBackend>;
using CudaProductRenderer = CudaRenderer;
#endif
#ifdef HAVE_METAL
class MetalBackend;
using MetalRenderer        = Renderer<MetalBackend>;
using MetalProductRenderer = MetalRenderer;
#endif
#ifdef HAVE_OPENCL
class OpenClBackend;
using OpenClRenderer        = Renderer<OpenClBackend>;
using OpenClProductRenderer = OpenClRenderer;
#endif
class CPUPipelineExecutor : public PipelineExecutor {
 private:
  sl_element_id_t                                                             bound_file_id_ = 0;
  bool                                                                        enable_cache_  = true;
  std::array<PipelineStage, static_cast<int>(PipelineStageName::Stage_Count)> stages_;

  // Sole ownership of the live pipeline for one frame of work: whoever holds
  // this lock may configure, Apply (including present slot wait), or rebuild
  // stages. Render holds it for the whole task; history waits for it. Do not
  // introduce a second occupancy counter — that is the same ownership question.
  std::mutex                                                                  render_lock_;

  OperatorParams                                                              global_params_;
  std::optional<RawRuntimeColorContext> injected_raw_color_context_;

  bool                                  is_thumbnail_     = false;

  bool                                  force_cpu_output_ = false;
  DecodeRes                             decode_res_       = DecodeRes::FULL;
  std::function<bool()>                 cancel_requested_;
  AcceleratorBackendPreference        accelerator_preference_ = AcceleratorBackendPreference::Auto;
  GpuBackendKind                      resolved_accelerator_backend_ = GpuBackendKind::None;

  nlohmann::json                      render_params_                = {};
  std::optional<ViewportRenderRegion> render_request_viewport_      = std::nullopt;

  static constexpr PipelineBackend    backend_                      = PipelineBackend::CPU;

  std::vector<PipelineStage*>         exec_stages_;
  std::unique_ptr<PipelineStage>      merged_stages_;
  IFrameSink*                         frame_sink_ = nullptr;
  FrameCompletionSubmission           bound_frame_submission_{};
#if defined(HAVE_CUDA) || defined(HAVE_METAL) || defined(HAVE_OPENCL)
  std::shared_ptr<PipelineDocument> pipeline_document_;
  nlohmann::json                    gpu_dag_legacy_snapshot_;
  bool                              mirror_legacy_stage_adapter_ = false;
#endif
#ifdef HAVE_CUDA
  std::shared_ptr<CudaRenderer> cuda_product_renderer_;
#endif
#ifdef HAVE_METAL
  std::shared_ptr<MetalRenderer> metal_product_renderer_;
#endif
#ifdef HAVE_OPENCL
  std::shared_ptr<OpenClRenderer> opencl_product_renderer_;
#endif

  void ResetStages();

  void ResetExecutionStagesCache();

  void SetTemplateParams();
  void ResolveAcceleratorBackend();
  void ApplyAcceleratorBackendToStages();
  void ApplyRuntimeRawDecodeBackend();
  void SyncRawDecodeRuntimeControls();

 public:
  CPUPipelineExecutor();
  CPUPipelineExecutor(bool enable_cache);

  void               SetBoundFile(sl_element_id_t file_id) override { bound_file_id_ = file_id; }
  auto               GetBoundFile() const -> sl_element_id_t override { return bound_file_id_; }

  void               SetEnableCache(bool enable_cache);
  auto               GetBackend() -> PipelineBackend override;

  void               SetForceCPUOutput(bool force) override { force_cpu_output_ = force; }
  void               SetCancelRequested(std::function<bool()> cancel_requested);

  void               SetAcceleratorBackendPreference(AcceleratorBackendPreference preference);
  [[nodiscard]] auto GetAcceleratorBackendPreference() const -> AcceleratorBackendPreference {
    return accelerator_preference_;
  }
  [[nodiscard]] auto GetResolvedAcceleratorBackend() const -> GpuBackendKind {
    return resolved_accelerator_backend_;
  }

  auto GetRenderLock() -> std::mutex& { return render_lock_; }

  auto GetStage(PipelineStageName stage) -> PipelineStage& override;
  auto Apply(std::shared_ptr<ImageBuffer> input) -> std::shared_ptr<ImageBuffer> override;

  /** @brief Select the format-version-2 document used by the GPU DAG product path. */
  void SetPipelineDocument(std::shared_ptr<PipelineDocument> document,
                           bool                              mirror_legacy_stage_adapter = false);
  [[nodiscard]] auto HasGpuDagDocument() const -> bool;
  [[nodiscard]] auto GpuDagDocument() const -> std::shared_ptr<PipelineDocument>;
  [[nodiscard]] auto MirrorsLegacyStageAdapter() const -> bool;

  void SetPreviewMode(bool is_preview);

  void DetachFrameSink();

  // Re-attach a frame sink without rebuilding execution stages.
  // Must be called under render_lock_.
  void AttachFrameSink(IFrameSink* frame_sink);

  void SetExecutionStages();
  void SetExecutionStages(IFrameSink* frame_sink);
  void ResetExecutionStages();

  // Returns the raw frame sink pointer. Caller must hold render_lock_.
  auto GetFrameSink() const -> IFrameSink* { return frame_sink_; }

  auto GetViewportRenderRegion() const -> std::optional<ViewportRenderRegion>;
  /// Freeze the viewport geometry carried by the current render request.
  /// A null value explicitly means full-frame rendering; Apply never re-reads live UI state.
  void SetRenderRequestViewport(std::optional<ViewportRenderRegion> viewport) {
    render_request_viewport_ = std::move(viewport);
  }
  void BindFrameSubmission(const FramePreviewMetadata& metadata, FramePresentationMode mode);
  [[nodiscard]] auto BoundFrameSubmission() const -> FrameCompletionSubmission;

  auto               GetGlobalParams() -> OperatorParams& override { return global_params_; }

  /**
   * @brief Serialize the pipeline parameters to JSON
   *
   * @return nlohmann::json
   */
  auto               ExportPipelineParams() const -> nlohmann::json override;
  /**
   * @brief Set the pipeline parameters from JSON. It will reset all stages and operators, as well
   * as cache. After importing, you need to call SetExecutionStages() to rebuild the execution
   * stages.
   *
   * @param j
   */
  void               ImportPipelineParams(const nlohmann::json& j) override;

  void SetRenderRegion(int x, int y, float scale_factor_x, float scale_factor_y = -1.0f,
                       int reference_width = 0, int reference_height = 0) override;
  void SetRenderRes(bool full_res, int max_side_length = 2048) override;
  void SetResizeDownsampleAlgorithm(ResizeDownsampleAlgorithm algorithm) override;
  void SetDecodeRes(DecodeRes res);

  /// Snapshot of one-shot render parameters that must not leak across Apply calls.
  struct OneShotRenderParamsSnapshot {
    DecodeRes                           decode_res_              = DecodeRes::FULL;
    nlohmann::json                      render_params_           = {};
    bool                                force_cpu_output_        = false;
    bool                                enable_cache_            = true;
    std::optional<ViewportRenderRegion> render_request_viewport_ = std::nullopt;
  };

  [[nodiscard]] auto CaptureOneShotRenderParams() const -> OneShotRenderParamsSnapshot;
  void               RestoreOneShotRenderParams(const OneShotRenderParamsSnapshot& snapshot);

  void               RegisterAllOperators();
  void               ResetToCleanBaselineAdjustments();

  void               InitDefaultPipeline();

  /**
   * @brief Install image-local raw metadata into the pipeline (transition path).
   *        Prefer writing inherent RAW params into RawDecodeOp at import so that
   *        reload and render no longer require a per-frame inject.
   */
  void               InjectRawMetadata(const RawRuntimeColorContext& ctx);

  /**
   * @brief Clear all intermediate image buffers from all stages.
   *        Call this after pipeline execution when you want to release memory
   *        while keeping the pipeline configuration intact.
   */
  void               ClearAllIntermediateBuffers();

  /**
   * @brief Release transient merged-stage preview scratch buffers while keeping
   *        the compiled GPU pipeline and LUT state intact.
   *        Use this when a full-resolution preview/export frame returns to the
   *        FAST_PREVIEW baseline and the large scratch high-water mark should
   *        not stay pinned in VRAM.
   */
  void               ReleasePreviewGpuScratch();

  /**
   * @brief Release persistent GPU allocations held by execution stages.
   *        Useful for batch export to avoid holding large VRAM allocations
   *        across many cached pipelines.
   */
  void               ReleaseAllGPUResources();

  [[nodiscard]] auto DebugGetMergedStageScratchBytes() const -> size_t;

  /// Stable identity of the merged GPU stage. Changes only when the stage is
  /// (re)created — which also recreates the LLF highlight/shadow reference
  /// cache. Used by tests to assert re-attaching a frame sink does not wipe
  /// the cross-frame LLF mask cache (the 42ed19b CanReuseReferenceForRoi path).
  [[nodiscard]] auto DebugGetMergedStageIdentity() const -> std::uintptr_t;

#ifdef HAVE_CUDA
  [[nodiscard]] auto DebugCudaRenderer() -> CudaRenderer* { return cuda_product_renderer_.get(); }
  [[nodiscard]] auto DebugCudaProductRenderer() -> CudaRenderer* { return DebugCudaRenderer(); }
#endif
#ifdef HAVE_METAL
  [[nodiscard]] auto DebugMetalRenderer() -> MetalRenderer* { return metal_product_renderer_.get(); }
#endif
#ifdef HAVE_OPENCL
  [[nodiscard]] auto DebugOpenClRenderer() -> OpenClRenderer* {
    return opencl_product_renderer_.get();
  }
#endif
};
};  // namespace alcedo
