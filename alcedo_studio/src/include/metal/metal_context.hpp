//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#ifdef HAVE_METAL
#include <alcedo/metal/Metal.hpp>
namespace alcedo {
class MetalContext {
 private:
  MTL::Device*       device_ = nullptr;
  MTL::CommandQueue* queue_  = nullptr;

  MetalContext();

  ~MetalContext();

 public:
  MetalContext(const MetalContext&)                      = delete;
  auto operator=(const MetalContext&) -> MetalContext&   = delete;
  MetalContext(MetalContext&&)                           = delete;
  auto        operator=(MetalContext&&) -> MetalContext& = delete;

  static auto Instance() -> MetalContext&;

  /**
   * @brief Bind the app/Qt presentation device before the first Instance() call.
   *
   * If Instance() already exists, @p device must be that same MTLDevice.
   * Workspace, MetalImage, and present all read this device; they do not create
   * another one.
   */
  static void BindPresentationDevice(MTL::Device* device, MTL::CommandQueue* queue = nullptr);

  auto        Device() const -> MTL::Device*;
  auto        Queue() const -> MTL::CommandQueue*;
};

}  // namespace alcedo

#endif
