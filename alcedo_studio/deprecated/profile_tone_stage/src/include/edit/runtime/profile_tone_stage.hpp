// Copyright 2026 Yurun Zi
// SPDX-License-Identifier: GPL-3.0-only
// Additional permission under GPLv3 section 7 applies; see the LICENSE file.
#pragma once

#include <optional>
#include <span>
#include <stdexcept>
#include <vector>

#include "edit/graph/drt_node_model.hpp"
#include "edit/graph/graph_ids.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "image/dng_tone_curve.hpp"

// The profile tone stage at the entry of the display transform (dcp_profile_tone_curve_study.md,
// section 6, phase T2): which documents run it, and its GPU buffer.

namespace alcedo {

/// The profile tone stage that the display transform of a document runs.
struct ProfileToneStage {
  DngToneCurvePtr curve;                       ///< `C`; never null.
  DrtMethod       method = DrtMethod::Aces20;  ///< `m`: ACES 2.0 or OpenDRT.
};

/**
 * @brief The stage of the display transform of @p document, or nullopt when it stays out.
 *
 * The stage runs when the Develop node holds a `camera_profile_file` reference with `profile_tone`
 * true, its profile has a `ProfileToneCurve`, and the DRT method is ACES 2.0 or OpenDRT (study
 * section 6.2). Film Print keeps it out (constraint K-6); the stored setting stays.
 *
 * @throws std::runtime_error for an unbound reference whose stage would run: its curve is unknown,
 *         and the camera pass fails for the same reference.
 */
[[nodiscard]] auto ResolveProfileToneStage(const PipelineDocument& document)
    -> std::optional<ProfileToneStage>;

/**
 * @brief The GPU buffer of @p stage, in the layout of profile_tone_gpu_math.h.
 *
 * The matrices `M` and `M^-1`, the anchor scale, the inverse table exponent, the `C` table of the
 * curve, and the `N_m^-1` table of ProfileToneNeutralResponse::Resolve(@p stage.method).
 */
[[nodiscard]] auto PackProfileToneGpuData(const ProfileToneStage& stage) -> std::vector<float>;

/// Upload @p data to the buffer of the stage of @p drt_node, kept with the workspace until its GPU
/// submission completes.
template <class Workspace, class Context>
auto UploadProfileToneGpuData(Workspace& workspace, const NodeId& drt_node,
                              const std::vector<float>& data, Context& context) -> auto& {
  const GraphValueId id{drt_node, PortId{"profile_tone"}};
  const auto         bytes  = std::as_bytes(std::span(data));
  auto*              buffer = workspace.Values().Find(id);
  if (!buffer || buffer->Bytes() < bytes.size()) {
    workspace.Values().Store(id, workspace.Device().CreateBuffer(bytes.size()));
    buffer = workspace.Values().Find(id);
  }
  if (!buffer) throw std::runtime_error("Profile tone: GPU table allocation failed");
  workspace.Device().UploadBufferRange(*buffer, 0, bytes, context);
  return *buffer;
}

}  // namespace alcedo
