// Copyright 2026 Yurun Zi
// SPDX-License-Identifier: GPL-3.0-only
// Additional permission under GPLv3 section 7 applies; see the LICENSE file.
#pragma once
#include <array>
#include <opencv2/core.hpp>
#include <span>
#include <stdexcept>
#include <vector>

#include "edit/graph/develop_color_transform.hpp"
#include "edit/graph/graph_ids.hpp"

namespace alcedo {
/**
 * @brief Linear ProPhoto RGB (D50) to linear AP1 with @p xyz_d50_to_ap1
 *        (DevelopColorTransform::xyz_d50_to_ap1, which equals XyzD50ToAp1Matrix).
 *
 * The camera pass evaluates the DNG profile tables in ProPhoto with this matrix and its inverse;
 * the profile tone stage uses the same pair (dcp_profile_tone_curve_study.md, section 6.1).
 */
auto ProPhotoToAp1(const std::array<float, 9>& xyz_d50_to_ap1) -> cv::Matx33d;

/// Pack interpolated tables for direct GPU evaluation, without a resampled RGB cube.
auto PackDngProfileGpuData(const DevelopCameraProfile&  profile,
                           const DevelopColorTransform& transform) -> std::vector<float>;

/**
 * @brief Pack the tables of the effective camera profile of @p develop (EffectiveCameraProfile).
 *
 * With a bound DCP reference the buffer holds the DCP tables, blended by the DCP illuminant CCT
 * values. @p transform must come from ResolveDevelopColorTransform of the same @p develop.
 * @throws std::runtime_error for an unbound DNG profile or an unbound DCP reference.
 */
auto PackDngProfileGpuData(const DevelopPayload& develop, const DevelopColorTransform& transform)
    -> std::vector<float>;

/// Keep the exact table buffer alive with the workspace until its GPU submission completes.
template <class Workspace, class Context>
auto UploadDngProfileGpuData(Workspace& workspace, const NodeId& node,
                             const std::vector<float>& data, Context& context) -> auto& {
  const GraphValueId id{node, PortId{"dng_profile"}};
  const auto         bytes  = std::as_bytes(std::span(data));
  auto*              buffer = workspace.Values().Find(id);
  if (!buffer || buffer->Bytes() < bytes.size()) {
    workspace.Values().Store(id, workspace.Device().CreateBuffer(bytes.size()));
    buffer = workspace.Values().Find(id);
  }
  if (!buffer) throw std::runtime_error("DNG profile: GPU table allocation failed");
  workspace.Device().UploadBufferRange(*buffer, 0, bytes, context);
  return *buffer;
}
}  // namespace alcedo
