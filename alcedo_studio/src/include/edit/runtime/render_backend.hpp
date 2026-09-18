//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <concepts>
#include <cstddef>
#include <cstdint>

#include "edit/runtime/texture_format.hpp"

namespace alcedo {

/**
 * @brief GPU resource factory used by workspace, PlanExecutor, and Renderer.
 *
 * Native CUDA/Metal types stay in backend headers. This concept is host-only.
 * @tparam Backend Resource factory with move-only Buffer and Texture2D.
 */
template <class Backend>
concept RenderBackend = requires(Backend backend, typename Backend::CommandContext& commands) {
  typename Backend::Buffer;
  typename Backend::Texture2D;
  typename Backend::CommandContext;
  backend.CreateBuffer(std::size_t{});
  backend.CreateTexture2D(std::uint32_t{}, std::uint32_t{}, TextureFormat{});
  backend.Submit(commands);
  backend.Wait(commands);
};

}  // namespace alcedo
