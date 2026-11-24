//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>
#include <optional>

#include "edit/geometry/render_request.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "io/image/export_color_profile_config.hpp"

/**
 * @file
 * @brief Render size and output encoding of a browser tile preview: the LUT previews (issue #598)
 * and the preset previews (edit presets plan, PS3).
 */
namespace alcedo {

/// Long edge of a browser tile preview, in pixels. A tile is at least 128 logical pixels wide.
inline constexpr std::uint32_t kBrowserPreviewLongEdge = 256;

/// Render request of a browser tile preview: the crop and rotation of the preview document,
/// Preview resampling, long edge kBrowserPreviewLongEdge.
[[nodiscard]] auto             MakeBrowserPreviewRenderRequest() -> RenderRequest;

/**
 * @brief Output encoding of a browser tile preview of @p document.
 *
 * Empty for an SDR DRT: the preview uses the encoding of the document. For an ST 2084 or HLG
 * DRT, Rec.709 gamma 2.2 at 100 nits (ExportColorProfileConfig defaults), because a tile is an
 * 8-bit SDR image.
 */
[[nodiscard]] auto             BrowserPreviewOutputColor(const PipelineDocument& document)
    -> std::optional<ExportColorProfileConfig>;

}  // namespace alcedo
