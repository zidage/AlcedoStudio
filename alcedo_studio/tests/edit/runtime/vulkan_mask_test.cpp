//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// MaskEvaluate, MaskUnion, the AI Mask coverage (Feather and Expand), and the coverage displays on
// the Vulkan backend (#1036, phase VK11): the same passes of the reference backend on the same
// GPU (decision B9: OpenCL on Windows, Metal on macOS) on the same inputs. The shared checks
// against the independent references and the Vulkan rules of the passes are in
// vulkan_mask_harness_test.cpp.

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <map>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

#include "../graph/grade_owned_mask_support.hpp"
#include "../graph/test_camera_profile.hpp"
#include "../input/prepared_raw_test_support.hpp"
#include "ai_mask_gpu_coverage_test_support.hpp"
#include "ai_mask_raster_test_support.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/input/raw_input_loader.hpp"
#include "edit/mask/mask_model.hpp"
#include "edit/operators/models/builtin_type_ids.hpp"
#include "edit/operators/models/scalar_operator_model.hpp"
#include "edit/runtime/frame_scene_binding.hpp"
#include "edit/runtime/graph_compiler.hpp"
#include "edit/runtime/mask_coverage_display.hpp"
#include "edit/runtime/pass_kind.hpp"
#include "edit/runtime/texture_format.hpp"
#include "edit/runtime/vulkan/vulkan_mask_pass.hpp"
#include "edit/runtime/vulkan/vulkan_pass_encoder.hpp"
#include "mask_range_reference.hpp"
#include "mask_range_test_support.hpp"
#include "multi_grade_runtime_test_support.hpp"
#include "multi_mask_runtime_test_support.hpp"
#include "vulkan/vulkan_context.hpp"
#include "vulkan_mask_test_support.hpp"
#include "vulkan_reference_backend_support.hpp"
#include "vulkan_workspace_test_support.hpp"

#if defined(HAVE_OPENCL)
#include "edit/runtime/opencl/opencl_mask_pass.hpp"
#include "edit/runtime/opencl/opencl_pass_encoder.hpp"
#else
#include "edit/runtime/metal/metal_mask_pass.hpp"
#include "edit/runtime/metal/metal_pass_encoder.hpp"
#endif

