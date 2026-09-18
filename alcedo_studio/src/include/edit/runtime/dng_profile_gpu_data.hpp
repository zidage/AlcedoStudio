// Copyright 2026 Yurun Zi
// SPDX-License-Identifier: GPL-3.0-only
// Additional permission under GPLv3 section 7 applies; see the LICENSE file.
#pragma once
#include <span>
#include <stdexcept>
#include <vector>

#include "edit/graph/develop_color_transform.hpp"
#include "edit/graph/graph_ids.hpp"

namespace alcedo {
/// Pack interpolated tables for direct GPU evaluation, without a resampled RGB cube.
auto PackDngProfileGpuData(const DevelopCameraProfile&  profile,
                           const DevelopColorTransform& transform) -> std::vector<float>;

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
