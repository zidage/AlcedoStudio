//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#ifdef HAVE_VULKAN

#include "decoders/processor/operators/gpu/vulkan_raw_encode.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <iterator>
#include <stdexcept>
#include <string>
#include <utility>

#include "edit/runtime/vulkan/vulkan_kernel_dispatch.hpp"
#include "vulkan/vulkan_debayer_rcd_kernels.hpp"
#include "vulkan/vulkan_raw_pack_kernels.hpp"

namespace alcedo::vulkan::raw {
namespace {

constexpr std::size_t kU16Bytes        = sizeof(std::uint16_t);
constexpr std::size_t kFloatBytes      = sizeof(float);
constexpr std::size_t kRgbaBytes       = 4 * sizeof(float);
/// The slot alignment of `ParameterArena`; also the alignment of an RGBA texel.
constexpr std::size_t kRecordAlignment = 16;
constexpr float       kMinGain         = 1e-6F;

auto                  Texels(std::uint32_t width, std::uint32_t height) -> std::size_t {
  return static_cast<std::size_t>(width) * height;
}

auto Groups(std::uint32_t width, std::uint32_t height) -> VulkanWorkgroupCount {
  // Every kernel of the RAW groups has 16 x 16 workgroups.
  static_assert(raw_core_kernels::kGroupSize == 16 && debayer_rcd_kernels::kGroupSize == 16 &&
                xtrans_kernels::kGroupSize == 16 && raw_pack_kernels::kGroupSize == 16 &&
                highlight_kernels::kGroupSize == 16);
  return WorkgroupsCovering(width, height, 16, 16);
}

template <class Record>
void UploadRecord(VulkanBackend& backend, VulkanCommandContext& command_context,
                  const VulkanBufferView& view, const Record& record) {
  backend.UploadBufferRange(*view.buffer, view.offset_bytes, PushConstantBytes(record),
                            command_context);
}

[[noreturn]] void ThrowRequestError(const char* operation, const std::string& message) {
  throw std::invalid_argument(std::string(operation) + ": " + message);
}

auto InverseScale(std::span<const float, 4> cam_mul) -> std::array<float, 3> {
  const float g = std::max(cam_mul[1], kMinGain);
  return {g / std::max(cam_mul[0], kMinGain), 1.0F, g / std::max(cam_mul[2], kMinGain)};
}

auto OrientedSize(std::uint32_t width, std::uint32_t height, int flip)
    -> std::pair<std::uint32_t, std::uint32_t> {
  if (flip == 5 || flip == 6) {
    return {height, width};
  }
  return {width, height};
}

/// The parameters of the pack kernels, after a check of the crop and the destination texture.
auto PackParams(const char* operation, const VulkanBackend::Texture2D& destination, RectI crop,
                std::uint32_t source_width, std::uint32_t source_height,
                std::span<const float, 4> cam_mul, int flip) -> raw_pack_kernels::PackOrientParams {
  if (crop.x < 0 || crop.y < 0 || crop.width <= 0 || crop.height <= 0 ||
      static_cast<std::int64_t>(crop.x) + crop.width > source_width ||
      static_cast<std::int64_t>(crop.y) + crop.height > source_height) {
    ThrowRequestError(operation,
                      "crop (" + std::to_string(crop.x) + ", " + std::to_string(crop.y) + ", " +
                          std::to_string(crop.width) + " x " + std::to_string(crop.height) +
                          ") is not inside the source of " + std::to_string(source_width) + " x " +
                          std::to_string(source_height));
  }
  const auto crop_width              = static_cast<std::uint32_t>(crop.width);
  const auto crop_height             = static_cast<std::uint32_t>(crop.height);
  const auto [dst_width, dst_height] = OrientedSize(crop_width, crop_height, flip);
  if (destination.Empty() || destination.Format() != TextureFormat::Rgba32f ||
      destination.Width() != dst_width || destination.Height() != dst_height) {
    ThrowRequestError(operation, "the destination must be an RGBA32F texture of " +
                                     std::to_string(dst_width) + " x " +
                                     std::to_string(dst_height) + " (flip " + std::to_string(flip) +
                                     "); it is " + std::to_string(destination.Width()) + " x " +
                                     std::to_string(destination.Height()));
  }
  const auto                         scale = InverseScale(cam_mul);
  raw_pack_kernels::PackOrientParams params;
  params.src_x        = static_cast<std::uint32_t>(crop.x);
  params.src_y        = static_cast<std::uint32_t>(crop.y);
  params.src_width    = crop_width;
  params.src_height   = crop_height;
  params.dst_width    = dst_width;
  params.dst_height   = dst_height;
  params.flip         = static_cast<std::uint32_t>(flip);
  params.scale_r      = scale[0];
  params.scale_g      = scale[1];
  params.scale_b      = scale[2];
  params.plane_stride = source_width;
  params.src_stride   = source_width;
  return params;
}

/// The OpenCL `MakeLinearizeParams` of opencl_encode.cpp.
auto MakeToLinearRefParameters(const RawLinearizationParams& linearization,
                               const RawCfaPattern& pattern, std::uint32_t width,
                               std::uint32_t height) -> raw_core_kernels::ToLinearRefParameters {
  raw_core_kernels::ToLinearRefParameters record{};
  auto&                                   wb = record.wb;
  for (int c = 0; c < 4; ++c) {
    wb.black_level[c]    = linearization.black_level[c];
    wb.white_level[c]    = linearization.white_level[c];
    wb.wb_multipliers[c] = linearization.cam_mul[c];
  }
  wb.apply_white_balance = linearization.apply_as_shot_wb != 0 ? 1 : 0;
  wb.black_tile_width    = linearization.black_tile_width;
  wb.black_tile_height   = linearization.black_tile_height;
  std::copy(std::begin(linearization.pattern_black), std::end(linearization.pattern_black),
            std::begin(wb.pattern_black));

  auto& cfa  = record.pattern;
  cfa.width  = static_cast<std::int32_t>(width);
  cfa.height = static_cast<std::int32_t>(height);
  if (pattern.kind == RawCfaKind::XTrans6x6) {
    cfa.tile_width  = 6;
    cfa.tile_height = 6;
    std::copy(std::begin(pattern.xtrans_pattern.raw_fc), std::end(pattern.xtrans_pattern.raw_fc),
              std::begin(cfa.raw_fc));
  } else {
    cfa.tile_width  = 2;
    cfa.tile_height = 2;
    std::copy(std::begin(pattern.bayer_pattern.raw_fc), std::end(pattern.bayer_pattern.raw_fc),
              std::begin(cfa.raw_fc));
  }
  return record;
}

constexpr float kHilightMagic = 0.987F;
constexpr float kChromaRingLo = 0.2F;

auto            ChannelRatio(float value, float green) -> float {
  if (!std::isfinite(value) || value <= 0.0F) {
    return 1.0F;
  }
  return std::clamp(value / green, 0.25F, 4.0F);
}

/// The OpenCL `MakeHighlightParams` of opencl_encode.cpp, for a region of @p width x @p height
/// in a source of @p stride texels in a row.
auto MakeHighlightLimits(std::span<const float, 4> cam_mul, std::uint32_t width,
                         std::uint32_t height, std::uint32_t stride)
    -> highlight_kernels::HighlightLimits {
  const float green = std::isfinite(cam_mul[1]) && cam_mul[1] > 0.0F ? cam_mul[1] : 1.0F;
  highlight_kernels::HighlightLimits limits;
  limits.clip_r     = kHilightMagic * ChannelRatio(cam_mul[0], green);
  limits.clip_g     = kHilightMagic;
  limits.clip_b     = kHilightMagic * ChannelRatio(cam_mul[2], green);
  limits.clipdark_r = kChromaRingLo * limits.clip_r;
  limits.clipdark_g = kChromaRingLo * limits.clip_g;
  limits.clipdark_b = kChromaRingLo * limits.clip_b;
  limits.width      = width;
  limits.height     = height;
  limits.stride     = stride;
  return limits;
}

/// The device addresses of the three scratch views for a region of @p width x @p height.
struct HighlightScratchAddresses {
  VkDeviceAddress mask       = 0;
  VkDeviceAddress dilated    = 0;
  VkDeviceAddress statistics = 0;
};

auto RequireHighlightScratch(const HighlightScratch& scratch, std::uint32_t width,
                             std::uint32_t height, const char* operation)
    -> HighlightScratchAddresses {
  constexpr std::size_t kWordBytes = sizeof(std::uint32_t);
  const std::size_t     mask_bytes = HighlightMaskBytes(width, height);
  return {RequireBufferRange(scratch.mask, mask_bytes, kWordBytes, operation, "mask"),
          RequireBufferRange(scratch.dilated, mask_bytes, kWordBytes, operation, "dilated mask"),
          RequireBufferRange(scratch.statistics, kHighlightStatisticsBytes, kWordBytes, operation,
                             "statistics")};
}

void ClearHighlightStatistics(VulkanBackend& backend, VulkanCommandContext& command_context,
                              const HighlightScratch& scratch) {
  backend.FillBufferRange(*scratch.statistics.buffer, scratch.statistics.offset_bytes,
                          kHighlightStatisticsBytes, 0, command_context);
}

}  // namespace

void EncodeLinearizeRgb(VulkanBackend& backend, VulkanCommandContext& command_context,
                        VulkanBufferView rgba, std::uint32_t width, std::uint32_t height,
                        const RawRgbLinearizationParams& params) {
  constexpr const char*                       kOperation = "vulkan::raw::EncodeLinearizeRgb";
  raw_core_kernels::LinearizeRgbPushConstants push;
  push.rgba =
      RequireBufferRange(rgba, Texels(width, height) * kRgbaBytes, kRgbaBytes, kOperation, "RGBA");
  push.width  = width;
  push.height = height;
  push.params = params;
  if (Texels(width, height) == 0) {
    return;
  }
  RecordKernelDispatch(backend, command_context, raw_core_kernels::LinearizeRgbDescriptor(), {},
                       PushConstantBytes(push), Groups(width, height));
}

void EncodeToLinearRef(VulkanBackend& backend, VulkanCommandContext& command_context,
                       VulkanBufferView source_u16, VulkanBufferView destination_f32,
                       VulkanBufferView parameters, std::uint32_t width, std::uint32_t height,
                       const RawLinearizationParams& linearization, const RawCfaPattern& pattern) {
  constexpr const char*                      kOperation = "vulkan::raw::EncodeToLinearRef";
  const std::size_t                          texels     = Texels(width, height);
  raw_core_kernels::ToLinearRefPushConstants push;
  push.source =
      RequireBufferRange(source_u16, texels * kU16Bytes, kU16Bytes, kOperation, "CFA source");
  push.destination = RequireBufferRange(destination_f32, texels * kFloatBytes, kFloatBytes,
                                        kOperation, "linear destination");
  push.parameters  = RequireBufferRange(parameters, kToLinearRefParameterBytes, kRecordAlignment,
                                        kOperation, "parameters");
  if (texels == 0) {
    return;
  }
  UploadRecord(backend, command_context, parameters,
               MakeToLinearRefParameters(linearization, pattern, width, height));
  RecordKernelDispatch(backend, command_context, raw_core_kernels::ToLinearRefDescriptor(), {},
                       PushConstantBytes(push), Groups(width, height));
}

void EncodeCfaClamp01(VulkanBackend& backend, VulkanCommandContext& command_context,
                      VulkanBufferView r32f, std::uint32_t width, std::uint32_t height) {
  constexpr const char*                   kOperation = "vulkan::raw::EncodeCfaClamp01";
  raw_core_kernels::CfaClampPushConstants push;
  push.image =
      RequireBufferRange(r32f, Texels(width, height) * kFloatBytes, kFloatBytes, kOperation, "CFA");
  push.width  = width;
  push.height = height;
  if (Texels(width, height) == 0) {
    return;
  }
  RecordKernelDispatch(backend, command_context, raw_core_kernels::CfaClamp01Descriptor(), {},
                       PushConstantBytes(push), Groups(width, height));
}

void EncodeBayerRcd(VulkanBackend& backend, VulkanCommandContext& command_context,
                    VulkanBufferView linear, const RcdPlanes& planes,
                    const BayerPattern2x2& pattern, std::uint32_t width, std::uint32_t height) {
  constexpr const char*                 kOperation  = "vulkan::raw::EncodeBayerRcd";
  const std::size_t                     plane_bytes = Texels(width, height) * kFloatBytes;
  debayer_rcd_kernels::RcdPushConstants push;
  push.raw           = RequireBufferRange(linear, plane_bytes, kFloatBytes, kOperation, "CFA");
  push.r             = RequireBufferRange(planes.r, plane_bytes, kFloatBytes, kOperation, "R");
  push.g             = RequireBufferRange(planes.g, plane_bytes, kFloatBytes, kOperation, "G");
  push.b             = RequireBufferRange(planes.b, plane_bytes, kFloatBytes, kOperation, "B");
  push.vh            = RequireBufferRange(planes.vh, plane_bytes, kFloatBytes, kOperation, "VH");
  push.pq            = RequireBufferRange(planes.pq, plane_bytes, kFloatBytes, kOperation, "PQ");
  push.params.width  = width;
  push.params.height = height;
  push.params.stride = width;
  for (int i = 0; i < 4; ++i) {
    push.params.rgb_fc[i] = static_cast<std::uint32_t>(pattern.rgb_fc[i]);
  }
  if (plane_bytes == 0) {
    return;
  }
  // The OpenCL order: each kernel reads what the kernels before it wrote; the dispatch recording
  // puts a barrier before each one.
  for (const auto kernel : debayer_rcd_kernels::kRcdKernels) {
    RecordKernelDispatch(backend, command_context, debayer_rcd_kernels::Descriptor(kernel), {},
                         PushConstantBytes(push), Groups(width, height));
  }
}

void EncodeXTrans(VulkanBackend& backend, VulkanCommandContext& command_context,
                  VulkanBufferView linear, VulkanBufferView green, VulkanBufferView rgba,
                  VulkanBufferView parameters, const XTransPattern6x6& pattern, std::uint32_t width,
                  std::uint32_t height, int passes) {
  constexpr const char*               kOperation = "vulkan::raw::EncodeXTrans";
  const std::size_t                   texels     = Texels(width, height);
  xtrans_kernels::XTransPushConstants push;
  push.raw   = RequireBufferRange(linear, texels * kFloatBytes, kFloatBytes, kOperation, "CFA");
  push.green = RequireBufferRange(green, texels * kFloatBytes, kFloatBytes, kOperation, "green");
  push.rgba  = RequireBufferRange(rgba, texels * kRgbaBytes, kRgbaBytes, kOperation, "RGBA");
  push.parameters = RequireBufferRange(parameters, kXTransParameterBytes, kRecordAlignment,
                                       kOperation, "parameters");
  if (texels == 0) {
    return;
  }
  // The OpenCL XTransParams of opencl_encode.cpp.
  xtrans_kernels::XTransParams record;
  record.width        = width;
  record.height       = height;
  record.tile_width   = 6;
  record.tile_height  = 6;
  record.passes       = static_cast<std::uint32_t>(std::max(passes, 1));
  record.green_radius = 3;
  record.rb_radius    = record.passes > 1 ? 4U : 3U;
  for (int i = 0; i < 36; ++i) {
    record.rgb_fc[i] = static_cast<std::uint32_t>(pattern.rgb_fc[i]);
  }
  UploadRecord(backend, command_context, parameters, record);
  RecordKernelDispatch(backend, command_context, xtrans_kernels::GreenDescriptor(), {},
                       PushConstantBytes(push), Groups(width, height));
  RecordKernelDispatch(backend, command_context, xtrans_kernels::RgbaDescriptor(), {},
                       PushConstantBytes(push), Groups(width, height));
}

void EncodePackPlanesCropInverseOrient(VulkanBackend&        backend,
                                       VulkanCommandContext& command_context, VulkanBufferView r,
                                       VulkanBufferView g, VulkanBufferView b,
                                       VulkanBackend::Texture2D& destination, RectI crop,
                                       std::uint32_t plane_width, std::uint32_t plane_height,
                                       std::span<const float, 4> cam_mul, int flip) {
  constexpr const char* kOperation  = "vulkan::raw::EncodePackPlanesCropInverseOrient";
  const std::size_t     plane_bytes = Texels(plane_width, plane_height) * kFloatBytes;
  raw_pack_kernels::PackPlanesPushConstants push;
  push.r      = RequireBufferRange(r, plane_bytes, kFloatBytes, kOperation, "R");
  push.g      = RequireBufferRange(g, plane_bytes, kFloatBytes, kOperation, "G");
  push.b      = RequireBufferRange(b, plane_bytes, kFloatBytes, kOperation, "B");
  push.params = PackParams(kOperation, destination, crop, plane_width, plane_height, cam_mul, flip);
  const VulkanImageBinding images[] = {{raw_pack_kernels::kDestinationBinding, &destination}};
  RecordKernelDispatch(backend, command_context, raw_pack_kernels::PackPlanesDescriptor(), images,
                       PushConstantBytes(push),
                       Groups(push.params.src_width, push.params.src_height));
}

void EncodeCopyRgbaCropInverseOrient(VulkanBackend& backend, VulkanCommandContext& command_context,
                                     VulkanBufferView source, VulkanBackend::Texture2D& destination,
                                     RectI crop, std::uint32_t source_width,
                                     std::uint32_t source_height, std::span<const float, 4> cam_mul,
                                     int flip) {
  constexpr const char* kOperation = "vulkan::raw::EncodeCopyRgbaCropInverseOrient";
  raw_pack_kernels::CopyRgbaPushConstants push;
  push.source = RequireBufferRange(source, Texels(source_width, source_height) * kRgbaBytes,
                                   kRgbaBytes, kOperation, "RGBA source");
  push.params =
      PackParams(kOperation, destination, crop, source_width, source_height, cam_mul, flip);
  const VulkanImageBinding images[] = {{raw_pack_kernels::kDestinationBinding, &destination}};
  RecordKernelDispatch(backend, command_context, raw_pack_kernels::CopyRgbaDescriptor(), images,
                       PushConstantBytes(push),
                       Groups(push.params.src_width, push.params.src_height));
}

void EncodeHighlightReconstruct(VulkanBackend& backend, VulkanCommandContext& command_context,
                                VulkanBufferView source, VulkanBufferView destination,
                                const HighlightScratch& scratch, std::span<const float, 4> cam_mul,
                                std::uint32_t width, std::uint32_t height) {
  constexpr const char* kOperation = "vulkan::raw::EncodeHighlightReconstruct";
  const std::size_t     rgba_bytes = Texels(width, height) * kRgbaBytes;
  const auto            addresses  = RequireHighlightScratch(scratch, width, height, kOperation);
  highlight_kernels::HighlightRgbaPushConstants push;
  push.source = RequireBufferRange(source, rgba_bytes, kRgbaBytes, kOperation, "RGBA source");
  push.destination =
      RequireBufferRange(destination, rgba_bytes, kRgbaBytes, kOperation, "RGBA destination");
  push.mask       = addresses.mask;
  push.dilated    = addresses.dilated;
  push.statistics = addresses.statistics;
  push.limits     = MakeHighlightLimits(cam_mul, width, height, width);
  if (rgba_bytes == 0) {
    return;
  }
  ClearHighlightStatistics(backend, command_context, scratch);
  // The OpenCL order; each kernel reads what the kernels before it wrote.
  for (const auto kernel : highlight_kernels::kRgbaKernels) {
    RecordKernelDispatch(backend, command_context, highlight_kernels::RgbaDescriptor(kernel), {},
                         PushConstantBytes(push), Groups(width, height));
  }
}

void EncodeHighlightReconstructPlanarAndPack(
    VulkanBackend& backend, VulkanCommandContext& command_context, VulkanBufferView r,
    VulkanBufferView g, VulkanBufferView b, VulkanBackend::Texture2D& destination,
    const HighlightScratch& scratch, std::span<const float, 4> cam_mul, RectI crop,
    std::uint32_t plane_width, std::uint32_t plane_height, int flip) {
  constexpr const char* kOperation  = "vulkan::raw::EncodeHighlightReconstructPlanarAndPack";
  const std::size_t     plane_bytes = Texels(plane_width, plane_height) * kFloatBytes;
  highlight_kernels::HighlightPlanarPackPushConstants pack;
  pack.r    = RequireBufferRange(r, plane_bytes, kFloatBytes, kOperation, "R");
  pack.g    = RequireBufferRange(g, plane_bytes, kFloatBytes, kOperation, "G");
  pack.b    = RequireBufferRange(b, plane_bytes, kFloatBytes, kOperation, "B");
  pack.pack = PackParams(kOperation, destination, crop, plane_width, plane_height, cam_mul, flip);
  const std::uint32_t crop_width  = pack.pack.src_width;
  const std::uint32_t crop_height = pack.pack.src_height;
  const auto addresses = RequireHighlightScratch(scratch, crop_width, crop_height, kOperation);
  pack.statistics      = addresses.statistics;
  pack.limits          = MakeHighlightLimits(cam_mul, crop_width, crop_height, plane_width);

  highlight_kernels::HighlightPlanarPushConstants planar;
  planar.r          = pack.r;
  planar.g          = pack.g;
  planar.b          = pack.b;
  planar.mask       = addresses.mask;
  planar.dilated    = addresses.dilated;
  planar.statistics = addresses.statistics;
  planar.limits     = pack.limits;
  planar.crop_x     = pack.pack.src_x;
  planar.crop_y     = pack.pack.src_y;

  // The dilation reads only the masks of the crop (a region of crop_width texels in a row).
  highlight_kernels::HighlightRgbaPushConstants dilate;
  dilate.mask       = addresses.mask;
  dilate.dilated    = addresses.dilated;
  dilate.statistics = addresses.statistics;
  dilate.limits     = pack.limits;

  const auto groups = Groups(crop_width, crop_height);
  ClearHighlightStatistics(backend, command_context, scratch);
  RecordKernelDispatch(backend, command_context,
                       highlight_kernels::PlanarDescriptor(highlight_kernels::kBuildMaskPlanar), {},
                       PushConstantBytes(planar), groups);
  RecordKernelDispatch(backend, command_context,
                       highlight_kernels::RgbaDescriptor(highlight_kernels::kDilateMask), {},
                       PushConstantBytes(dilate), groups);
  RecordKernelDispatch(
      backend, command_context,
      highlight_kernels::PlanarDescriptor(highlight_kernels::kChrominanceContribPlanar), {},
      PushConstantBytes(planar), groups);
  const VulkanImageBinding images[] = {{highlight_kernels::kDestinationBinding, &destination}};
  RecordKernelDispatch(backend, command_context,
                       highlight_kernels::ReconstructPlanarPackDescriptor(), images,
                       PushConstantBytes(pack), groups);
}

}  // namespace alcedo::vulkan::raw

#endif  // HAVE_VULKAN