namespace alcedo {
namespace {

using namespace vulkan_reference;
using mask_range_reference::ScenePixel;
using namespace vulkan_mask_test;

/// The largest R8 difference of a Mask with a Hue Range or a Luminance Range from the reference
/// backend: the range weight runs atan2 and cube roots, whose last bits differ between the
/// Vulkan functions of math.slangh and the built-ins of OpenCL and Metal, so a texel at a
/// rounding boundary of the quantization moves by one code (measured: 1 texel of 6912 on the
/// RTX 3080). The other Mask results use @ref ReferenceCodes.
constexpr int                 kRangeCodes                 = 1;

/**
 * OpenCL on Intel Iris Xe gives a wrong MaskUnion of three or more Masks (nondeterministic) and a
 * wrong AI Mask Expand that writes a morphology work image a second time (three or more
 * steps into the two work images): its own checks against the shared independent
 * references fail there (`OpenClMaskFixture.GpuExpandMatchesAlternatingMorphologyExactly`), and
 * they pass on the NVIDIA OpenCL device. The OpenCL backend goes to the archive after Vulkan
 * (VK22), so it is not fixed (VK11 record). Where this is the reference, the test compares those
 * results with independent results instead: the Union with the maximum of the sources, an Expand
 * with the host morphology.
 */
inline constexpr std::size_t  kWrongReferenceUnionSources = 3;
inline constexpr std::int32_t kWrongReferenceWorkSteps    = 3;

auto                          ReferenceIsOpenClOnIntel() -> bool {
#if defined(HAVE_OPENCL)
  constexpr std::uint32_t kIntelVendorId = 0x8086;
  return vulkan::VulkanContext::Instance().Capabilities().vendor_id == kIntelVendorId;
#else
  return false;
#endif
}

/// Whether the reference backend gives a wrong coverage for @p edge: OpenCL on Intel when the
/// morphology writes a work image a second time.
auto ReferenceExpandIsWrong(const AiMaskEdgeAdjustment& edge) -> bool {
  // Without Feather the last morphology step writes the output, not a work image.
  const auto work_steps = std::abs(edge.expand_px) - (edge.feather_px > 0.0F ? 0 : 1);
  return ReferenceIsOpenClOnIntel() && work_steps >= kWrongReferenceWorkSteps;
}

/// The largest R8 difference of every other Mask result from the reference backend. OpenCL on
/// NVIDIA: none, the kernels give the same bytes. OpenCL on Intel: one code, from the last bits of
/// its cos and sin (measured: 1 texel of 17063 of a rotated Radial). Metal: one code, because the
/// product Metal kernels are built with fast math and contract the sums of the raster sampling
/// (measured on the Apple M4: 20 of 768 texels of an AI Mask at render scale 0.5, 9 of 3072 texels
/// of a coverage display).
auto ReferenceCodes() -> int {
#if defined(HAVE_OPENCL)
  return ReferenceIsOpenClOnIntel() ? 1 : 0;
#else
  return 1;
#endif
}

/// The R8 images of one Mask render.
struct MaskRender {
  std::map<std::string, std::vector<std::uint8_t>> sources;  ///< by Mask id
  std::vector<std::uint8_t>                        union_r8;
  std::uint64_t                                    union_resource = 0;
  std::map<std::string, std::uint64_t>             source_resources;
};

/// MaskEvaluate of every enabled Mask and MaskUnion of each Grade through the pass encoders, with
/// @p scene (when not empty) as the Grade input of every Mask.
template <class Device>
auto RenderMasks(Device& device, const ExecutionPlan& plan, const PreparedRawInput& input,
                 const PipelineDocument& document, std::span<const ScenePixel> scene = {},
                 SceneStorage storage = SceneStorage::kCachedImage) -> MaskRender {
  using Backend = BackendOf<Device>;
  device.BeginRender();
  try {
    FrameSceneBinding binding = FrameSceneBinding::CachedImage(kRangeSceneId);
    if (!scene.empty()) {
      binding = UploadScene(device, scene, storage, plan.geometry.render_extent);
    }
    for (const auto& grade : plan.grade_nodes) {
      if (!grade.mask_stack.has_value()) {
        continue;
      }
      for (const auto& source : grade.mask_stack->sources) {
        if (MaskSourceIsEnabled(document, grade.node_id, source.mask_id)) {
          PassEncoder<Backend, GpuPassKind::MaskEvaluate>::Encode(device, plan, input, document,
                                                                  grade, source, binding);
        }
      }
      PassEncoder<Backend, GpuPassKind::MaskUnion>::Encode(device, plan, input, document, grade);
      device.Workspace().TransientBuffers().Reset();
    }
    device.EndRender();
    device.WaitIdle();
  } catch (...) {
    device.CancelRender();
    throw;
  }
  MaskRender  render;
  const auto& grade = *plan.FirstGrade();
  for (const auto& source : grade.mask_stack->sources) {
    if (MaskSourceIsEnabled(document, grade.node_id, source.mask_id)) {
      const std::string id{source.mask_id.Value()};
      render.sources[id] = DownloadR8(device, source.effective_output);
      render.source_resources[id] =
          device.Workspace().Images().Find(source.effective_output)->Texture().ResourceId();
    }
  }
  render.union_r8 = DownloadR8(device, grade.mask_output);
  render.union_resource =
      device.Workspace().Images().Find(grade.mask_output)->Texture().ResourceId();
  device.PublishResults();
  return render;
}

/// The R8 codes of @p vulkan against @p reference: each within @p codes, with a report.
void ExpectR8EqualToReference(const std::string& label, std::span<const std::uint8_t> reference,
                              std::span<const std::uint8_t> vulkan, std::uint32_t width,
                              int codes = ReferenceCodes(), bool require_partial = true) {
  ASSERT_FALSE(reference.empty()) << label;
  ASSERT_EQ(reference.size(), vulkan.size()) << label;
  std::size_t differing = 0;
  std::size_t above     = 0;
  int         largest   = 0;
  std::size_t at        = 0;
  std::size_t partial   = 0;
  for (std::size_t i = 0; i < reference.size(); ++i) {
    const int difference = std::abs(int(vulkan[i]) - int(reference[i]));
    partial += reference[i] != 0 && reference[i] != 255 ? 1 : 0;
    differing += difference != 0 ? 1 : 0;
    above += difference > codes ? 1 : 0;
    if (difference > largest) {
      largest = difference;
      at      = i;
    }
  }
  std::printf(
      "%s: %s against Vulkan, %zu texels (%zu partial), %zu differ, largest %d codes at "
      "(%zu, %zu) (%s %d, Vulkan %d)\n",
      label.c_str(), kReferenceBackend, reference.size(), partial, differing, largest, at % width,
      at / width, kReferenceBackend, int(reference[at]), int(vulkan[at]));
  EXPECT_EQ(above, 0U) << label << ": largest difference " << largest << " codes";
  if (require_partial) {
    EXPECT_GT(partial, 0U) << label << ": the case has no partial coverage";
  }
}

/// The texelwise maximum of the source images: the Union, independent of any backend.
auto MaximumOfSources(const MaskRender& render) -> std::vector<std::uint8_t> {
  std::vector<std::uint8_t> maximum;
  for (const auto& [id, pixels] : render.sources) {
    if (maximum.empty()) {
      maximum = pixels;
      continue;
    }
    for (std::size_t i = 0; i < pixels.size(); ++i) {
      maximum[i] = std::max(maximum[i], pixels[i]);
    }
  }
  return maximum;
}

/// Each source of @p vulkan against @p reference (except @p independent, which the caller checks
/// against an independent result), and the Union.
void ExpectRenderEqualToReference(const std::string& label, const MaskRender& reference,
                                  const MaskRender& vulkan, std::uint32_t width,
                                  int                             codes       = ReferenceCodes(),
                                  const std::vector<std::string>& independent = {}) {
  ASSERT_EQ(reference.sources.size(), vulkan.sources.size()) << label;
  for (const auto& [id, pixels] : reference.sources) {
    ASSERT_TRUE(vulkan.sources.contains(id)) << label << " " << id;
    if (std::find(independent.begin(), independent.end(), id) != independent.end()) {
      continue;
    }
    ExpectR8EqualToReference(label + ", " + id, pixels, vulkan.sources.at(id), width, codes);
  }
  if (vulkan.sources.size() < 2) {
    return;
  }
  EXPECT_EQ(vulkan.union_r8, MaximumOfSources(vulkan))
      << label << ": the Union is not the maximum of its sources";
  if (!independent.empty() ||
      (ReferenceIsOpenClOnIntel() && reference.sources.size() >= kWrongReferenceUnionSources)) {
    std::printf("%s, Union: compared with the maximum of the sources\n", label.c_str());
    return;
  }
  ExpectR8EqualToReference(label + ", Union", reference.union_r8, vulkan.union_r8, width, codes,
                           false);
}

/// The scene of the range cases at @p extent: hue around the frame center, OkLab L along y,
/// chroma along x (the ramp and hue wheel of the shared range checks, at any size).
auto MakeRangeScene(Extent2D extent) -> std::vector<ScenePixel> {
  std::vector<ScenePixel> pixels;
  pixels.reserve(static_cast<std::size_t>(extent.width) * extent.height);
  for (std::uint32_t y = 0; y < extent.height; ++y) {
    for (std::uint32_t x = 0; x < extent.width; ++x) {
      const double u   = (x + 0.5) / extent.width - 0.5;
      const double v   = (y + 0.5) / extent.height - 0.5;
      const double hue = std::fmod(std::atan2(v, u) * 57.29577951308232 + 360.0, 360.0);
      pixels.push_back(mask_range_reference::PixelFromOkLch(
          0.08 + 0.84 * (y + 0.5) / extent.height, 0.005 + 0.16 * (x + 0.5) / extent.width, hue));
    }
  }
  return pixels;
}

class VulkanMaskTest : public vulkan_workspace_test::VulkanWorkspaceFixture {
 protected:
  void SetUp() override {
    VulkanWorkspaceFixture::SetUp();
    const std::string reason = PrepareReferenceBackend();
    ASSERT_TRUE(reason.empty()) << reason;
  }

