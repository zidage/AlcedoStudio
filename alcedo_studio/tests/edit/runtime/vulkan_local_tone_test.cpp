//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// The Local Laplacian stage of the Color Grade (Shadows, Highlights) on the Vulkan backend
// (#1036, phase VK10): the full frame, a region of interest, a slider edit, the mix, the mask, and
// the curve input histogram against the same passes of the reference backend on the same GPU
// (decision B9: OpenCL on Windows, Metal on macOS); the canonical planes, the steady state, and
// the recording rule of the pass.

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

#include "../graph/grade_owned_mask_support.hpp"
#include "../graph/test_camera_profile.hpp"
#include "../input/prepared_raw_test_support.hpp"
#include "curve_input_histogram_test_support.hpp"
#include "edit/graph/adjustment_ownership.hpp"
#include "edit/graph/color_grade_node_model.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/input/raw_input_loader.hpp"
#include "edit/mask/mask_model.hpp"
#include "edit/operators/models/scalar_operator_model.hpp"
#include "edit/runtime/curve_input_histogram.hpp"
#include "edit/runtime/graph_compiler.hpp"
#include "edit/runtime/local_tone_cache_ids.hpp"
#include "edit/runtime/local_tone_mapping.hpp"
#include "edit/runtime/texture_format.hpp"
#include "edit/runtime/vulkan/vulkan_develop_pass.hpp"
#include "edit/runtime/vulkan/vulkan_geometry_camera_pass.hpp"
#include "edit/runtime/vulkan/vulkan_local_tone_pass.hpp"
#include "edit/runtime/vulkan/vulkan_pass_encoder.hpp"
#include "edit/runtime/vulkan/vulkan_primary_grade_pass.hpp"
#include "multi_grade_runtime_test_support.hpp"
#include "vulkan_reference_backend_support.hpp"
#include "vulkan_workspace_test_support.hpp"

