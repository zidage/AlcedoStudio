//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#ifdef HAVE_METAL

#include <cstdint>
#include <vector>

#include "edit/geometry/resolved_render_geometry.hpp"
#include "edit/graph/graph_ids.hpp"
#include "edit/runtime/content_key.hpp"
#include "edit/runtime/metal/metal_backend.hpp"
#include "edit/runtime/metal/metal_renderer.hpp"

namespace alcedo {

struct MetalLocalToneResult {
  std::uint64_t reference_resource_id       = 0;
  bool          rebuilt_reference           = false;
  bool          sampled_canonical_reference = false;
  std::uint32_t transient_bytes             = 0;
};

/**
 * @brief Encode Shadows/Highlights LLF onto the current Metal command buffer.
 *
 * Pyramid scratch comes from TransientBufferArena. Canonical source/result planes live
 * in workspace Values() under GraphValueId. Failures throw; there is no CPU or
 * MetalStage substitute.
 */
[[nodiscard]] auto ExecuteMetalLocalTone(MetalRenderDevice&             device,
                                         const MetalBackend::Texture2D& input,
                                         MetalBackend::Texture2D& output, const NodeId& grade_id,
                                         float shadows_slider, float highlights_slider,
                                         const ResolvedRenderGeometry& geometry,
                                         ContentKey source_key, ContentKey result_key)
    -> MetalLocalToneResult;

void AppendMetalLocalToneWarmup(std::vector<MetalPipelineWarmup>& pipelines);

}  // namespace alcedo

#endif
