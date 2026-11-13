//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>

#include "edit/geometry/resolved_render_geometry.hpp"
#include "edit/runtime/cuda/cuda_backend.hpp"

namespace alcedo {

/**
 * @brief Internal GPU pass: crop, rotation, view crop, and dynamic scale in one kernel.
 *
 * Not a user node and not serialized. GraphCompiler (G4+) inserts this between Develop
 * output and the first ColorGrade. One in-flight submission; does not allocate textures.
 *
 * @pre src is decoded RGBA32F, dst is render RGBA32F, sizes match @p geometry.
 * @throws std::runtime_error on size or format mismatch, or CUDA launch failure.
 */
class GeometryResamplePass {
 public:
  void Encode(const ResolvedRenderGeometry& geometry, const CudaBackend::Texture2D& src,
              CudaBackend::Texture2D& dst, CudaCommandContext& command_context);

  [[nodiscard]] auto LaunchCount() const -> std::uint32_t { return launch_count_; }
  void               ResetLaunchCount() { launch_count_ = 0; }

 private:
  std::uint32_t launch_count_ = 0;
};

}  // namespace alcedo
