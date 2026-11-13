//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#ifdef HAVE_METAL

#include <cstddef>
#include <cstdint>

#include <alcedo/metal/Metal.hpp>

#include "ui/edit_viewer/frame_sink.hpp"

namespace alcedo::scope::metal_detail {

enum class MetalLinearImageStorage : uint32_t {
  Float32RGBA,
  UInt32RGBA,
};

struct MetalTextureImageResource {
  NS::SharedPtr<MTL::Texture> texture      = nullptr;
  int                         width        = 0;
  int                         height       = 0;
  FramePixelFormat            format       = FramePixelFormat::RGBA32F;
  std::uintptr_t              native_object = 0;
};

struct MetalCommandBufferSignalResource {
  NS::SharedPtr<MTL::CommandBuffer> command_buffer = nullptr;
};

struct MetalBufferResource {
  NS::SharedPtr<MTL::Buffer> buffer      = nullptr;
  size_t                    size_bytes  = 0;
};

struct MetalLinearImageResource {
  NS::SharedPtr<MTL::Buffer>   buffer       = nullptr;
  size_t                       row_bytes    = 0;
  int                          width        = 0;
  int                          height       = 0;
  FramePixelFormat             format       = FramePixelFormat::RGBA32F;
  MetalLinearImageStorage      storage      = MetalLinearImageStorage::Float32RGBA;
};

}  // namespace alcedo::scope::metal_detail

#endif
