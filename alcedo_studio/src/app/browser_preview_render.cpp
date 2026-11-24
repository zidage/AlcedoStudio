//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/browser_preview_render.hpp"

#include "edit/graph/drt_node_model.hpp"

namespace alcedo {

auto MakeBrowserPreviewRenderRequest() -> RenderRequest {
  RenderRequest request;
  request.document_geometry   = DocumentGeometryUse::ApplyCropAndRotation;
  request.resolution.max_edge = kBrowserPreviewLongEdge;
  request.resolution.quality  = RenderQuality::Preview;
  return request;
}

auto BrowserPreviewOutputColor(const PipelineDocument& document)
    -> std::optional<ExportColorProfileConfig> {
  const auto* drt = document.Drt();
  if (drt != nullptr && IsHdrExportEncoding(*drt)) {
    return ExportColorProfileConfig{};
  }
  return std::nullopt;
}

}  // namespace alcedo
