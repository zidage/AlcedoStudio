//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <memory>
#include <mutex>
#include <optional>

#include "edit/pipeline/pipeline_accelerator.hpp"
#include "edit/runtime/pipeline_apply_request.hpp"
#include "image/image_buffer.hpp"
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

/**
 * @brief Binds one PipelineDocument to the GPU DAG renderer of the selected backend.
 *
 * The executor owns the render lock, the accelerator selection, the attached frame sink, the
 * bound file id, and one lazily created renderer per compiled backend. It owns no parameter
 * values: every render reads the bound document.
 */
class PipelineExecutor {
 private:
  sl_element_id_t              bound_file_id_ = 0;

  // Sole ownership of the live pipeline for one frame of work: whoever holds
  // this lock may configure, Apply (including present slot wait), or rebind the
  // document. Render holds it for the whole task; history waits for it. Do not
  // introduce a second occupancy counter — that is the same ownership question.
  std::mutex                   render_lock_;

  AcceleratorBackendPreference accelerator_preference_       = AcceleratorBackendPreference::Auto;
  GpuBackendKind               resolved_accelerator_backend_ = GpuBackendKind::None;

  IFrameSink*                  frame_sink_                   = nullptr;
#if defined(HAVE_CUDA) || defined(HAVE_METAL) || defined(HAVE_OPENCL)
  std::shared_ptr<PipelineDocument> pipeline_document_;
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

 public:
  /// Resolve the Auto accelerator preference. No document is bound yet.
  PipelineExecutor();

  void               SetBoundFile(sl_element_id_t file_id) { bound_file_id_ = file_id; }
  [[nodiscard]] auto GetBoundFile() const -> sl_element_id_t { return bound_file_id_; }

  /**
   * @brief Select the accelerator for later renders.
   * @pre Caller holds GetRenderLock() or has exclusive access.
   * Existing renderers and the attached frame sink are kept. A render with a backend that has no
   * compiled renderer throws; no other backend runs in its place.
   */
  void               SetAcceleratorBackendPreference(AcceleratorBackendPreference preference);
  [[nodiscard]] auto GetAcceleratorBackendPreference() const -> AcceleratorBackendPreference {
    return accelerator_preference_;
  }
  [[nodiscard]] auto GetResolvedAcceleratorBackend() const -> GpuBackendKind {
    return resolved_accelerator_backend_;
  }

  auto GetRenderLock() -> std::mutex& { return render_lock_; }

  /**
   * @brief Render using an immutable per-task request. Does not write decode, cache, ROI,
   *        or host-output onto executor members.
   * @pre Caller holds GetRenderLock(); camera/profile data is already bound on the document.
   * @param request Carries the cancel callback; Apply does not store it on the executor.
   * @throws std::runtime_error for missing document/backend, decode, GPU, or presentation failure.
   *         Failures do not switch executor cache/decode mode. Bypass ExactRelease scratch is
   *         released after GPU last-use or on the failure path.
   */
  auto Apply(std::shared_ptr<ImageBuffer> input, const PipelineApplyRequest& request)
      -> std::shared_ptr<ImageBuffer>;

  /**
   * @brief Select the document used by the GPU DAG product path.
   *
   * @param document Graph owned by the caller; retains shared ownership without changing values.
   * @pre Caller holds GetRenderLock() or has exclusive access before publication.
   * @throws std::invalid_argument when document is null; retains the prior binding.
   */
  void               SetPipelineDocument(std::shared_ptr<PipelineDocument> document);
  [[nodiscard]] auto HasGpuDagDocument() const -> bool;
  [[nodiscard]] auto GpuDagDocument() const -> std::shared_ptr<PipelineDocument>;

  /// Attach the editor frame sink that later requests read. Caller must hold render_lock_.
  void AttachFrameSink(IFrameSink* frame_sink) { frame_sink_ = frame_sink; }
  /// Clear the attached frame sink so no later request presents to it. Caller holds render_lock_.
  void DetachFrameSink() { frame_sink_ = nullptr; }

  // Returns the raw frame sink pointer. Caller must hold render_lock_.
  auto GetFrameSink() const -> IFrameSink* { return frame_sink_; }

  /// Viewport region of the attached frame sink, or nullopt when no sink is attached.
  auto GetViewportRenderRegion() const -> std::optional<ViewportRenderRegion>;

  /**
   * @brief Release the session caches of every created renderer.
   * @pre Caller holds GetRenderLock(). The document binding and the frame sink are kept.
   */
  void               ClearAllIntermediateBuffers();

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
