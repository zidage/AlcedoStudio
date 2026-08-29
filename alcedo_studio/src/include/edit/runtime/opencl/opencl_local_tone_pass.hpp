//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#ifdef HAVE_OPENCL

#include <cstdint>

#include "edit/geometry/resolved_render_geometry.hpp"
#include "edit/graph/graph_ids.hpp"
#include "edit/runtime/content_key.hpp"
#include "edit/runtime/opencl/opencl_backend.hpp"

namespace alcedo {

struct OpenClLocalToneResult {
  std::uint64_t reference_resource_id       = 0;
  bool          rebuilt_reference           = false;
  bool          sampled_canonical_reference = false;
  std::uint32_t transient_bytes             = 0;
};

/**
 * @brief Encode the Shadows/Highlights local-Laplacian stage on the current OpenCL queue.
 *
 * Canonical source and adjusted planes are R32f graph images owned by GraphImageCache. The
 * source/remap/result pyramid planes are transient slab views. The function only enqueues work;
 * it does not wait, finish the queue, allocate a private cache, or call the legacy OpenCL stage.
 * Failures throw and leave canonical writes unpublished until BasicRenderDevice::PublishResults.
 */
[[nodiscard]] auto ExecuteOpenClLocalTone(OpenClRenderDevice&             device,
                                          const OpenClBackend::Texture2D& input,
                                          OpenClBackend::Texture2D& output, const NodeId& grade_id,
                                          float shadows_slider, float highlights_slider,
                                          const ResolvedRenderGeometry& geometry,
                                          ContentKey source_key, ContentKey result_key)
    -> OpenClLocalToneResult;

}  // namespace alcedo

#endif  // HAVE_OPENCL