  /// The plan and the Vulkan render of one case, for the checks against independent results.
  struct VulkanCase {
    ExecutionPlan plan;
    MaskRender    render;
  };

  /// Renders @p document on both backends with the same plan and compares the R8 images; the
  /// sources of @p independent are left to the caller.
  auto ExpectMasksEqual(const std::string& label, const PipelineDocument& document,
                        const PreparedRawInput& input, const RenderRequest& request = {},
                        std::span<const ScenePixel> scene   = {},
                        SceneStorage                storage = SceneStorage::kCachedImage,
                        const std::shared_ptr<const MaskAssetReader>& reader = nullptr,
                        int                                           codes  = ReferenceCodes(),
                        const std::vector<std::string>& independent          = {}) -> VulkanCase {
    auto                  plan = Compile(document, input, request);
    VulkanRenderDevice    vulkan_device;
    ReferenceRenderDevice reference_device;
    if (reader != nullptr) {
      vulkan_device.Workspace().SetMaskAssets(reader);
      reference_device.Workspace().SetMaskAssets(reader);
    }
    const auto vulkan    = RenderMasks(vulkan_device, plan, input, document, scene, storage);
    const auto reference = RenderMasks(reference_device, plan, input, document, scene, storage);
    ExpectRenderEqualToReference(label, reference, vulkan, plan.geometry.render_extent.width, codes,
                                 independent);
    return VulkanCase{std::move(plan), vulkan};
  }
};

// ---- Against the reference backend (B9) -------------------------------------------------------

TEST_F(VulkanMaskTest, AnalyticMasksEqualTheReferenceBackend) {
  const auto input = MakeInput(203, 157);
  struct Case {
    const char*   name;
    RenderRequest request;
    bool          crop_and_rotate = false;
  };
  RenderRequest region;
  region.view.visible_rect_in_edit_space = NormalizedRect{0.2F, 0.1F, 0.5F, 0.7F};
  region.view.viewport_extent            = Extent2D{101, 110};
  RenderRequest preview;
  preview.resolution.render_scale = 0.5F;
  const std::vector<Case> cases   = {
      {"full frame", {}},
      {"crop and 30 degree rotation", {}, true},
      {"region of interest", region},
      {"render scale 0.5", preview},
  };
  for (const auto& c : cases) {
    auto document = MakeDocument();
    AddMask(document, mask_range_test::MakeSoftRadialMask("mask.radial"));
    AddMask(document, MakeRotatedRadial("mask.rotated"));
    AddMask(document, MakeDiagonalGradient("mask.gradient"));
    if (c.crop_and_rotate) {
      document.Geometry().SetCropRect({0.1F, 0.15F, 0.8F, 0.7F});
      document.Geometry().SetRotationDegrees(30.0F);
    }
    ExpectMasksEqual(c.name, document, input, c.request);
  }
}

TEST_F(VulkanMaskTest, RangedMasksEqualTheReferenceBackend) {
  const Extent2D extent{96, 72};
  const auto     input      = MakeInput(extent.width, extent.height);
  const auto     scene      = MakeRangeScene(extent);
  auto           hue        = mask_range_test::MakeFullCoverageMask("mask.hue");
  hue.color_range           = ColorRangeModel{20.0F, 120.0F, 12.0F};
  auto wrapping             = mask_range_test::MakeFullCoverageMask("mask.wrapping_hue");
  wrapping.color_range      = ColorRangeModel{300.0F, 40.0F, 20.0F};
  auto luminance            = mask_range_test::MakeSoftRadialMask("mask.luminance");
  luminance.luminance_range = LuminanceRangeModel{0.3F, 0.7F, 0.1F};
  auto both                 = MakeRotatedRadial("mask.both");
  both.color_range          = ColorRangeModel{150.0F, 260.0F, 30.0F, 0.08F};
  both.luminance_range      = LuminanceRangeModel{0.2F, 0.8F, 0.15F};
  auto gradient             = MakeDiagonalGradient("mask.gradient_luminance");
  gradient.luminance_range  = LuminanceRangeModel{0.45F, 0.95F, 0.05F};
  for (const auto storage : {SceneStorage::kCachedImage, SceneStorage::kSceneWork}) {
    auto document = MakeDocument();
    for (const auto& mask : {hue, wrapping, luminance, both, gradient}) {
      AddMask(document, mask);
    }
    ExpectMasksEqual(storage == SceneStorage::kCachedImage ? "Grade input as a cached image"
                                                           : "Grade input as a scene-work member",
                     document, input, {}, scene, storage, nullptr, kRangeCodes);
  }
}

TEST_F(VulkanMaskTest, AiMasksEqualTheReferenceBackend) {
  const auto analysis = ai_mask_test::kAnalysisExtent;
  const auto asset    = ai_mask_test::MakeEllipseAsset(analysis, ai_mask_test::kAssetRect);
  const auto reader   = std::make_shared<ai_mask_test::InMemoryMaskAssetReader>();
  reader->Put(asset);
  struct Case {
    const char*          name;
    AiMaskEdgeAdjustment edge;
    float                render_scale    = 1.0F;
    bool                 crop_and_rotate = false;
    bool                 ranged          = false;
  };
  const std::vector<Case> cases = {
      {"no edge adjustment", {}},
      {"Feather 3", {3.0F, 0}},
      {"Expand +2", {0.0F, 2}},
      {"Expand -3", {0.0F, -3}},
      {"Feather 2.5 and Expand +4", {2.5F, 4}},
      {"Feather 2 at render scale 0.5", {2.0F, 0}, 0.5F},
      {"Expand +1 with crop and rotation", {0.0F, 1}, 1.0F, true},
      {"Feather 1.5 with a Hue Range and a Luminance Range", {1.5F, 0}, 1.0F, false, true},
  };
  const auto input = MakeInput(analysis.width, analysis.height);
  const auto scene = MakeRangeScene(analysis);
  for (const auto& c : cases) {
    auto document = MakeDocument();
    auto ai       = ai_mask_test::MakeAiMask(asset, analysis, "mask.ai", c.edge);
    if (c.ranged) {
      ai.color_range     = ColorRangeModel{200.0F, 330.0F, 25.0F};
      ai.luminance_range = LuminanceRangeModel{0.25F, 0.85F, 0.1F};
    }
    ai.invert  = c.ranged;
    ai.opacity = c.ranged ? 0.8F : 1.0F;
    AddMask(document, ai);
    AddMask(document, MakeRotatedRadial("mask.rotated"));
    if (c.crop_and_rotate) {
      document.Geometry().SetCropRect({0.1F, 0.15F, 0.8F, 0.7F});
      document.Geometry().SetRotationDegrees(30.0F);
    }
    RenderRequest request;
    request.resolution.render_scale = c.render_scale;
    const bool independent          = ReferenceExpandIsWrong(c.edge);
    const auto result               = ExpectMasksEqual(
        c.name, document, input, request,
        c.ranged ? std::span<const ScenePixel>(scene) : std::span<const ScenePixel>{},
        SceneStorage::kCachedImage, reader, c.ranged ? kRangeCodes : ReferenceCodes(),
        independent ? std::vector<std::string>{"mask.ai"} : std::vector<std::string>{});
    if (independent) {
      ASSERT_FALSE(c.ranged) << "the host evaluation has no ranges";
      std::printf("%s, mask.ai: compared with the host evaluation (OpenCL on Intel)\n", c.name);
      ai_mask_test::ExpectR8Within1(
          result.render.sources.at("mask.ai"),
          ai_mask_test::ExpectedAiMaskR8(asset, c.edge, result.plan, ai.invert, ai.opacity),
          std::string(c.name) + ", mask.ai against the host evaluation");
    }
  }
}

TEST_F(VulkanMaskTest, AiMaskFeatherAndExpandCoverageEqualsTheReferenceBackend) {
  const auto                              analysis = ai_mask_test::kEdgeAnalysisExtent;
  const auto                              pixels   = ai_mask_test::MakeEdgeTestCandidate(analysis);
  const std::vector<AiMaskEdgeAdjustment> edges    = {
      {0.0F, 1}, {0.0F, 5}, {0.0F, -4}, {0.5F, 0}, {4.0F, 0}, {50.0F, 0}, {3.5F, 3}, {7.0F, -2},
  };
  for (const auto& edge : edges) {
    VulkanRenderDevice    vulkan_device;
    ReferenceRenderDevice reference_device;
    const auto            vulkan =
        ai_mask_test::AcquireCandidateInCoverageWork(vulkan_device, pixels, analysis, edge);
    const auto reference =
        ai_mask_test::AcquireCandidateInCoverageWork(reference_device, pixels, analysis, edge);
    const std::string label =
        "Feather " + std::to_string(edge.feather_px) + ", Expand " + std::to_string(edge.expand_px);
    EXPECT_EQ(vulkan.descriptor.extent, reference.descriptor.extent) << label;
    if (ReferenceExpandIsWrong(edge)) {
      std::printf("%s: compared with the host morphology (OpenCL on Intel)\n", label.c_str());
      const auto host = ai_mask_test::ReferenceAiMaskEdgeOnImage(
          *pixels, static_cast<int>(analysis.width), static_cast<int>(analysis.height), edge);
      if (edge.feather_px == 0.0F) {
        EXPECT_EQ(vulkan.pixels, host) << label;
      } else {
        ai_mask_test::ExpectR8Within1(vulkan.pixels, host, label + " against the host");
      }
      continue;
    }
    ExpectR8EqualToReference(label, reference.pixels, vulkan.pixels,
                             vulkan.descriptor.extent.width);
  }
}

TEST_F(VulkanMaskTest, MaskUnionEqualsTheReferenceBackendAndAliasesOneSource) {
  const auto input = MakeInput(151, 113);
  // Three enabled sources fold by maximum; a disabled source does not count.
  {
    auto document = MakeDocument();
    AddMask(document, mask_range_test::MakeSoftRadialMask("mask.radial"));
    AddMask(document, MakeRotatedRadial("mask.rotated"));
    AddMask(document, MakeDiagonalGradient("mask.gradient"));
    AddMask(document, MakeDiagonalGradient("mask.disabled"));
    document.PrimaryGrade()->SetMaskEnabled(MaskId{"mask.disabled"}, false);
    ExpectMasksEqual("three enabled sources", document, input);
    // The independent reference of the shared mask checks (within its one code).
    const auto         plan = Compile(document, input);
    VulkanRenderDevice device;
    const auto         render = RenderMasks(device, plan, input, document);
    multi_mask_test::ExpectR8WithinTolerance(
        render.union_r8,
        multi_mask_test::EvaluateEnabledUnionR8(document.PrimaryGrade()->Masks(), plan.geometry));
  }
  // One enabled source: the Union output is the source image.
  {
    auto document = MakeDocument();
    AddMask(document, MakeRotatedRadial("mask.rotated"));
    AddMask(document, MakeDiagonalGradient("mask.disabled"));
    document.PrimaryGrade()->SetMaskEnabled(MaskId{"mask.disabled"}, false);
    const auto         plan = Compile(document, input);
    VulkanRenderDevice device;
    const auto         render = RenderMasks(device, plan, input, document);
    EXPECT_EQ(render.union_resource, render.source_resources.at("mask.rotated"));
    EXPECT_EQ(render.union_r8, render.sources.at("mask.rotated"));
  }
  // No enabled source: zeros.
  {
    auto document = MakeDocument();
    AddMask(document, MakeRotatedRadial("mask.disabled"));
    document.PrimaryGrade()->SetMaskEnabled(MaskId{"mask.disabled"}, false);
    const auto            plan = Compile(document, input);
    VulkanRenderDevice    vulkan_device;
    ReferenceRenderDevice reference_device;
    const auto            vulkan    = RenderMasks(vulkan_device, plan, input, document);
    const auto            reference = RenderMasks(reference_device, plan, input, document);
    EXPECT_TRUE(vulkan.sources.empty());
    EXPECT_EQ(vulkan.union_r8, reference.union_r8);
    EXPECT_TRUE(std::all_of(vulkan.union_r8.begin(), vulkan.union_r8.end(),
                            [](std::uint8_t code) { return code == 0; }));
  }
}

/// The red codes of an RGBA32F display image, after a check of green, blue, and alpha.
template <class Device>
auto DownloadDisplayCodes(Device& device, const GraphValueId& id) -> std::vector<std::uint8_t> {
  const auto                values = DownloadImage(device, id);
  std::vector<std::uint8_t> codes;
  codes.reserve(values.size() / 4);
  for (std::size_t i = 0; i < values.size(); i += 4) {
    EXPECT_EQ(values[i + 1], 0.0F);
    EXPECT_EQ(values[i + 2], 0.0F);
    EXPECT_EQ(values[i + 3], 1.0F);
    const float code = values[i] * 255.0F;
    EXPECT_NEAR(code, std::round(code), 1.0e-3F) << "the display value is not an R8 code";
    codes.push_back(static_cast<std::uint8_t>(std::lround(code)));
  }
  return codes;
}

/// The three display writes of one render: the value display of a probed Mask, the coverage
/// display of a committed AI Mask, and the range probe display of a candidate.
struct DisplayRender {
  std::vector<std::uint8_t> value;
  std::vector<std::uint8_t> coverage;
  std::vector<std::uint8_t> candidate;
  std::vector<std::uint8_t> probed_mask;
};

template <class Device>
auto RenderDisplays(Device& device, const ExecutionPlan& plan, const PreparedRawInput& input,
                    const PipelineDocument& document, const AiSegmentationMaskSource& ai,
                    const MaskModel& candidate, const AiMaskCoveragePixels& candidate_pixels,
                    std::span<const ScenePixel> scene) -> DisplayRender {
  using Backend = BackendOf<Device>;
  const GraphValueId kValueId{NodeId{"test.display"}, PortId{"value"}};
  const GraphValueId kCoverageId{NodeId{"test.display"}, PortId{"coverage"}};
  const GraphValueId kCandidateId{NodeId{"test.display"}, PortId{"candidate"}};
  const auto         extent    = plan.geometry.render_extent;
  const auto&        grade     = *plan.FirstGrade();
  auto&              workspace = device.Workspace();
  device.BeginRender();
  try {
    const auto  binding = UploadScene(device, scene, SceneStorage::kCachedImage, extent);
    const auto& source  = grade.mask_stack->sources.front();
    PassEncoder<Backend, GpuPassKind::MaskEvaluate>::Encode(device, plan, input, document, grade,
                                                            source, binding);
    const auto acquire = [&](const GraphValueId& id) -> auto& {
      return workspace
          .AcquireImageForWrite(id, {extent.width, extent.height, TextureFormat::Rgba32f})
          .Texture();
    };
    RecordMaskValueDisplay(device, workspace.Images().Find(source.effective_output)->Texture(),
                           acquire(kValueId));
    const auto coverage = workspace.AcquireAiMaskCoverage(ai, device.CommandContext());
    RecordMaskCoverageDisplay(device, *coverage,
                              MaskCoverageDisplayRequest{plan.geometry, true, 0.7F},
                              acquire(kCoverageId));
    const auto candidate_coverage = workspace.AcquireAiMaskCandidateCoverage(
        candidate_pixels, ai_mask_test::kAnalysisExtent, AiMaskEdgeAdjustment{1.5F, 1},
        device.CommandContext());
    RecordMaskCandidateRangeDisplay(device, plan, *candidate_coverage, candidate, binding,
                                    acquire(kCandidateId));
    device.EndRender();
    device.WaitIdle();
  } catch (...) {
    device.CancelRender();
    throw;
  }
  DisplayRender render;
  render.value       = DownloadDisplayCodes(device, kValueId);
  render.coverage    = DownloadDisplayCodes(device, kCoverageId);
  render.candidate   = DownloadDisplayCodes(device, kCandidateId);
  render.probed_mask = DownloadR8(device, grade.mask_stack->sources.front().effective_output);
  device.PublishResults();
  return render;
}

TEST_F(VulkanMaskTest, CoverageDisplaysEqualTheReferenceBackend) {
  const auto analysis = ai_mask_test::kAnalysisExtent;
  const auto asset    = ai_mask_test::MakeEllipseAsset(analysis, ai_mask_test::kAssetRect);
  const auto reader   = std::make_shared<ai_mask_test::InMemoryMaskAssetReader>();
  reader->Put(asset);
  auto document  = MakeDocument();
  auto ai        = ai_mask_test::MakeAiMask(asset, analysis, "mask.ai", {2.0F, 1});
  ai.color_range = ColorRangeModel{40.0F, 180.0F, 15.0F};
  AddMask(document, ai);
  auto candidate            = ai_mask_test::MakeAiMask(asset, analysis, "mask.candidate");
  candidate.luminance_range = LuminanceRangeModel{0.3F, 0.9F, 0.1F};
  candidate.opacity         = 0.9F;
  const auto            candidate_pixels = ai_mask_test::MakeEdgeTestCandidate(analysis);
  const auto            input            = MakeInput(analysis.width, analysis.height);
  const auto            scene            = MakeRangeScene(analysis);
  const auto            plan             = Compile(document, input);
  const auto&           stored           = std::get<AiSegmentationMaskSource>(ai.source);

  VulkanRenderDevice    vulkan_device;
  ReferenceRenderDevice reference_device;
  vulkan_device.Workspace().SetMaskAssets(reader);
  reference_device.Workspace().SetMaskAssets(reader);
  const auto vulkan    = RenderDisplays(vulkan_device, plan, input, document, stored, candidate,
                                        candidate_pixels, scene);
  const auto reference = RenderDisplays(reference_device, plan, input, document, stored, candidate,
                                        candidate_pixels, scene);
  const auto width     = plan.geometry.render_extent.width;
  ExpectR8EqualToReference("value display", reference.value, vulkan.value, width, kRangeCodes);
  EXPECT_EQ(vulkan.value, vulkan.probed_mask) << "the value display is the probed R8 image";
  ExpectR8EqualToReference("coverage display", reference.coverage, vulkan.coverage, width);
  ExpectR8EqualToReference("candidate range display", reference.candidate, vulkan.candidate, width,
                           kRangeCodes);
}

/// Develop, GeometryResample, CameraToAp1, then for each Grade its Masks on the running scene
/// binding, the Union, and the Grade, through the pass encoders (the order of PlanExecutor).
template <class Device>
auto RenderMaskedGrades(Device& device, const ExecutionPlan& plan, const PreparedRawInput& input,
                        const PipelineDocument& document) -> std::vector<float> {
  using Backend = BackendOf<Device>;
  std::vector<float> output;
  device.BeginRender();
  try {
    PassEncoder<Backend, GpuPassKind::UploadRaw>::Encode(device, plan, input, document);
    PassEncoder<Backend, GpuPassKind::GeometryResample>::Encode(device, plan, input, document);
    PassEncoder<Backend, GpuPassKind::CameraToAp1>::Encode(device, plan, input, document);
    device.Workspace().EnsureSceneWorkImages(
        {plan.geometry.render_extent.width, plan.geometry.render_extent.height});
    FrameSceneBinding scene = FrameSceneBinding::CachedImage(plan.develop_output);
    for (const auto& grade : plan.grade_nodes) {
      if (grade.mask_stack.has_value()) {
        for (const auto& source : grade.mask_stack->sources) {
          if (MaskSourceIsEnabled(document, grade.node_id, source.mask_id)) {
            PassEncoder<Backend, GpuPassKind::MaskEvaluate>::Encode(device, plan, input, document,
                                                                    grade, source, scene);
          }
        }
        PassEncoder<Backend, GpuPassKind::MaskUnion>::Encode(device, plan, input, document, grade);
        device.Workspace().TransientBuffers().Reset();
      }
      scene = PassEncoder<Backend, GpuPassKind::PrimaryColorGrade>::Encode(device, plan, input,
                                                                           document, grade, scene);
    }
    device.EndRender();
    device.WaitIdle();
    output = DownloadScene(device, scene);
  } catch (...) {
    device.CancelRender();
    throw;
  }
  device.PublishResults();
  return output;
}

TEST_F(VulkanMaskTest, MaskedGradesEqualTheReferenceBackend) {
  // Two Grades: the second Grade's Hue Range reads the output of the first Grade (a scene-work
  // member), as in the product order.
  auto document = multi_grade_test::MakeIdentityGradeDocument();
  multi_grade_test::GradeAdjustment<ExposureModel>(document, document.PrimaryGrade()->Id(),
                                                   type_ids::Exposure())
      .SetValue(1.2F);
  AddMask(document, MakeRotatedRadial("mask.rotated"));
  AddMask(document, MakeDiagonalGradient("mask.gradient"));
  multi_grade_test::AddCleanGradesBeforeDrt(document, {"grade.b"});
  auto* second = multi_grade_test::GradeNode(document, "grade.b");
  ASSERT_NE(second, nullptr);
  multi_grade_test::GradeAdjustment<ExposureModel>(document, second->Id(), type_ids::Exposure())
      .SetValue(-0.8F);
  auto hue        = mask_range_test::MakeFullCoverageMask("mask.second_hue");
  hue.color_range = ColorRangeModel{0.0F, 140.0F, 30.0F};
  (void)grade_mask_test::AddMask(*second, hue);
  document.MarkTopologyChanged();

  const auto            input = MakeInput(160, 120);
  const auto            plan  = Compile(document, input);
  VulkanRenderDevice    vulkan_device;
  ReferenceRenderDevice reference_device;
  const auto            vulkan    = RenderMaskedGrades(vulkan_device, plan, input, document);
  const auto            reference = RenderMaskedGrades(reference_device, plan, input, document);
  // The Grade tolerance of phase VK8: the Develop and Grade passes differ by float rounding.
  ExpectEqualToReference("two masked Grades", reference, vulkan, plan.geometry.render_extent.width,
                         Uniform(2.0e-4));
}

}  // namespace
}  // namespace alcedo
