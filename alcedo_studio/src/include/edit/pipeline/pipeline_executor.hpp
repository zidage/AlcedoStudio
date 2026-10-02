//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <memory>
#include <mutex>
#include <optional>

#include "edit/graph/pipeline_graph_snapshot.hpp"
#include "edit/pipeline/pipeline_accelerator.hpp"
#include "edit/pipeline/rendered_pipeline_image.hpp"
#include "edit/runtime/executor_role.hpp"
#include "edit/runtime/lut_resource_resolver.hpp"
#include "edit/runtime/pipeline_apply_request.hpp"
#include "image/image_buffer.hpp"
#include "ui/edit_viewer/frame_sink.hpp"

namespace alcedo {
#if defined(HAVE_CUDA) || defined(HAVE_METAL) || defined(HAVE_OPENCL)
// Renderer<Backend> is defined in backend headers that pull in CUDA / OpenCL / Metal device
// types; this header is included by app and UI code that must not see them. The members below
// hold renderers only through shared_ptr, and pipeline_executor.cpp includes every definition.
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
 * @brief Renders immutable pipeline graph snapshots on the selected GPU backend.
 *
 * Holds no document. Every Apply receives the snapshot to render; the executor keeps only GPU
 * state: the render lock, the accelerator selection, the attached frame sink, and one lazily
 * created renderer per compiled backend and served role.
 *
 * Roles (@ref ExecutorRole): an executor constructed with one role serves only that role, so it
 * owns exactly one renderer per backend. Owners: the editor render port owns one Interactive
 * executor; ThumbnailService and ExportService own Batch executors.
 */
class PipelineExecutor {
 private:
  // Sole ownership of the executor for one frame of work: whoever holds this lock may configure,
  // Apply (including present slot wait), or release the binding. The scheduler holds it for the
  // whole task. An owner whose every use runs on one worker thread has exclusive access without
  // it.
  std::mutex                   render_lock_;

  bool                         serves_interactive_           = false;
  bool                         serves_batch_                 = false;

  AcceleratorBackendPreference accelerator_preference_       = AcceleratorBackendPreference::Auto;
  GpuBackendKind               resolved_accelerator_backend_ = GpuBackendKind::None;

  IFrameSink*                  frame_sink_                   = nullptr;

  /// Resolver every renderer of this executor uses for Color Grade LUT references.
  std::shared_ptr<const LutResourceResolver> lut_resources_;

  /// Renderers of one backend, one per served role; each is created by its first Apply.
  template <class RendererType>
  struct RoleRenderers {
    std::shared_ptr<RendererType> interactive;
    std::shared_ptr<RendererType> batch;
  };
#ifdef HAVE_CUDA
  RoleRenderers<CudaRenderer> cuda_renderers_;
#endif
#ifdef HAVE_METAL
  RoleRenderers<MetalRenderer> metal_renderers_;
#endif
#ifdef HAVE_OPENCL
  RoleRenderers<OpenClRenderer> opencl_renderers_;
#endif

  /// Shared body of Apply and ApplyImage: the renderer of the resolved backend and request role.
  auto Render(const PipelineGraphSnapshot& snapshot, const std::shared_ptr<ImageBuffer>& input,
              const PipelineApplyRequest& request) -> RenderedPipelineImage;

 public:
  /**
   * @brief Executor that serves only @p role. Resolves the Auto accelerator preference.
   * @param lut_resources Resolver for Color Grade LUT references, handed to every renderer this
   *        executor creates. The application passes the LUT library; null selects the
   *        file-path-only DefaultLutResourceResolver.
   */
  explicit PipelineExecutor(ExecutorRole                               role,
                            std::shared_ptr<const LutResourceResolver> lut_resources = nullptr);

