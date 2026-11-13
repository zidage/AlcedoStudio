//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>

#include "edit/graph/graph_ids.hpp"
#include "edit/runtime/adjustment_runtime.hpp"
#include "edit/runtime/frame_scene_binding.hpp"

namespace alcedo {

/**
 * @brief One separable neighborhood launch: packed kernel parameters plus command identity.
 *
 * CUDA, OpenCL, and Metal start kernels from @ref params.
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
  using LutBinding = typename Ops::LutBinding;

  /**
   * @brief Start the horizontal kernel, then the vertical apply kernel, then release scratch.
   *
   * Horizontal writes independent scratch. Vertical reads @p src same-pixel and scratch
   * neighborhood, then writes @p dst. @p dst may alias @p src when both are the same work
   * member. When @p mix is not 1 or @p mask_id is set, vertical apply also reads @p original
   * and fuses Mix. Scratch is acquired before source lookup so a TexturePool growth cannot
   * invalidate pool textures.
   */
  static void Execute(Device& device, const FrameSceneBinding& src, const FrameSceneBinding& dst,
                      const FrameSceneBinding& original, const LutBinding& lut,
                      const NeighborWork& work, float mix, const GraphValueId* mask_id,
                      std::uint32_t width, std::uint32_t height) {
    auto scratch = Ops::AcquireHorizontalScratch(device, width, height);
    Ops::DispatchHorizontal(device, src, scratch, work, width, height);
    Ops::DispatchVerticalApply(device, src, scratch, dst, original, lut, work, mix, mask_id, width,
                               height);
  }
};

}  // namespace alcedo
