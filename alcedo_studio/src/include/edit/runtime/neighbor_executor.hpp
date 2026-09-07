//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>

#include "edit/graph/graph_ids.hpp"
#include "edit/runtime/adjustment_runtime.hpp"

namespace alcedo {

/**
 * @brief One separable neighborhood launch: packed kernel parameters plus command identity.
 *
 * CUDA and OpenCL start kernels from @ref params. Metal neighborhood may ignore @ref params
 * and start its existing kernel from @ref owner / @ref command_index.
 */
struct NeighborWork {
  GradeNeighborParams params{};
  NodeId              owner;
  std::uint32_t       fused_offset  = 0;
  std::uint32_t       command_index = 0;
};

/**
 * @brief Shared horizontal-scratch, blur, apply, and scratch-release order.
 *
 * @tparam Ops Backend horizontal scratch, kernel starts, and native error reporting.
 *         Shared code owns the two-start sequence and scratch lifetime.
 */
template <class Ops>
class NeighborExecutor {
 public:
  using Device     = typename Ops::Device;
  using Texture    = typename Ops::Texture;
  using LutBinding = typename Ops::LutBinding;

  /**
   * @brief Start the horizontal kernel, then the vertical apply kernel, then release scratch.
   *
   * Scratch is acquired before source/destination lookup so a TexturePool growth cannot
   * invalidate those textures. @p lut is the Grade LUT or the backend dummy LUT; CUDA and
   * OpenCL ignore it.
   */
  static void Execute(Device& device, const GraphValueId& src_id, const GraphValueId& dst_id,
                      const LutBinding& lut, const NeighborWork& work, std::uint32_t width,
                      std::uint32_t height) {
    auto  scratch = Ops::AcquireHorizontalScratch(device, width, height);
    auto& blur    = Ops::HorizontalScratchTexture(scratch);
    auto& src     = Ops::SceneTexture(device, src_id);
    auto& dst     = Ops::SceneTexture(device, dst_id);
    Ops::DispatchHorizontal(device, src, blur, work, width, height);
    Ops::DispatchVerticalApply(device, src, blur, dst, lut, work, width, height);
  }
};

}  // namespace alcedo