  [[nodiscard]] auto Serves(ExecutorRole role) const -> bool {
    return role == ExecutorRole::Interactive ? serves_interactive_ : serves_batch_;
  }

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
   * @brief Render @p snapshot with an immutable per-task request.
   *
   * Runs on the renderer of the resolved backend and of `request.role`. The interactive renderer
   * releases every resource of its previous binding when the snapshot's lineage or element
   * differs from it. Neither the snapshot nor the request is stored on the executor.
   *
   * @pre Caller holds GetRenderLock(); camera/profile data is already bound on the document.
   * @param snapshot Graph to render; only read. The caller keeps it alive for the call.
   * @param request Carries the role and the cancel callback.
   * @throws std::invalid_argument when this executor does not serve `request.role`.
   * @throws std::runtime_error for missing backend, decode, GPU, or presentation failure.
   *         Batch result resources are released after GPU last-use or on the failure path.
   */
  auto Apply(const PipelineGraphSnapshot& snapshot, std::shared_ptr<ImageBuffer> input,
             const PipelineApplyRequest& request) -> std::shared_ptr<ImageBuffer>;

  /**
   * @brief Render @p snapshot to host pixels and return them with the exact geometry and display
   *        encoding of the executed plan.
   *
   * Same renderer, binding rule, and failure behavior as @ref Apply; @ref Apply returns the
   * `pixels` member of the same render. The request's own sink is used: a null sink presents
   * nothing, whatever sink is attached to the executor.
   *
   * @pre Caller holds GetRenderLock() or has exclusive access; `request.require_host_output`.
   * @throws std::invalid_argument when this executor does not serve `request.role` or host
   *         output is not requested.
   * @throws std::runtime_error for missing backend, decode, GPU, presentation, or download
   *         failure. Nothing is returned for a failed render.
   */
  auto ApplyImage(const PipelineGraphSnapshot& snapshot, std::shared_ptr<ImageBuffer> input,
                  const PipelineApplyRequest& request) -> RenderedPipelineImage;

  /// Attach the editor frame sink that later requests read. Caller holds render_lock_ or has
  /// exclusive access.
  void AttachFrameSink(IFrameSink* frame_sink) { frame_sink_ = frame_sink; }
  /// Clear the attached frame sink so no later request presents to it. Caller holds render_lock_
  /// or has exclusive access.
  void DetachFrameSink() { frame_sink_ = nullptr; }

  // Returns the raw frame sink pointer. Caller holds render_lock_ or has exclusive access.
  auto GetFrameSink() const -> IFrameSink* { return frame_sink_; }

  /// Viewport region of the attached frame sink, or nullopt when no sink is attached.
  auto GetViewportRenderRegion() const -> std::optional<ViewportRenderRegion>;

  /**
   * @brief Release every GPU and host resource bound to the last rendered image, in every
   *        created renderer. Devices and the frame sink are kept.
   * @pre Caller holds GetRenderLock() or has exclusive access.
   */
  void               ReleaseBinding();

#ifdef HAVE_CUDA
  [[nodiscard]] auto DebugCudaRenderer() -> CudaRenderer* {
    return cuda_renderers_.interactive.get();
  }
  [[nodiscard]] auto DebugCudaProductRenderer() -> CudaRenderer* { return DebugCudaRenderer(); }
  [[nodiscard]] auto DebugCudaBatchRenderer() -> CudaRenderer* {
    return cuda_renderers_.batch.get();
  }
#endif
#ifdef HAVE_METAL
  [[nodiscard]] auto DebugMetalRenderer() -> MetalRenderer* {
    return metal_renderers_.interactive.get();
  }
  [[nodiscard]] auto DebugMetalBatchRenderer() -> MetalRenderer* {
    return metal_renderers_.batch.get();
  }
#endif
#ifdef HAVE_OPENCL
  [[nodiscard]] auto DebugOpenClRenderer() -> OpenClRenderer* {
    return opencl_renderers_.interactive.get();
  }
  [[nodiscard]] auto DebugOpenClBatchRenderer() -> OpenClRenderer* {
    return opencl_renderers_.batch.get();
  }
#endif
};
};  // namespace alcedo
