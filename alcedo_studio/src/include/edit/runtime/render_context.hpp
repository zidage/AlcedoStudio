//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include "edit/geometry/resolved_render_geometry.hpp"
#include "edit/geometry/types.hpp"
#include "edit/runtime/develop_compile_source.hpp"

namespace alcedo {

/**
 * @brief Per-frame read-only render inputs. Does not own GPU memory.
 */
struct RenderContext {
  SourceContentKey              source_key{};
  RenderQuality                 quality = RenderQuality::Preview;
  const ResolvedRenderGeometry* geometry = nullptr;
};

}  // namespace alcedo
