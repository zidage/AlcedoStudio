//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#ifdef HAVE_VULKAN

#include <cstdint>

#include "edit/graph/graph_ids.hpp"
#include "edit/runtime/adjustment_runtime.hpp"
#include "edit/runtime/frame_scene_binding.hpp"
#include "edit/runtime/neighbor_executor.hpp"
#include "edit/runtime/texture_pool.hpp"
#include "edit/runtime/vulkan/vulkan_render_device.hpp"

namespace alcedo {

/**
 * @brief Checks that @p params is a behavior of the neighborhood kernels: Clarity, Sharpen, or
 * Halation. Film Grain has its own pass (the DRT finish, phase VK14).
 * @throws std::invalid_argument "<caller>: ..." for Film Grain or another behavior.
 */
void RequireVulkanNeighborhoodKernelBehavior(const GradeNeighborParams& params, const char* caller);

/**
 * @brief The node value that holds the packed @ref GradeNeighborParams of @p work: one buffer for
 * each owner and command index, so a stable render creates no buffer.
 */
[[nodiscard]] auto VulkanNeighborParamsValueId(const NeighborWork& work) -> GraphValueId;

/**
 * @brief The Vulkan operations of the shared `NeighborExecutor` (phase VK9): the form of the
 * neighborhood operations of `OpenClGradeOps` and `OpenClDrtOps`.
 *
 * `VulkanGradeOps` has them for a Grade neighborhood stage; the DRT finish (phase VK12) has them
 * for Clarity, Sharpen, and Halation.
 *
 * - `AcquireHorizontalScratch`: an RGBA32F image of the texture pool of the workspace.
 * - `DispatchHorizontal`: uploads `work.params` into the node value of
 *   @ref VulkanNeighborParamsValueId, then records `primary_grade_neighbor_blur_h_scene_rgba32f`
 *   from @p src into the scratch image.
 * - `DispatchVerticalApply`: records `primary_grade_neighbor_apply_v_scene_rgba32f`: the vertical
 *   blur of the scratch image, the adjustment on @p src, and, when @p mix is not 1 or @p mask_id is
 *   set, the mix with @p original times the R8 mask. @p dst is a scene-work member or an RGBA32F
 *   image (the display output of the DRT finish); it may be @p src.
 *
 * Each dispatch records into the open recording and submits nothing.
 *
 * @throws std::invalid_argument For Film Grain, a vertical radius above
 *         `primary_grade_neighbor_kernels::kMaxRadius`, or more Gaussian taps than the radius
 *         gives, before a command.
 * @throws std::runtime_error For scene images of different sizes, a scratch image that is not
 *         RGBA32F of the size, a missing or wrong mask, and a vertical dispatch without the
 *         horizontal one; the caller cancels the render.
 */
struct VulkanNeighborOps {
  using Device            = VulkanRenderDevice;
  using Backend           = VulkanBackend;
  using HorizontalScratch = ResourceLease<VulkanBackend>;
  using LutBinding        = VulkanLutBinding;

  static auto AcquireHorizontalScratch(VulkanRenderDevice& device, std::uint32_t width,
                                       std::uint32_t height) -> HorizontalScratch;

  static void DispatchHorizontal(VulkanRenderDevice& device, const FrameSceneBinding& src,
                                 HorizontalScratch& scratch, const NeighborWork& work,
                                 std::uint32_t width, std::uint32_t height);

  static void DispatchVerticalApply(VulkanRenderDevice& device, const FrameSceneBinding& src,
                                    HorizontalScratch& scratch, const FrameSceneBinding& dst,
                                    const FrameSceneBinding& original, const LutBinding& lut,
                                    const NeighborWork& work, float mix,
                                    const GraphValueId* mask_id, std::uint32_t width,
                                    std::uint32_t height);
};

}  // namespace alcedo

#endif  // HAVE_VULKAN
