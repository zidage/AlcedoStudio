//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "edit/geometry/resolved_render_geometry.hpp"
#include "edit/runtime/texture_format.hpp"

namespace alcedo {

/// Largest axis of a coverage display target. Photo frames of the editor stay below it.
inline constexpr std::uint32_t kMaskCoverageDisplayMaximumAxis = 16384;

/**
 * @brief Inputs of one coverage display write (SM8 section 5.2): the processed coverage of one
 *        AI Mask mapped to the grid of the presented photo frame.
 *
 * @p geometry is the ResolveRenderGeometry result of the presented full photo frame; the display
 * target has its render extent. @p invert and @p opacity are those of the Mask (a new Mask uses
 * false and 1). The write uses the raster sampling of committed MaskEvaluate
 * (MakeRasterMaskSamplingPlan, RasterMaskSamplesPerAxis, invert, opacity, R8 quantization), so the
 * same Mask gives the same coverage values in the display and in the photo.
 */
struct MaskCoverageDisplayRequest {
  ResolvedRenderGeometry geometry{};
  bool                   invert  = false;
  float                  opacity = 1.0f;
};

/**
 * @brief Check @p request before any allocation or device work.
 * @throws std::runtime_error with a "Mask coverage display:" message for an empty render or
 *         reference extent, a render axis above @ref kMaskCoverageDisplayMaximumAxis, or an
 *         opacity that is not finite or not in [0, 1].
 */
inline void ValidateMaskCoverageDisplayRequest(const MaskCoverageDisplayRequest& request) {
  const auto& geometry = request.geometry;
  if (geometry.render_extent.Empty() || geometry.full_reference_extent.Empty()) {
    throw std::runtime_error("Mask coverage display: the photo frame geometry has an empty extent");
  }
  if (geometry.render_extent.width > kMaskCoverageDisplayMaximumAxis ||
      geometry.render_extent.height > kMaskCoverageDisplayMaximumAxis) {
    throw std::runtime_error("Mask coverage display: the " +
                             std::to_string(geometry.render_extent.width) + " x " +
                             std::to_string(geometry.render_extent.height) +
                             " photo frame exceeds the largest display target axis " +
                             std::to_string(kMaskCoverageDisplayMaximumAxis));
  }
  if (!std::isfinite(request.opacity) || request.opacity < 0.0f || request.opacity > 1.0f) {
    throw std::runtime_error("Mask coverage display: opacity must be in [0, 1]");
  }
}

/**
 * @brief One RGBA32F coverage display target of a render workspace.
 *
 * @p readers is shared with each presentation reader of the texture (a Metal viewport frame).
 * While a copy exists outside @ref MaskCoverageDisplayTargets, the texture is not written again.
 */
template <class Backend>
struct MaskCoverageDisplayTarget {
  typename Backend::Texture2D texture{};
  Extent2D                    extent{};
  std::shared_ptr<const void> readers              = std::make_shared<char>();
  std::uint64_t               last_used_submission = 0;
};

/**
 * @brief Owner of the RGBA32F textures that coverage display writes (SM8.5).
 *
 * The device writes effective coverage into a target; the frame presenter then copies it into the
 * native target of the viewport coverage channel (CUDA, OpenCL) or hands the texture itself to the
 * viewport (Metal). A target that no presentation reader holds is reused for the next write of the
 * same size on the ordered device queue, so CUDA and OpenCL keep one target per size and Metal
 * keeps one more target per frame that the viewport still displays. These textures are not photo
 * results and not part of the texture pool.
 *
 * Owner: BasicRenderWorkspace. Thread: the render thread of the workspace. Released by
 * BasicRenderWorkspace::ReleaseSessionResources. A Metal reader keeps its own reference to the
 * native texture, so it stays valid after the release.
 */
template <class Backend>
class MaskCoverageDisplayTargets {
 public:
  using Target                 = MaskCoverageDisplayTarget<Backend>;

  MaskCoverageDisplayTargets() = default;
  MaskCoverageDisplayTargets(const MaskCoverageDisplayTargets&)                    = delete;
  auto operator=(const MaskCoverageDisplayTargets&) -> MaskCoverageDisplayTargets& = delete;

  /**
   * @brief A target of @p extent for the work of @p submission.
   *
   * Reuses a target of @p extent that no reader holds. Otherwise allocates one and then destroys
   * the targets of another size that no reader holds and no in-flight submission uses. The
   * returned reference stays valid until the next Acquire or Clear.
   *
   * @throws std::runtime_error with a "Mask coverage display:" message when the allocation fails;
   *         the existing targets are unchanged.
   */
  auto Acquire(Extent2D extent, Backend& backend, std::uint64_t submission) -> Target& {
    for (auto& target : targets_) {
      if (target->extent == extent && target->readers.use_count() == 1) {
        target->last_used_submission = submission;
        return *target;
      }
    }
    auto target = std::make_unique<Target>();
    try {
      target->texture =
          backend.CreateTexture2D(extent.width, extent.height, TextureFormat::Rgba32f);
    } catch (const std::exception& error) {
      throw std::runtime_error(
          "Mask coverage display: allocating the " + std::to_string(extent.width) + " x " +
          std::to_string(extent.height) + " RGBA32F target failed: " + error.what());
    }
    target->extent               = extent;
    target->last_used_submission = submission;
    std::erase_if(targets_, [&](const std::unique_ptr<Target>& other) {
      return other->extent != extent && other->readers.use_count() == 1 &&
             !backend.IsResourceBusy(other->last_used_submission);
    });
    targets_.push_back(std::move(target));
    ++create_count_;
    return *targets_.back();
  }

  /// Destroy every target. @pre The GPU no longer uses them (the caller waited for idle).
  void               Clear() { targets_.clear(); }

  /// Number of live targets.
  [[nodiscard]] auto Count() const -> std::size_t { return targets_.size(); }
  /// Device bytes of the live targets (16 bytes per texel).
  [[nodiscard]] auto Bytes() const -> std::size_t {
    std::size_t bytes = 0;
    for (const auto& target : targets_) {
      bytes += static_cast<std::size_t>(target->extent.width) * target->extent.height *
               TextureFormatBytesPerPixel(TextureFormat::Rgba32f);
    }
    return bytes;
  }
  /// Number of target allocations since construction.
  [[nodiscard]] auto CreateCount() const -> std::uint64_t { return create_count_; }

 private:
  std::vector<std::unique_ptr<Target>> targets_;
  std::uint64_t                        create_count_ = 0;
};

}  // namespace alcedo
