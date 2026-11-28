//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// Phase VK11 (#1036): the Vulkan dispatch of the AI Mask Feather and Expand (SM8 section 5.1),
// the form of opencl_ai_mask_coverage.cpp. The kernels are the group ai_mask_coverage
// (shader/ai_mask_coverage.slang).

#ifdef HAVE_VULKAN

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>

#include "edit/runtime/ai_mask_coverage_work.hpp"
#include "edit/runtime/texture_format.hpp"
#include "edit/runtime/vulkan/vulkan_backend.hpp"
#include "edit/runtime/vulkan/vulkan_kernel_dispatch.hpp"
#include "vulkan/vulkan_ai_mask_coverage_kernels.hpp"

namespace alcedo {
namespace {

namespace kernels = vulkan::ai_mask_coverage_kernels;

void RequireTexture(const VulkanBackend::Texture2D* texture, Extent2D extent, TextureFormat format,
                    const char* name) {
  if (texture == nullptr || texture->Empty()) {
    throw std::runtime_error(std::string{"VulkanBackend::RecordAiMaskEdge: the "} + name +
                             " texture is missing");
  }
  if (texture->Width() != extent.width || texture->Height() != extent.height ||
      texture->Format() != format) {
    throw std::runtime_error(std::string{"VulkanBackend::RecordAiMaskEdge: the "} + name +
                             " texture is not " + std::to_string(extent.width) + " x " +
                             std::to_string(extent.height) + " " + TextureFormatName(format));
  }
}

/// The grid of a pass whose input is @p input placed at (@p x, @p y) in the output.
auto MakeGrid(Extent2D output, const VulkanBackend::Texture2D& input, std::int32_t x,
              std::int32_t y) -> kernels::EdgePushConstants {
  kernels::EdgePushConstants push;
  push.width         = static_cast<std::int32_t>(output.width);
  push.height        = static_cast<std::int32_t>(output.height);
  push.source_x      = x;
  push.source_y      = y;
  push.source_width  = static_cast<std::int32_t>(input.Width());
  push.source_height = static_cast<std::int32_t>(input.Height());
  return push;
}

}  // namespace

void VulkanBackend::RecordAiMaskEdge(const AiMaskEdgeDispatch&            dispatch,
                                     const AiMaskEdgeTextures<Texture2D>& textures,
                                     CommandContext&                      command_context) {
  const auto extent = dispatch.output_extent;
  if (extent.Empty() || (dispatch.expand_steps == 0 && !dispatch.HasFeather())) {
    throw std::runtime_error("VulkanBackend::RecordAiMaskEdge: the dispatch has no work");
  }
  if (static_cast<int>(dispatch.feather_weights.size()) > kAiMaskFeatherMaximumRadius + 1) {
    throw std::runtime_error("VulkanBackend::RecordAiMaskEdge: the Feather radius is too large");
  }
  RequireTexture(textures.source, dispatch.source_extent, TextureFormat::R8, "source");
  RequireTexture(textures.output, extent, TextureFormat::R8, "output");
  for (std::uint32_t i = 0; i < dispatch.MorphologyWorkTextureCount(); ++i) {
    RequireTexture(textures.morphology[i], extent, TextureFormat::R8, "morphology work");
  }
  if (dispatch.HasFeather()) {
    RequireTexture(textures.feather_rows, extent, TextureFormat::R32f, "Feather work");
  }

  const auto groups =
      WorkgroupsCovering(extent.width, extent.height, kernels::kGroupSize, kernels::kGroupSize);
  const Texture2D* current = textures.source;
  auto             grid    = MakeGrid(extent, *current, dispatch.source_x, dispatch.source_y);
  if (dispatch.expand_steps > 0) {
    static const auto kMorphology = kernels::MorphologyDescriptor();
    for (std::int32_t step = 0; step < dispatch.expand_steps; ++step) {
      const bool last_writes_output = !dispatch.HasFeather() && step + 1 == dispatch.expand_steps;
      Texture2D& target = last_writes_output ? *textures.output : *textures.morphology[step % 2];
      auto       push   = grid;
      push.dilate       = dispatch.dilate ? 1 : 0;
      push.square       = step % 2;
      const std::array<VulkanImageBinding, 2> images = {
          VulkanImageBinding{kernels::kInputBinding, current},
          VulkanImageBinding{kernels::kOutputBinding, &target}};
      RecordKernelDispatch(*this, command_context, kMorphology, images, PushConstantBytes(push),
                           groups);
      current = &target;
      grid    = MakeGrid(extent, target, 0, 0);
    }
  }

  if (dispatch.HasFeather()) {
    // The half kernel exceeds the push constant limit (201 floats): a buffer of this call. The
    // reset at the end gives it back; the lifetime rule keeps it until its batch completes.
    const auto weight_bytes = std::as_bytes(std::span<const float>(dispatch.feather_weights));
    auto       weights      = CreateBuffer(weight_bytes.size());
    UploadBufferRange(weights, 0, weight_bytes, command_context);
    const auto        radius = static_cast<std::int32_t>(dispatch.feather_weights.size()) - 1;

    static const auto kRows  = kernels::FeatherRowsDescriptor();
    auto              rows   = grid;
    rows.weights             = weights.DeviceAddress();
    rows.radius              = radius;
    const std::array<VulkanImageBinding, 2> row_images = {
        VulkanImageBinding{kernels::kInputBinding, current},
        VulkanImageBinding{kernels::kRowsBinding, textures.feather_rows}};
    RecordKernelDispatch(*this, command_context, kRows, row_images, PushConstantBytes(rows),
                         groups);

    static const auto kColumns = kernels::FeatherColumnsDescriptor();
    auto              columns  = MakeGrid(extent, *textures.feather_rows, 0, 0);
    columns.weights            = weights.DeviceAddress();
    columns.radius             = radius;
    const std::array<VulkanImageBinding, 2> column_images = {
        VulkanImageBinding{kernels::kOutputBinding, textures.output},
        VulkanImageBinding{kernels::kRowsBinding, textures.feather_rows}};
    RecordKernelDispatch(*this, command_context, kColumns, column_images,
                         PushConstantBytes(columns), groups);
    weights.Reset();
  }
}

}  // namespace alcedo

#endif  // HAVE_VULKAN