namespace alcedo {
namespace {

using namespace vulkan_reference;

/// The largest relative difference of the local tone output from the reference backend (ACEScc):
/// the neighborhood tolerance of phase VK9 (measured 5.4e-7 on the RTX 3080, Iris Xe, and the
/// Apple M4).
constexpr double        kLocalToneTolerance = 1.0e-5;

constexpr std::uint32_t kWidth              = 256;
constexpr std::uint32_t kHeight             = 192;

/// Scene-linear camera RGB with large tonal regions and fine detail: a slow gradient from 2^-8
/// to 2^2, a bright window, a dark block, a texture of 12 % on every texel, and hues that change
/// along y. The Local Laplacian stage needs large regions to move and detail to keep.
auto                    MakeTonePlane(std::uint32_t width, std::uint32_t height) -> HostImagePlane {
  HostImagePlane plane;
  plane.extent       = {width, height};
  plane.stride_bytes = width * 16U;
  plane.format       = HostPixelFormat::F32Rgba;
  auto  storage = std::shared_ptr<std::byte>(new std::byte[plane.ByteCount()],
                                                                [](std::byte* pointer) { delete[] pointer; });
  auto* pixels  = reinterpret_cast<float*>(storage.get());
  for (std::uint32_t y = 0; y < height; ++y) {
    const float v   = static_cast<float>(y) / static_cast<float>(height - 1);
    const float hue = 6.2831853F * v;
    for (std::uint32_t x = 0; x < width; ++x) {
      const float u    = static_cast<float>(x) / static_cast<float>(width - 1);
      float       stop = -8.0F + 7.0F * u + 3.0F * v;
      if (u > 0.55F && u < 0.85F && v > 0.15F && v < 0.45F) {
        stop = 2.0F;  // a bright window
      } else if (u > 0.1F && u < 0.35F && v > 0.6F && v < 0.9F) {
        stop = -7.0F;  // a dark block
      }
      const float texture = 1.0F + 0.12F * std::sin(0.9F * static_cast<float>(x)) *
                                       std::cos(0.7F * static_cast<float>(y));
      const float level = std::exp2(stop) * texture;
      const auto  index = (static_cast<std::size_t>(y) * width + x) * 4;
      pixels[index + 0] = level * (1.0F + 0.35F * std::cos(hue));
      pixels[index + 1] = level * (1.0F + 0.35F * std::cos(hue - 2.0943951F));
      pixels[index + 2] = level * (1.0F + 0.35F * std::cos(hue + 2.0943951F));
      pixels[index + 3] = 1.0F;
    }
  }
  plane.bytes = std::const_pointer_cast<const std::byte>(storage);
  return plane;
}

auto MakeToneInput(std::uint32_t width = kWidth, std::uint32_t height = kHeight)
    -> PreparedRawInput {
  return RawInputLoader::FromDirectRgb(MakeTonePlane(width, height),
                                       gpu_dag_test::FullSensor(width, height));
}

auto Compile(const PipelineDocument& document, const PreparedRawInput& input,
             const RenderRequest& request = RenderRequest{}) -> ExecutionPlan {
  return GraphCompiler::Compile(document, input.CompileSource(), request);
}

/// The left-center half of the frame at its own pixel size: a region of interest.
auto RegionRequest(std::uint32_t width, std::uint32_t height) -> RenderRequest {
  RenderRequest request;
  request.view.visible_rect_in_edit_space = NormalizedRect{0.25F, 0.0F, 0.5F, 1.0F};
  request.view.viewport_extent            = Extent2D{width / 2, height};
  return request;
}

/// An R8 mask with a ramp along x and a step along y: coverage 0, partial, and 1.
auto MakeMaskBytes(std::uint32_t width, std::uint32_t height) -> std::vector<std::byte> {
  std::vector<std::byte> bytes(static_cast<std::size_t>(width) * height);
  for (std::uint32_t y = 0; y < height; ++y) {
    for (std::uint32_t x = 0; x < width; ++x) {
      const auto ramp = static_cast<std::uint32_t>(255U * x / (width - 1));
      bytes[static_cast<std::size_t>(y) * width + x] =
          static_cast<std::byte>(y < height / 4 ? 0U : (y >= 3 * height / 4 ? 255U : ramp));
    }
  }
  return bytes;
}

auto IdentityDocument() -> PipelineDocument {
  return multi_grade_test::MakeIdentityGradeDocument();
}

void SetShadows(PipelineDocument& document, float value) {
  multi_grade_test::GradeAdjustment<ShadowsModel>(document, document.PrimaryGrade()->Id(),
                                                  type_ids::Shadows())
      .SetValue(value);
}

void SetHighlights(PipelineDocument& document, float value) {
  multi_grade_test::GradeAdjustment<HighlightsModel>(document, document.PrimaryGrade()->Id(),
                                                     type_ids::Highlights())
      .SetValue(value);
}

/// Removes the color adjustments of the primary Grade, so the Local Laplacian stage is its last
/// write (the OpenCL test helper of the same name).
void RemoveColorAdjustments(PipelineDocument& document) {
  auto* grade = document.PrimaryGrade();
  ASSERT_NE(grade, nullptr);
  for (const auto& type : ColorGradeCompileOrder()) {
    if (ColorGradeCompileGroupOf(type) != ColorGradeCompileGroup::Color) {
      continue;
    }
    const auto* id = grade->FindAdjustmentIdByType(type);
    ASSERT_NE(id, nullptr) << type.Text();
    grade->RemoveAdjustment(AdjustmentInstanceId{*id});
  }
}

/// Adds a radial mask to the primary Grade; the render gives its output as an R8 image, so the
/// comparison holds the mask the same on both backends (VulkanMaskTest compares MaskEvaluate).
void AddMask(PipelineDocument& document) {
  MaskModel mask;
  mask.id     = MaskId{"mask.radial"};
  mask.source = RadialMaskSource{};
  (void)grade_mask_test::AddMask(*document.PrimaryGrade(), std::move(mask));
  document.MarkTopologyChanged();
}

/// The images and the Grade outcome of one render.
struct ToneRender {
  std::vector<float>       input;
  std::vector<float>       output;
  VulkanPrimaryGradeResult result{};
};

/// Develop, GeometryResample, CameraToAp1, then every Color Grade on @p device; publishes the
/// results. With @p mask, the mask output of the first Grade is that R8 image.
template <class Device>
auto Render(Device& device, const ExecutionPlan& plan, const PreparedRawInput& input,
            const PipelineDocument& document, const std::vector<std::byte>* mask = nullptr,
            CurveInputHistogramRecord* histogram = nullptr) -> ToneRender {
  ToneRender        render;
  FrameSceneBinding binding{};
  device.BeginRender();
  try {
    if constexpr (std::is_same_v<Device, VulkanRenderDevice>) {
      ExecuteVulkanDevelop(device, plan, input, document);
      ExecuteVulkanGeometryResample(device, plan);
      ExecuteVulkanCameraColor(device, plan, document);
    } else {
      ExecuteReferenceDevelop(device, plan, input, document);
      ExecuteReferenceGeometryResample(device, plan);
      ExecuteReferenceCameraColor(device, plan, document);
    }
    if (mask != nullptr) {
      const auto& extent = plan.geometry.render_extent;
      auto&       lease  = device.Workspace().AcquireImageForWrite(
          plan.FirstGrade()->mask_output, {extent.width, extent.height, TextureFormat::R8});
      device.Workspace().Device().UploadTexture2D(lease.Texture(), *mask, device.CommandContext());
    }
    if constexpr (std::is_same_v<Device, VulkanRenderDevice>) {
      render.result = ExecuteVulkanPrimaryGrade(device, plan, input, document, histogram);
      binding       = render.result.output_binding;
    } else {
      binding = ExecuteReferencePrimaryGrade(device, plan, input, document, histogram);
    }
    device.EndRender();
    device.WaitIdle();
  } catch (...) {
    device.CancelRender();
    throw;
  }
  render.input  = DownloadImage(device, plan.develop_output);
  render.output = DownloadScene(device, binding);
  device.PublishResults();
  return render;
}

/// The largest absolute change of the output from the input: a case tests the stage, not an
/// alias of the input.
auto LargestChange(const ToneRender& render) -> double {
  double change = 0.0;
  for (std::size_t i = 0; i < render.output.size(); ++i) {
    change = std::max(change, std::abs(static_cast<double>(render.output[i]) - render.input[i]));
  }
  return change;
}

class VulkanLocalToneTest : public vulkan_workspace_test::VulkanWorkspaceFixture {
 protected:
  void SetUp() override {
    VulkanWorkspaceFixture::SetUp();
    const std::string reason = PrepareReferenceBackend();
    ASSERT_TRUE(reason.empty()) << reason;
  }
};

TEST_F(VulkanLocalToneTest, ShadowsAndHighlightsEqualTheReferenceBackend) {
  struct Case {
    const char*   name;
    float         shadows;
    float         highlights;
    std::uint32_t width;
    std::uint32_t height;
  };
  const std::vector<Case> cases = {
      {"Shadows +80", 80.0F, 0.0F, kWidth, kHeight},
      {"Shadows -60", -60.0F, 0.0F, kWidth, kHeight},
      {"Highlights -100", 0.0F, -100.0F, kWidth, kHeight},
      {"Highlights +50", 0.0F, 50.0F, kWidth, kHeight},
      {"Shadows +150, Highlights -150 (the slider ends)", 150.0F, -150.0F, kWidth, kHeight},
      {"Shadows +45, Highlights -70 on 203 x 157", 45.0F, -70.0F, 203, 157},
  };
  for (const auto& c : cases) {
    const auto input    = MakeToneInput(c.width, c.height);
    auto       document = IdentityDocument();
    SetShadows(document, c.shadows);
    SetHighlights(document, c.highlights);
    const auto            plan = Compile(document, input);
    VulkanRenderDevice    vulkan_device;
    ReferenceRenderDevice reference_device;
    const auto            vulkan    = Render(vulkan_device, plan, input, document);
    const auto            reference = Render(reference_device, plan, input, document);
    EXPECT_EQ(vulkan.result.local_tone_pass_count, 1U) << c.name;
    EXPECT_TRUE(vulkan.result.local_tone_rebuilt_reference) << c.name;
    EXPECT_FALSE(vulkan.result.local_tone_sampled_canonical_reference) << c.name;
    EXPECT_GT(vulkan.result.local_tone_transient_bytes, 0U) << c.name;
    ExpectEqualToReference(c.name, reference.output, vulkan.output,
                           plan.geometry.render_extent.width, Uniform(kLocalToneTolerance));
    EXPECT_GT(LargestChange(vulkan), 1.0e-2) << c.name << ": the stage left its input";
  }
}

TEST_F(VulkanLocalToneTest, RegionOfInterestEqualsTheReferenceBackend) {
  const auto input    = MakeToneInput();
  auto       document = IdentityDocument();
  SetShadows(document, 75.0F);
  SetHighlights(document, -40.0F);
  const auto full_plan   = Compile(document, input);
  const auto region_plan = Compile(document, input, RegionRequest(kWidth, kHeight));
  ASSERT_EQ(region_plan.geometry.render_extent.width, kWidth / 2);
  ASSERT_NE(region_plan.geometry.render_extent.width,
            region_plan.geometry.full_reference_extent.width);

  // After a full frame, the region samples the canonical planes of the full frame.
  {
    VulkanRenderDevice    vulkan_device;
    ReferenceRenderDevice reference_device;
    const auto            vulkan_full = Render(vulkan_device, full_plan, input, document);
    (void)Render(reference_device, full_plan, input, document);
    ASSERT_TRUE(vulkan_full.result.local_tone_rebuilt_reference);
    const auto vulkan    = Render(vulkan_device, region_plan, input, document);
    const auto reference = Render(reference_device, region_plan, input, document);
    EXPECT_FALSE(vulkan.result.local_tone_rebuilt_reference);
    EXPECT_TRUE(vulkan.result.local_tone_sampled_canonical_reference);
    EXPECT_EQ(vulkan.result.local_tone_reference_resource_id,
              vulkan_full.result.local_tone_reference_resource_id);
    ExpectEqualToReference("region after a full frame", reference.output, vulkan.output,
                           region_plan.geometry.render_extent.width, Uniform(kLocalToneTolerance));
    EXPECT_GT(LargestChange(vulkan), 1.0e-2);
  }
  // Without a canonical plane, the region builds planes of its own and keeps none.
  {
    VulkanRenderDevice    vulkan_device;
    ReferenceRenderDevice reference_device;
    const auto            vulkan    = Render(vulkan_device, region_plan, input, document);
    const auto            reference = Render(reference_device, region_plan, input, document);
    EXPECT_TRUE(vulkan.result.local_tone_rebuilt_reference);
    EXPECT_FALSE(vulkan.result.local_tone_sampled_canonical_reference);
    EXPECT_EQ(vulkan.result.local_tone_reference_resource_id, 0U);
    EXPECT_EQ(
        vulkan_device.Workspace().Images().Find(LocalToneSourceId(document.PrimaryGrade()->Id())),
        nullptr);
    ExpectEqualToReference("region without a canonical plane", reference.output, vulkan.output,
                           region_plan.geometry.render_extent.width, Uniform(kLocalToneTolerance));
    EXPECT_GT(LargestChange(vulkan), 1.0e-2);
  }
}

TEST_F(VulkanLocalToneTest, SliderEditReusesTheCanonicalSourceAndEqualsTheReferenceBackend) {
  const auto input    = MakeToneInput();
  auto       document = IdentityDocument();
  SetShadows(document, 25.0F);
  VulkanRenderDevice    vulkan_device;
  ReferenceRenderDevice reference_device;
  const auto            first = Render(vulkan_device, Compile(document, input), input, document);
  (void)Render(reference_device, Compile(document, input), input, document);
  const auto  grade_id   = document.PrimaryGrade()->Id();
  const auto& images     = vulkan_device.Workspace().Images();
  const auto  source_rev = images.PublishedRevision(LocalToneSourceId(grade_id));
  const auto  result_rev = images.PublishedRevision(LocalToneResultId(grade_id));
  ASSERT_TRUE(first.result.local_tone_rebuilt_reference);
  ASSERT_NE(source_rev, 0U);

  SetShadows(document, 70.0F);
  const auto plan      = Compile(document, input);
  const auto vulkan    = Render(vulkan_device, plan, input, document);
  const auto reference = Render(reference_device, plan, input, document);
  EXPECT_TRUE(vulkan.result.local_tone_rebuilt_reference);
  EXPECT_FALSE(vulkan.result.local_tone_sampled_canonical_reference);
  EXPECT_EQ(vulkan.result.local_tone_reference_resource_id,
            first.result.local_tone_reference_resource_id);
  EXPECT_EQ(images.PublishedRevision(LocalToneSourceId(grade_id)), source_rev);
  EXPECT_NE(images.PublishedRevision(LocalToneResultId(grade_id)), result_rev);
  ExpectEqualToReference("Shadows 25 -> 70", reference.output, vulkan.output,
                         plan.geometry.render_extent.width, Uniform(kLocalToneTolerance));
}

TEST_F(VulkanLocalToneTest, MixMaskAndHistogramEqualTheReferenceBackend) {
  const auto input = MakeToneInput();
  {
    auto document = IdentityDocument();
    SetShadows(document, 90.0F);
    document.PrimaryGrade()->SetMix(0.6F);
    const auto            plan = Compile(document, input);
    VulkanRenderDevice    vulkan_device;
    ReferenceRenderDevice reference_device;
    ExpectEqualToReference("mix 0.6", Render(reference_device, plan, input, document).output,
                           Render(vulkan_device, plan, input, document).output, kWidth,
                           Uniform(kLocalToneTolerance));
  }
  {
    auto document = IdentityDocument();
    SetHighlights(document, -120.0F);
    document.PrimaryGrade()->SetMix(0.8F);
    AddMask(document);
    const auto plan = Compile(document, input);
    ASSERT_NE(plan.FirstGrade(), nullptr);
    ASSERT_TRUE(plan.FirstGrade()->mask_stack.has_value());
    const auto            bytes = MakeMaskBytes(kWidth, kHeight);
    VulkanRenderDevice    vulkan_device;
    ReferenceRenderDevice reference_device;
    ExpectEqualToReference("mix 0.8 with a mask",
                           Render(reference_device, plan, input, document, &bytes).output,
                           Render(vulkan_device, plan, input, document, &bytes).output, kWidth,
                           Uniform(kLocalToneTolerance));
  }
  {
    // A final Local Laplacian stage with a histogram writes the unmixed output, which the
    // histogram reads, and mixes in a pass without commands.
    auto document = IdentityDocument();
    RemoveColorAdjustments(document);
    SetShadows(document, 60.0F);
    VulkanRenderDevice vulkan_device;
    const auto         unmixed = Render(vulkan_device, Compile(document, input), input, document);
    document.PrimaryGrade()->SetMix(0.5F);
    const auto                plan  = Compile(document, input);
    const auto                plain = Render(vulkan_device, plan, input, document);
    CurveInputHistogramRecord record{document.PrimaryGrade()->Id()};
    const auto            vulkan = Render(vulkan_device, plan, input, document, nullptr, &record);
    ReferenceRenderDevice reference_device;
    const auto            reference = Render(reference_device, plan, input, document);
    ASSERT_TRUE(record.recorded);
    EXPECT_EQ(vulkan.result.local_tone_pass_count, 1U);
    EXPECT_EQ(vulkan.result.pointwise_dispatch_count, plain.result.pointwise_dispatch_count + 1U);
    ExpectEqualToReference("the histogram render against the render without it", plain.output,
                           vulkan.output, kWidth, Uniform(1.0e-6), "the render without it");
    struct Rgb {
      float r = 0.0F;
      float g = 0.0F;
      float b = 0.0F;
    };
    std::vector<Rgb> stage(unmixed.output.size() / 4);
    for (std::size_t i = 0; i < stage.size(); ++i) {
      stage[i] = {unmixed.output[i * 4], unmixed.output[i * 4 + 1], unmixed.output[i * 4 + 2]};
    }
    EXPECT_EQ(ReadCurveInputHistogramCounts(vulkan_device, record.grade_id),
              curve_histogram_test::ReferenceCounts(stage, kWidth, kHeight));
    ExpectEqualToReference("mix 0.5 with a histogram", reference.output, vulkan.output, kWidth,
                           Uniform(kLocalToneTolerance));
  }
}

TEST_F(VulkanLocalToneTest, CanonicalPlanesArePublishedOnlyAfterTheRender) {
  const auto input    = MakeToneInput();
  auto       document = IdentityDocument();
  SetShadows(document, 55.0F);
  const auto         plan     = Compile(document, input);
  const auto         grade_id = document.PrimaryGrade()->Id();
  const auto         source   = LocalToneSourceId(grade_id);
  const auto         result   = LocalToneResultId(grade_id);
  VulkanRenderDevice device;
  auto&              workspace = device.Workspace();

  // A cancelled render publishes nothing.
  device.BeginRender();
  ExecuteVulkanDevelop(device, plan, input, document);
  ExecuteVulkanGeometryResample(device, plan);
  ExecuteVulkanCameraColor(device, plan, document);
  const auto cancelled = ExecuteVulkanPrimaryGrade(device, plan, input, document);
  EXPECT_TRUE(cancelled.local_tone_rebuilt_reference);
  EXPECT_EQ(workspace.Images().PublishedRevision(source), 0U);
  device.CancelRender();
  EXPECT_EQ(workspace.Images().PublishedRevision(source), 0U);
  EXPECT_EQ(workspace.Images().PublishedRevision(result), 0U);

  const auto rendered = Render(device, plan, input, document);
  ASSERT_TRUE(rendered.result.local_tone_rebuilt_reference);
  for (const auto& id : {source, result}) {
    const auto* image = workspace.Images().Find(id);
    ASSERT_NE(image, nullptr);
    EXPECT_EQ(image->Texture().Format(), TextureFormat::R32f);
    const auto canonical = local_tone_mapping::ComputeMaskDimensions(
        static_cast<int>(kWidth), static_cast<int>(kHeight),
        local_tone_mapping::kReferenceMaskMaxLongEdge);
    EXPECT_EQ(image->Texture().Width(), static_cast<std::uint32_t>(canonical.width));
    EXPECT_EQ(image->Texture().Height(), static_cast<std::uint32_t>(canonical.height));
    EXPECT_NE(workspace.Images().PublishedRevision(id), 0U);
    EXPECT_EQ(workspace.Images().PublishedRevision(id),
              workspace.ResultInvalidation().RequiredRevision(id));
  }
  EXPECT_EQ(workspace.Images().PublishedAuxiliary(source), std::max(kWidth, kHeight));
  // The pyramid levels above level 0 are transient: no cached image, no node value.
  EXPECT_EQ(workspace.Images().Find(LocalToneLevelId(grade_id, "source", 1)), nullptr);
  EXPECT_EQ(workspace.Values().Find(LocalToneLevelId(grade_id, "remap_a", 0)), nullptr);
  EXPECT_EQ(rendered.result.local_tone_reference_resource_id,
            workspace.Images().Find(source)->Texture().ResourceId());
}

TEST_F(VulkanLocalToneTest, StableRenderSamplesTheCanonicalPlanesAndCreatesNoResource) {
  const auto input    = MakeToneInput();
  auto       document = IdentityDocument();
  SetShadows(document, 60.0F);
  const auto         plan = Compile(document, input);
  VulkanRenderDevice device;
  const auto         first = Render(device, plan, input, document);
  ASSERT_TRUE(first.result.local_tone_rebuilt_reference);

  // The second render: the Grade alone is counted (the Develop pass has scratch of its own).
  auto& backend = device.Workspace().Device();
  device.BeginRender();
  ExecuteVulkanDevelop(device, plan, input, document);
  ExecuteVulkanGeometryResample(device, plan);
  ExecuteVulkanCameraColor(device, plan, document);
  backend.ResetCounters();
  const auto second = ExecuteVulkanPrimaryGrade(device, plan, input, document);
  EXPECT_EQ(backend.BufferCreateCount(), 0U);
  EXPECT_EQ(backend.TextureCreateCount(), 0U);
  device.EndRender();
  device.WaitIdle();
  const auto output = DownloadScene(device, second.output_binding);
  device.PublishResults();
  EXPECT_FALSE(second.local_tone_rebuilt_reference);
  EXPECT_TRUE(second.local_tone_sampled_canonical_reference);
  EXPECT_EQ(second.local_tone_reference_resource_id, first.result.local_tone_reference_resource_id);
  // The sample of the canonical planes equals the render that built them.
  ExpectEqualToReference("the canonical sample against the rebuild", first.output, output, kWidth,
                         Uniform(0.0), "the first render");
}

TEST_F(VulkanLocalToneTest, LocalToneRecordsIntoTheOpenRecordingWithoutASubmission) {
  const auto input    = MakeToneInput();
  auto       document = IdentityDocument();
  SetShadows(document, 60.0F);
  SetHighlights(document, -60.0F);
  const auto         plan = Compile(document, input);
  VulkanRenderDevice device;
  auto&              backend = device.Workspace().Device();
  device.BeginRender();
  ExecuteVulkanDevelop(device, plan, input, document);
  ExecuteVulkanGeometryResample(device, plan);
  ExecuteVulkanCameraColor(device, plan, document);
  // PlanExecutor prepares the scene-work members before the first Grade.
  device.Workspace().EnsureSceneWorkImages(
      {plan.geometry.render_extent.width, plan.geometry.render_extent.height});
  backend.ResetCounters();
  const auto binding = PassEncoder<VulkanBackend, GpuPassKind::PrimaryColorGrade>::Encode(
      device, plan, input, document, *plan.FirstGrade(),
      FrameSceneBinding::CachedImage(plan.develop_output));
  EXPECT_TRUE(binding.IsWorkImage());
  EXPECT_EQ(backend.SubmitCount(), 0U);
  EXPECT_EQ(backend.WaitCount(), 0U);
  device.EndRender();
  device.WaitIdle();
  EXPECT_EQ(backend.SubmitCount(), 1U);
  device.PublishResults();
}

TEST_F(VulkanLocalToneTest, InvalidRequestsFailAndPublishNothing) {
  const auto input    = MakeToneInput();
  auto       document = IdentityDocument();
  SetShadows(document, 60.0F);
  const auto         plan     = Compile(document, input);
  const auto         grade_id = document.PrimaryGrade()->Id();
  VulkanRenderDevice device;
  const auto         scene = FrameSceneBinding::CachedImage(plan.develop_output);
  const auto         work  = FrameSceneBinding::WorkImage(SceneWorkMember::Member0);
  // Outside BeginRender.
  EXPECT_THROW((void)ExecuteVulkanLocalTone(device, work, work, scene, grade_id, 60.0F, 0.0F,
                                            plan.geometry, 1.0F, nullptr),
               std::runtime_error);
  // The working binding is not the adjusted scene-work member.
  device.BeginRender();
  ExecuteVulkanDevelop(device, plan, input, document);
  ExecuteVulkanGeometryResample(device, plan);
  ExecuteVulkanCameraColor(device, plan, document);
  device.Workspace().PrepareResultValidity(plan, document, input);
  device.Workspace().EnsureSceneWorkImages(
      {plan.geometry.render_extent.width, plan.geometry.render_extent.height});
  try {
    (void)ExecuteVulkanLocalTone(device, scene, work, scene, grade_id, 60.0F, 0.0F, plan.geometry,
                                 1.0F, nullptr);
    ADD_FAILURE() << "a cached input as the working image did not fail";
  } catch (const std::runtime_error& error) {
    EXPECT_NE(std::string(error.what()).find("in-place scene work member"), std::string::npos)
        << error.what();
  }
  device.CancelRender();

  // A Grade with a mask whose output the render does not have.
  auto masked = IdentityDocument();
  SetShadows(masked, 60.0F);
  masked.PrimaryGrade()->SetMix(0.5F);
  AddMask(masked);
  VulkanRenderDevice masked_device;
  try {
    (void)Render(masked_device, Compile(masked, input), input, masked);
    ADD_FAILURE() << "a missing mask did not fail";
  } catch (const std::runtime_error& error) {
    EXPECT_NE(std::string(error.what()).find("mask output is missing"), std::string::npos)
        << error.what();
  }
  EXPECT_EQ(masked_device.Workspace().Images().PublishedRevision(
                LocalToneSourceId(masked.PrimaryGrade()->Id())),
            0U);
  EXPECT_EQ(masked_device.Workspace().Images().PublishedRevision(
                LocalToneResultId(masked.PrimaryGrade()->Id())),
            0U);
}

}  // namespace
}  // namespace alcedo
