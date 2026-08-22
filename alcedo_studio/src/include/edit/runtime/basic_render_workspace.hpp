//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <stdexcept>

#include "edit/runtime/graph_image_cache.hpp"
#include "edit/runtime/mask_texture_cache.hpp"
#include "edit/runtime/node_result_cache.hpp"
#include "edit/runtime/parameter_arena.hpp"
#include "edit/runtime/texture_pool.hpp"
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
  [[nodiscard]] auto MaskTextures() -> MaskTextureCache<Backend>& { return mask_textures_; }
  [[nodiscard]] auto Values() -> NodeResultCache<Backend>& { return values_; }
  [[nodiscard]] auto Images() -> GraphImageCache<Backend>& { return images_; }
  [[nodiscard]] auto Images() const -> const GraphImageCache<Backend>& { return images_; }

  /**
   * @brief Wait the previous submission, rewind transients, start a new submission id.
   * @throws std::runtime_error if called re-entrantly.
   */
  void               BeginRender(CommandContext& command_context) {
    if (rendering_) {
      throw std::runtime_error("BasicRenderWorkspace::BeginRender: already rendering");
    }
    backend_.Wait(command_context);
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
