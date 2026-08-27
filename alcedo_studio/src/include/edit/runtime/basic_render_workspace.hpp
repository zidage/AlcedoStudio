//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdio>
#include <stdexcept>

#include "edit/runtime/graph_image_cache.hpp"
#include "edit/runtime/mask_texture_cache.hpp"
#include "edit/runtime/node_result_cache.hpp"
#include "edit/runtime/parameter_arena.hpp"
#include "edit/runtime/texture_pool.hpp"
#include "gpu/gpu_pool_trace.hpp"
#include "gpu/transient_buffer_arena.hpp"

namespace alcedo {

/**
 * @brief Per-device GPU workspace: parameters, transients, textures, node values.
 *
 * Owned by a render device, not by PipelineDocument or ExecutionPlan. Grows
 * only when no GPU submission is in flight. One in-flight frame.
 *
 * @tparam Backend Resource factory. CUDA is first; other GPU APIs follow.
 */
template <class Backend>
class BasicRenderWorkspace {
 public:
  using Buffer         = typename Backend::Buffer;
  using Texture2D      = typename Backend::Texture2D;
  using CommandContext = typename Backend::CommandContext;

  BasicRenderWorkspace()
      : parameters_(backend_),
        transients_(backend_),
        textures_(backend_),
        mask_textures_(backend_) {}

  BasicRenderWorkspace(const BasicRenderWorkspace&)                                  = delete;
  auto               operator=(const BasicRenderWorkspace&) -> BasicRenderWorkspace& = delete;

  [[nodiscard]] auto Device() -> Backend& { return backend_; }
  [[nodiscard]] auto Device() const -> const Backend& { return backend_; }

  [[nodiscard]] auto Parameters() -> ParameterArena<Backend>& { return parameters_; }
  [[nodiscard]] auto TransientBuffers() -> TransientBufferArena<Backend>& { return transients_; }
  [[nodiscard]] auto Textures() -> TexturePool<Backend>& { return textures_; }
  [[nodiscard]] auto Textures() const -> const TexturePool<Backend>& { return textures_; }
  [[nodiscard]] auto MaskTextures() -> MaskTextureCache<Backend>& { return mask_textures_; }
  [[nodiscard]] auto Values() -> NodeResultCache<Backend>& { return values_; }
  [[nodiscard]] auto Images() -> GraphImageCache<Backend>& { return images_; }
  [[nodiscard]] auto Images() const -> const GraphImageCache<Backend>& { return images_; }

  /**
   * @brief Allocate an unpublished write texture for @p id. See GraphImageCache.
   */
  auto AcquireImageForWrite(const GraphValueId& id, const TextureRequest& request)
      -> ResourceLease<Backend>& {
    auto&      lease = images_.AcquireTextureForWrite(textures_, backend_, id, request);
    const auto bytes = static_cast<std::size_t>(request.width) * request.height *
                       TextureFormatBytesPerPixel(request.format);
    if (ShouldTraceGpuPoolAlloc(bytes)) {
      DumpGpuPools("image-write");
    }
    return lease;
  }

  /** @brief Print pool totals. Per-resource lines require ALCEDO_GPU_POOL_TRACE. */
  void DumpGpuPools(const char* reason) const {
    GpuDeviceMemorySnapshot device_memory{};
    if constexpr (requires(const Backend& backend) { backend.QueryDeviceMemory(); }) {
      device_memory = backend_.QueryDeviceMemory();
    }
    const auto device_used =
        device_memory.valid && device_memory.total_bytes > device_memory.free_bytes
            ? device_memory.total_bytes - device_memory.free_bytes
            : 0;
    std::fprintf(stderr,
                 "[GPU_POOL] %s textures=%.1f/%.1f MiB n=%zu  transient=%.1f/%.1f MiB  "
                 "images=pub%zu/write%zu  values=%.1f n=%zu  masks=%.1f n=%zu",
                 reason == nullptr ? "" : reason, GpuPoolMiB(textures_.UsedBytes()),
                 GpuPoolMiB(textures_.ByteBudget()), textures_.EntryCount(),
                 GpuPoolMiB(transients_.used_bytes()), GpuPoolMiB(transients_.capacity_bytes()),
                 images_.PublishedCount(), images_.UnpublishedCount(),
                 GpuPoolMiB(values_.UsedBytes()), values_.Size(),
                 GpuPoolMiB(mask_textures_.UsedBytes()), mask_textures_.EntryCount());
    if (device_memory.valid) {
      std::fprintf(stderr, "  device used=%.1f free=%.1f total=%.1f MiB", GpuPoolMiB(device_used),
                   GpuPoolMiB(device_memory.free_bytes), GpuPoolMiB(device_memory.total_bytes));
    }
    std::fprintf(stderr, "\n");
    if (GpuPoolTraceVerbose()) {
      textures_.DumpToStderr(reason);
      images_.DumpToStderr(reason, textures_);
      values_.DumpToStderr(reason);
      mask_textures_.DumpToStderr(reason);
    }
  }

  /**
   * @brief Share @p source's current texture as an unpublished write of @p dest.
   *
   * Does not copy pixels and does not add a TexturePool allocation.
   */
  auto AliasImageFrom(const GraphValueId& dest, const GraphValueId& source)
      -> ResourceLease<Backend>& {
    return images_.AliasTextureFrom(textures_, dest, source);
  }

  /**
   * @brief Wait the previous submission, drop leftover unpublished writes, start a new id.
   * @throws std::runtime_error if called re-entrantly.
   */
  void BeginRender(CommandContext& command_context) {
    if (rendering_) {
      throw std::runtime_error("BasicRenderWorkspace::BeginRender: already rendering");
    }
    backend_.Wait(command_context);
    images_.DiscardUnpublished();
    transients_.Reset();
    textures_.BeginFrame();
    mask_textures_.BeginFrame();
    command_context.SetSubmissionId(backend_.NextSubmissionId());
    rendering_ = true;
  }

  /**
   * @brief Record completion of the current command buffer. Does not wait.
   */
  void EndRender(CommandContext& command_context) {
    if (!rendering_) {
      throw std::runtime_error("BasicRenderWorkspace::EndRender: not rendering");
    }
    textures_.MarkSubmitted(command_context.SubmissionId());
    mask_textures_.MarkSubmitted(command_context.SubmissionId());
    backend_.Submit(command_context);
    rendering_ = false;
  }

  [[nodiscard]] auto IsRendering() const -> bool { return rendering_; }

  /**
   * @brief Leave a failed encode without submitting. Unpublished writes are discarded.
   */
  void CancelRender() {
    images_.DiscardUnpublished();
    rendering_ = false;
  }

  /**
   * @brief Drop published GPU results, textures, transients, and parameter slots.
   *
   * @pre Not rendering. Caller WaitIdle first so no texture is still busy.
   */
  void ReleaseSessionResources() {
    if (rendering_) {
      throw std::runtime_error(
          "BasicRenderWorkspace::ReleaseSessionResources: cannot release while rendering");
    }
    images_.Clear();
    values_.Clear();
    mask_textures_.Clear();
    textures_.ReleaseUnleased();
    transients_.ReleaseDeviceMemory();
    parameters_.Clear();
  }

 private:
  Backend                       backend_{};
  ParameterArena<Backend>       parameters_;
  TransientBufferArena<Backend> transients_;
  TexturePool<Backend>          textures_;
  MaskTextureCache<Backend>     mask_textures_;
  NodeResultCache<Backend>      values_{};
  GraphImageCache<Backend>      images_{};
  bool                          rendering_ = false;
};

}  // namespace alcedo
