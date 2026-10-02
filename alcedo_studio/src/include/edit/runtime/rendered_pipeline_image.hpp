//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <memory>

#include "edit/geometry/resolved_render_geometry.hpp"
#include "image/image_buffer.hpp"
#include "ui/edit_viewer/frame_sink.hpp"

namespace alcedo {

/**
 * @brief Host pixels of one render, with the geometry and display encoding that produced them.
 *
 * Returned by PipelineExecutor::ApplyImage and Renderer::RenderImage for a request that asks for
 * host output. It holds no document, history, or executor state, so it stays valid after the
 * renderer releases its submission-local resources and after later renders.
 *
 * Owner: the caller of the render. The pixel buffer is the renderer's download; nothing else
 * refers to it.
 */
struct RenderedPipelineImage {
  /// Downloaded output: CPU data in RGBA32F, render_extent pixels wide and high.
  std::shared_ptr<ImageBuffer> pixels;
  /// Exact ResolveRenderGeometry result of the executed plan. `render_to_reference` places the
  /// pixels in the full source reference space.
  ResolvedRenderGeometry       geometry{};
  /// Output encoding of @ref pixels: the document DRT, or the request's output color override.
  ViewerDisplayConfig          display{};
};

}  // namespace alcedo
