//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#ifdef HAVE_VULKAN

#include "edit/runtime/vulkan/vulkan_develop_pass.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>

#include "decoders/processor/operators/gpu/vulkan_raw_encode.hpp"
#include "edit/geometry/types.hpp"
#include "edit/graph/develop_node_model.hpp"
#include "edit/graph/graph_ids.hpp"
#include "edit/runtime/develop_demosaic.hpp"
#include "edit/runtime/lens/lens_calibration_resolver.hpp"
#include "edit/runtime/lens/vulkan/vulkan_lens_calib.hpp"
#include "edit/runtime/texture_format.hpp"
#include "edit/runtime/vulkan/vulkan_buffer_view.hpp"
#include "edit/runtime/vulkan/vulkan_dng_warp.hpp"
#include "utils/diagnostics/preview_performance.hpp"

namespace alcedo {
namespace {

constexpr const char* kOperation                = "ExecuteVulkanDevelop";

/// The parameter records of one Develop share one buffer; each record starts at a multiple of this.
constexpr std::size_t kParameterRecordAlignment = 256;

auto                  Fail(const std::string& message) -> std::runtime_error {
  return std::runtime_error(std::string(kOperation) + ": " + message);
}

auto AlignUp(std::size_t value, std::size_t alignment) -> std::size_t {
  return (value + alignment - 1) / alignment * alignment;
}

auto AcquireRgba(VulkanRenderWorkspace& workspace, const GraphValueId& id, std::uint32_t width,
                 std::uint32_t height) -> ResourceLease<VulkanBackend>& {
  return workspace.AcquireImageForWrite(id, {width, height, TextureFormat::Rgba32f});
}

auto CropOrFull(const PreparedRawInput& input, std::uint32_t width, std::uint32_t height) -> RectI {
  if (input.demosaic_output_crop.width > 0 && input.demosaic_output_crop.height > 0) {
    return input.demosaic_output_crop;
  }
  return RectI{0, 0, static_cast<std::int32_t>(width), static_cast<std::int32_t>(height)};
}

auto PlaneBytes(std::uint32_t width, std::uint32_t height) -> std::size_t {
  return static_cast<std::size_t>(width) * height * sizeof(float);
}

/// The buffers of a highlight reconstruction of a @p width x @p height region.
struct HighlightBuffers {
  VulkanBackend::Buffer mask;
  VulkanBackend::Buffer dilated;
  VulkanBackend::Buffer statistics;

  [[nodiscard]] auto    Scratch() -> vulkan::raw::HighlightScratch {
    return {.mask = {&mask, 0}, .dilated = {&dilated, 0}, .statistics = {&statistics, 0}};
  }
};

auto CreateHighlightBuffers(VulkanBackend& backend, std::uint32_t width, std::uint32_t height)
    -> HighlightBuffers {
  const std::size_t mask_bytes = vulkan::raw::HighlightMaskBytes(width, height);
  return HighlightBuffers{
      .mask       = backend.CreateBuffer(mask_bytes),
      .dilated    = backend.CreateBuffer(mask_bytes),
      .statistics = backend.CreateBuffer(vulkan::raw::kHighlightStatisticsBytes),
  };
}

/// The checks of the request that come before a command or an allocation.
void RequireSupportedInput(VulkanRenderDevice& device, const ExecutionPlan& plan,
                           const PreparedRawInput& input, const PipelineDocument& document) {
  if (!device.Workspace().IsRendering()) {
    throw Fail("BeginRender has not been called");
  }
  const auto* develop = document.Develop();
  if (develop == nullptr) {
    throw Fail("missing develop node");
  }
  if (input.input_kind == RawInputKind::RasterRgb) {
    throw Fail(
        "a raster input needs the raster linearization of phase VK7; the Vulkan backend has no "
        "raster input yet (#1036)");
  }
  const auto width  = input.host_extent.width;
  const auto height = input.host_extent.height;
  if (input.input_kind == RawInputKind::DebayeredRgb ||
      plan.source.kind == DevelopInputKind::DirectRgb) {
    if (input.pixels.format != HostPixelFormat::F32Rgba ||
        input.pixels.stride_bytes != width * 4 * sizeof(float)) {
      throw Fail("expected tightly packed RGB input");
    }
    return;
  }
  if (width == 0 || height == 0) {
    throw Fail("empty CFA");
  }
  if (input.pixels.stride_bytes != width * sizeof(std::uint16_t)) {
    throw Fail("CFA plane must be tightly packed");
  }
  const auto method = ResolveDevelopDemosaicMethod(develop->Params().Params(),
                                                   input.cfa_pattern.kind, input.downsample_passes);
  if (method == RawDemosaicMethod::NeuralEngine) {
    throw Fail(
        "the Neural Engine demosaic is not available on the Vulkan backend (#1036, decision A5); "
        "no other demosaic method runs in its place");
  }
}

/// `DebayeredRgb` and `DirectRgb`: the upload is linearized in place, then optionally
/// reconstructed, then cropped and oriented into @p decoded.
void EncodeRgbInput(VulkanRenderDevice& device, const PreparedRawInput& input, bool hlr,
                    VulkanBackend::Texture2D& decoded) {
  auto&      backend = device.Workspace().Device();
  auto&      context = device.CommandContext();
  const auto width   = input.host_extent.width;
  const auto height  = input.host_extent.height;
  auto       source  = backend.CreateBuffer(input.pixels.ByteCount());
  {
    diag::PreviewSubStageInterval upload(diag::PreviewSubStageKind::Upload);
    backend.UploadBufferRange(source, 0, input.pixels.Span(), context);
  }
  {
    diag::PreviewSubStageInterval linearize(diag::PreviewSubStageKind::Linearize);
    vulkan::raw::EncodeLinearizeRgb(backend, context, {&source, 0}, width, height,
                                    input.rgb_linearization.value_or(RawRgbLinearizationParams{}));
  }
  const RectI crop = CropOrFull(input, width, height);
  if (hlr && input.rgb_linearization.has_value()) {
    auto reconstructed = backend.CreateBuffer(input.pixels.ByteCount());
    auto highlight     = CreateHighlightBuffers(backend, width, height);
    vulkan::raw::EncodeHighlightReconstruct(backend, context, {&source, 0}, {&reconstructed, 0},
                                            highlight.Scratch(), input.linearization.cam_mul, width,
                                            height);
    vulkan::raw::EncodeCopyRgbaCropInverseOrient(backend, context, {&reconstructed, 0}, decoded,
                                                 crop, width, height, input.linearization.cam_mul,
                                                 input.sensor.orientation_flip);
    return;
  }
  const std::array<float, 4> identity{1.0F, 1.0F, 1.0F, 1.0F};
  vulkan::raw::EncodeCopyRgbaCropInverseOrient(
      backend, context, {&source, 0}, decoded, crop, width, height,
      input.rgb_linearization.has_value() ? std::span<const float, 4>(input.linearization.cam_mul)
                                          : std::span<const float, 4>(identity),
      input.sensor.orientation_flip);
}

/// CFA input with the legacy demosaic (RCD for Bayer, the X-Trans interpolation of VK4).
void EncodeCfaInput(VulkanRenderDevice& device, const PreparedRawInput& input, bool hlr,
                    VulkanBackend::Texture2D& decoded) {
  auto&      backend     = device.Workspace().Device();
  auto&      context     = device.CommandContext();
  const auto width       = input.host_extent.width;
  const auto height      = input.host_extent.height;
  const auto plane_bytes = PlaneBytes(width, height);
  const auto xtrans_parameter_offset =
      AlignUp(vulkan::raw::kToLinearRefParameterBytes, kParameterRecordAlignment);

  auto cfa    = backend.CreateBuffer(input.pixels.ByteCount());
  auto linear = backend.CreateBuffer(plane_bytes);
  auto parameters =
      backend.CreateBuffer(xtrans_parameter_offset +
                           AlignUp(vulkan::raw::kXTransParameterBytes, kParameterRecordAlignment));
  {
    diag::PreviewSubStageInterval upload(diag::PreviewSubStageKind::Upload);
    backend.UploadBufferRange(cfa, 0, input.pixels.Span(), context);
  }
  {
    diag::PreviewSubStageInterval linearize(diag::PreviewSubStageKind::Linearize);
    vulkan::raw::EncodeToLinearRef(backend, context, {&cfa, 0}, {&linear, 0}, {&parameters, 0},
                                   width, height, input.linearization, input.cfa_pattern);
  }
  if (!hlr) {
    diag::PreviewSubStageInterval clamp(diag::PreviewSubStageKind::CfaClamp);
    vulkan::raw::EncodeCfaClamp01(backend, context, {&linear, 0}, width, height);
  }
  cfa.Reset();

  diag::PreviewPerformance::NoteDevelopLayout(diag::PreviewDevelopLayout::FullFrame);
  diag::PreviewSubStageInterval demosaic(diag::PreviewSubStageKind::Demosaic);
  const RectI                   crop = CropOrFull(input, width, height);
  if (input.cfa_pattern.kind == RawCfaKind::XTrans6x6) {
    auto green = backend.CreateBuffer(plane_bytes);
    auto rgba  = backend.CreateBuffer(plane_bytes * 4);
    vulkan::raw::EncodeXTrans(backend, context, {&linear, 0}, {&green, 0}, {&rgba, 0},
                              {&parameters, static_cast<std::uint32_t>(xtrans_parameter_offset)},
                              input.cfa_pattern.xtrans_pattern, width, height,
                              input.downsample_passes == 0 ? 3 : 1);
    linear.Reset();
    green.Reset();
    if (hlr) {
      auto reconstructed = backend.CreateBuffer(plane_bytes * 4);
      auto highlight     = CreateHighlightBuffers(backend, width, height);
      vulkan::raw::EncodeHighlightReconstruct(backend, context, {&rgba, 0}, {&reconstructed, 0},
                                              highlight.Scratch(), input.linearization.cam_mul,
                                              width, height);
      vulkan::raw::EncodeCopyRgbaCropInverseOrient(backend, context, {&reconstructed, 0}, decoded,
                                                   crop, width, height, input.linearization.cam_mul,
                                                   input.sensor.orientation_flip);
      return;
    }
    vulkan::raw::EncodeCopyRgbaCropInverseOrient(backend, context, {&rgba, 0}, decoded, crop, width,
                                                 height, input.linearization.cam_mul,
                                                 input.sensor.orientation_flip);
    return;
  }

  auto r  = backend.CreateBuffer(plane_bytes);
  auto g  = backend.CreateBuffer(plane_bytes);
  auto b  = backend.CreateBuffer(plane_bytes);
  auto vh = backend.CreateBuffer(plane_bytes);
  auto pq = backend.CreateBuffer(plane_bytes);
  vulkan::raw::EncodeBayerRcd(
      backend, context, {&linear, 0},
      vulkan::raw::RcdPlanes{
          .r = {&r, 0}, .g = {&g, 0}, .b = {&b, 0}, .vh = {&vh, 0}, .pq = {&pq, 0}},
      input.cfa_pattern.bayer_pattern, width, height);
  linear.Reset();
  vh.Reset();
  pq.Reset();
  if (hlr) {
    auto highlight = CreateHighlightBuffers(backend, static_cast<std::uint32_t>(crop.width),
                                            static_cast<std::uint32_t>(crop.height));
    vulkan::raw::EncodeHighlightReconstructPlanarAndPack(
        backend, context, {&r, 0}, {&g, 0}, {&b, 0}, decoded, highlight.Scratch(),
        input.linearization.cam_mul, crop, width, height, input.sensor.orientation_flip);
    return;
  }
  vulkan::raw::EncodePackPlanesCropInverseOrient(
      backend, context, {&r, 0}, {&g, 0}, {&b, 0}, decoded, crop, width, height,
      input.linearization.cam_mul, input.sensor.orientation_flip);
}

/// `GpuPassKind::Lens`: the lens calibration of `develop.sensor_linear` in place, recorded into the
/// open recording (the OpenCL form copies to a buffer and waits for the queue).
void EncodeLensCalibration(VulkanRenderDevice& device, const ExecutionPlan& plan,
                           const PreparedRawInput& input, const DevelopPayload& develop_params) {
  const auto params = LensCalibrationResolver::Resolve(develop_params, input.color_context,
                                                       plan.source.develop_output_extent,
                                                       input.dng_warp_rectilinear.has_value());
  if (!params.has_value()) {
    return;
  }
  auto& workspace = device.Workspace();
  auto* sensor    = workspace.Images().Find(plan.sensor_linear_output);
  if (sensor == nullptr || sensor->Empty()) {
    throw Fail("missing develop.sensor_linear for the lens calibration");
  }
  auto&      image  = sensor->Texture();
  const auto width  = image.Width();
  const auto height = image.Height();
  const auto steps  = vulkan::lens::ResolveLensCalibrationSteps(*params, width, height);
  if (!steps.Any()) {
    return;
  }
  auto&                                   backend = workspace.Device();
  std::optional<VulkanBackend::Texture2D> warped;
  std::optional<VulkanBackend::Texture2D> cropped;
  if (steps.warp) {
    warped.emplace(backend.CreateTexture2D(width, height, TextureFormat::Rgba32f));
  }
  if (steps.rectangle_crop.has_value()) {
    cropped.emplace(backend.CreateTexture2D(
        static_cast<std::uint32_t>(steps.rectangle_crop->width),
        static_cast<std::uint32_t>(steps.rectangle_crop->height), TextureFormat::Rgba32f));
  }
  auto parameters = backend.CreateBuffer(vulkan::lens::kLensCalibParameterBytes);
  vulkan::lens::EncodeLensCalibration(
      backend, device.CommandContext(), image,
      vulkan::lens::LensCalibrationScratch{.warped  = warped ? &*warped : nullptr,
                                           .cropped = cropped ? &*cropped : nullptr},
      {&parameters, 0}, *params);
}

}  // namespace

void ExecuteVulkanDevelop(VulkanRenderDevice& device, const ExecutionPlan& plan,
                          const PreparedRawInput& input, const PipelineDocument& document) {
  RequireSupportedInput(device, plan, input, document);

  auto&              workspace   = device.Workspace();
  const auto         flags       = document.Develop()->Params().Params();
  const bool         hlr         = flags.highlights_reconstruct;
  const auto         out_w       = plan.source.develop_output_extent.width;
  const auto         out_h       = plan.source.develop_output_extent.height;
  const GraphValueId sensor_id   = plan.sensor_linear_output;
  const GraphValueId demosaic_id = input.dng_warp_rectilinear.has_value()
                                       ? GraphValueId{NodeId{"develop"}, PortId{"sensor_unwarped"}}
                                       : sensor_id;

  auto&              decoded     = AcquireRgba(workspace, demosaic_id, out_w, out_h);
  if (input.input_kind == RawInputKind::DebayeredRgb ||
      plan.source.kind == DevelopInputKind::DirectRgb) {
    EncodeRgbInput(device, input, hlr, decoded.Texture());
  } else {
    EncodeCfaInput(device, input, hlr, decoded.Texture());
  }

  if (input.dng_warp_rectilinear.has_value()) {
    auto& warped = AcquireRgba(workspace, sensor_id, out_w, out_h);
    // The acquire can move the leases of the cache: find the source again.
    auto* source = workspace.Images().Find(demosaic_id);
    if (source == nullptr) {
      throw Fail("DNG warp source was lost");
    }
    diag::PreviewSubStageInterval warp(diag::PreviewSubStageKind::DngWarp);
    vulkan::geometry::EncodeWarpRectilinear(workspace.Device(), device.CommandContext(),
                                            source->Texture(), warped.Texture(),
                                            *input.dng_warp_rectilinear);
  }

  if (plan.Contains(GpuPassKind::Lens)) {
    diag::PreviewSubStageInterval lens(diag::PreviewSubStageKind::Lens);
    EncodeLensCalibration(device, plan, input, flags);
  }
}

}  // namespace alcedo

#endif  // HAVE_VULKAN
