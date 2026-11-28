//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// The DRT pass on the Vulkan backend (#1036, phase VK12): the parameter block, the display output
// of ACES 2.0 and OpenDRT (SDR and HDR encodings, the output color override of an export, a region
// of interest, the DRT finish with Sharpen, Clarity, and Halation) after Develop, the Color Grade,
// and the DiffusionFilter against the same passes of the reference backend on the same GPU
// (decision B9: OpenCL on Windows, Metal on macOS), the steady state, the recording rule, and the
// phases that are not on Vulkan yet (the film print, the Film Grain of the finish).

#include <gtest/gtest.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

#include "../graph/test_camera_profile.hpp"
#include "../input/prepared_raw_test_support.hpp"
#include "edit/graph/drt_node_model.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/input/raw_input_loader.hpp"
#include "edit/operators/models/scalar_operator_model.hpp"
#include "edit/operators/models/sharpen_model.hpp"
#include "edit/runtime/drt/drt_output_resolver.hpp"
#include "edit/runtime/graph_compiler.hpp"
#include "edit/runtime/parameter_binding.hpp"
#include "edit/runtime/vulkan/vulkan_develop_pass.hpp"
#include "edit/runtime/vulkan/vulkan_diffusion_filter_pass.hpp"
#include "edit/runtime/vulkan/vulkan_drt_params.hpp"
#include "edit/runtime/vulkan/vulkan_drt_pass.hpp"
#include "edit/runtime/vulkan/vulkan_geometry_camera_pass.hpp"
#include "edit/runtime/vulkan/vulkan_pass_encoder.hpp"
#include "edit/runtime/vulkan/vulkan_primary_grade_pass.hpp"
#include "vulkan/vulkan_api_counters.hpp"
#include "vulkan_reference_backend_support.hpp"
#include "vulkan_workspace_test_support.hpp"

#if defined(HAVE_OPENCL)
#include "edit/runtime/opencl/opencl_drt_params.hpp"
#endif

namespace alcedo {
namespace {

using namespace vulkan_reference;

/// The difference of a display value from the reference backend: ACES 2.0 at most 5e-4 (half a
/// 10-bit code) with an rms of 2e-6, OpenDRT 1e-5. The ACES 2.0 output of a dark color beyond the
/// AP1 gamut moves by 1.2e-4 in the reference itself when its input moves by one ulp (measured by
/// DisplayTransformOfOneInputEqualsTheReferenceBackend), so a last-bit difference of a math
/// function gives a difference of that size.
constexpr Tolerance kAcesTolerance    = {5.0e-4, 5.0e-4, 0, 2.0e-6};
constexpr Tolerance kOpenDrtTolerance = Uniform(1.0e-5);

constexpr auto      ToleranceFor(DrtMethod method) -> Tolerance {
  return method == DrtMethod::Aces20 ? kAcesTolerance : kOpenDrtTolerance;
}

constexpr std::uint32_t kWidth  = 192;
constexpr std::uint32_t kHeight = 128;

/// Scene-linear camera RGB for the display transform: an exposure ramp from 2^-10 to 2^5 along x,
/// the hue circle along y with a saturation that rises to beyond the AP1 gamut, a white window
/// above the display peak, and a texture of 10 % for the finish.
auto MakeDisplayPlane(std::uint32_t width, std::uint32_t height) -> HostImagePlane {
  HostImagePlane plane;
  plane.extent       = {width, height};
  plane.stride_bytes = width * 16U;
  plane.format       = HostPixelFormat::F32Rgba;
  auto  storage      = std::shared_ptr<std::byte>(new std::byte[plane.ByteCount()],
                                                  [](std::byte* pointer) { delete[] pointer; });
  auto* pixels       = reinterpret_cast<float*>(storage.get());
  for (std::uint32_t y = 0; y < height; ++y) {
    const float v          = static_cast<float>(y) / static_cast<float>(height - 1);
    const float hue        = 6.2831853F * v;
    const float saturation = 1.6F * std::fmod(3.0F * v, 1.0F);
    for (std::uint32_t x = 0; x < width; ++x) {
      const float u       = static_cast<float>(x) / static_cast<float>(width - 1);
      float       stop    = -10.0F + 15.0F * u;
      const bool  window  = u > 0.7F && u < 0.8F && v > 0.1F && v < 0.2F;
      const float texture = 1.0F + 0.1F * std::sin(0.8F * static_cast<float>(x)) *
                                       std::cos(0.6F * static_cast<float>(y));
      const float level = std::exp2(window ? 6.0F : stop) * texture;
      const float s     = window ? 0.0F : saturation;
      const auto  index = (static_cast<std::size_t>(y) * width + x) * 4;
      pixels[index + 0] = level * std::max(0.02F, 1.0F + s * std::cos(hue));
      pixels[index + 1] = level * std::max(0.02F, 1.0F + s * std::cos(hue - 2.0943951F));
      pixels[index + 2] = level * std::max(0.02F, 1.0F + s * std::cos(hue + 2.0943951F));
      pixels[index + 3] = 1.0F;
    }
  }
  plane.bytes = std::const_pointer_cast<const std::byte>(storage);
  return plane;
}

auto MakeDisplayInput(std::uint32_t width = kWidth, std::uint32_t height = kHeight)
    -> PreparedRawInput {
  return RawInputLoader::FromDirectRgb(MakeDisplayPlane(width, height),
                                       gpu_dag_test::FullSensor(width, height));
}

auto MakeDocument() -> PipelineDocument {
  auto document = CreateDefaultPipelineDocument();
  gpu_dag_test::EnsureTestCameraProfile(document);
  return document;
}

struct Encoding {
  DrtMethod     method    = DrtMethod::OpenDrt;
  DrtColorSpace space     = DrtColorSpace::Rec709;
  DrtEotf       eotf      = DrtEotf::Gamma22;
  float         peak      = 100.0F;
  float         diffusion = 0.0F;
};

void SetEncoding(PipelineDocument& document, const Encoding& encoding) {
  auto payload           = document.Drt()->Params().Params();
  payload.method         = encoding.method;
  payload.encoding_space = encoding.space;
  payload.limiting_space = encoding.space;
  payload.encoding_eotf  = encoding.eotf;
  payload.peak_luminance = encoding.peak;
  document.Drt()->Params().ReplaceParams(payload);
  document.Drt()->Params().ApplyDiffusionStrength(encoding.diffusion);
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

template <class Model>
auto DrtAdjustment(PipelineDocument& document, const OperatorTypeId& type) -> Model& {
  auto* model = dynamic_cast<Model*>(document.Drt()->FindAdjustmentByType(type));
  if (model == nullptr) {
    throw std::runtime_error("the default DRT has no " + std::string(type.Text()));
  }
  return *model;
}

void SetSharpen(PipelineDocument& document, float amount) {
  auto& sharpen = DrtAdjustment<SharpenModel>(document, type_ids::Sharpen());
  sharpen.SetAmount(amount);
  sharpen.SetRadius(2.0F);
  sharpen.SetThreshold(0.0F);
}

/// One render of the display output and the counters of its DiffusionFilter and DRT passes.
struct DisplayRender {
  std::vector<float>        display;
  vulkan::VulkanApiCounters drt_delta{};
  VulkanDrtResult           result{};
};

/// Develop, GeometryResample, CameraToAp1, every Color Grade, the DiffusionFilter, and the DRT on
/// @p device (the Vulkan passes through their pass encoders); publishes the results.
template <class Device>
auto Render(Device& device, const ExecutionPlan& plan, const PreparedRawInput& input,
            const PipelineDocument& document) -> DisplayRender {
  DisplayRender render;
  GraphValueId  display{};
  device.BeginRender();
  try {
    FrameSceneBinding scene{};
    if constexpr (std::is_same_v<Device, VulkanRenderDevice>) {
      ExecuteVulkanDevelop(device, plan, input, document);
      ExecuteVulkanGeometryResample(device, plan);
      ExecuteVulkanCameraColor(device, plan, document);
      scene             = ExecuteVulkanPrimaryGrade(device, plan, input, document).output_binding;
      const auto before = vulkan::ReadVulkanApiCounters();
      const auto linear = PassEncoder<VulkanBackend, GpuPassKind::DiffusionFilter>::Encode(
          device, plan, input, document, scene);
      render.result    = ExecuteVulkanDrt(device, plan, document, linear);
      render.drt_delta = vulkan::VulkanApiCounterDelta(before, vulkan::ReadVulkanApiCounters());
      display          = render.result.output;
    } else {
      ExecuteReferenceDevelop(device, plan, input, document);
      ExecuteReferenceGeometryResample(device, plan);
      ExecuteReferenceCameraColor(device, plan, document);
      scene             = ExecuteReferencePrimaryGrade(device, plan, input, document);
      const auto linear = ExecuteReferenceDiffusionFilter(device, plan, document, scene);
      display           = ExecuteReferenceDrt(device, plan, document, linear);
    }
    device.EndRender();
    device.WaitIdle();
  } catch (...) {
    device.CancelRender();
    throw;
  }
  render.display = DownloadImage(device, display);
  device.PublishResults();
  return render;
}

/// Linear AP1 for the display transform alone: the hue circle along x, the exposure from 2^-12 to
/// 2^6 along y, and in each band of rows a chroma from 0 to beyond the AP1 gamut (a negative
/// channel).
auto MakeLinearAp1(std::uint32_t width, std::uint32_t height) -> std::vector<float> {
  std::vector<float> values(static_cast<std::size_t>(width) * height * 4);
  for (std::uint32_t y = 0; y < height; ++y) {
    const float v      = static_cast<float>(y) / static_cast<float>(height - 1);
    const float level  = std::exp2(-12.0F + 18.0F * v);
    const float chroma = 1.4F * std::fmod(4.0F * v, 1.0F);
    for (std::uint32_t x = 0; x < width; ++x) {
      const float hue   = 6.2831853F * static_cast<float>(x) / static_cast<float>(width);
      const auto  index = (static_cast<std::size_t>(y) * width + x) * 4;
      values[index + 0] = level * (1.0F + chroma * std::cos(hue));
      values[index + 1] = level * (1.0F + chroma * std::cos(hue - 2.0943951F));
      values[index + 2] = level * (1.0F + chroma * std::cos(hue + 2.0943951F));
      values[index + 3] = 0.5F;
    }
  }
  return values;
}

/// The DRT pass of @p device alone on the linear AP1 @p linear (a cached RGBA32F image of the
/// render extent); the display output.
template <class Device>
auto RenderDrtOnly(Device& device, const ExecutionPlan& plan, const PipelineDocument& document,
                   const PreparedRawInput& input, const std::vector<float>& linear)
    -> std::vector<float> {
  const auto&        extent = plan.geometry.render_extent;
  const GraphValueId linear_id{NodeId{"test.linear"}, PortId{"image"}};
  GraphValueId       display{};
  device.BeginRender();
  try {
    auto& workspace = device.Workspace();
    workspace.PrepareResultValidity(plan, document, input);
    workspace.EnsureSceneWorkImages({extent.width, extent.height});
    auto& lease = workspace.AcquireImageForWrite(
        linear_id, {extent.width, extent.height, TextureFormat::Rgba32f});
    workspace.Device().UploadTexture2D(lease.Texture(), std::as_bytes(std::span(linear)),
                                       device.CommandContext());
    const auto scene = FrameSceneBinding::CachedImage(linear_id);
    if constexpr (std::is_same_v<Device, VulkanRenderDevice>) {
      PassEncoder<VulkanBackend, GpuPassKind::Drt>::Encode(device, plan, input, document, scene);
      display = plan.display_output;
    } else {
      display = ExecuteReferenceDrt(device, plan, document, scene);
    }
    device.EndRender();
    device.WaitIdle();
  } catch (...) {
    device.CancelRender();
    throw;
  }
  auto values = DownloadImage(device, display);
  device.PublishResults();
  return values;
}

/// The largest display value and the count of non-finite values: a case must reach the display
/// range and stay finite.
void ExpectFiniteDisplay(const std::string& label, const std::vector<float>& display) {
  float       largest    = 0.0F;
  std::size_t non_finite = 0;
  for (const float value : display) {
    if (!std::isfinite(value)) {
      ++non_finite;
    } else {
      largest = std::max(largest, value);
    }
  }
  EXPECT_EQ(non_finite, 0U) << label;
  EXPECT_GT(largest, 0.5F) << label << ": the case does not reach the display range";
}

class VulkanDrtTest : public vulkan_workspace_test::VulkanWorkspaceFixture {};

class VulkanDrtComparisonTest : public VulkanDrtTest {
 protected:
  void SetUp() override {
    VulkanDrtTest::SetUp();
    const std::string reason = PrepareReferenceBackend();
    ASSERT_TRUE(reason.empty()) << reason;
  }

  /// Renders @p document on both backends and expects the display outputs equal.
  void ExpectEqualRenders(const std::string& label, const PipelineDocument& document,
                          const PreparedRawInput& input, const ExecutionPlan& plan,
                          std::uint32_t expected_post = 0) {
    VulkanRenderDevice    vulkan_device;
    ReferenceRenderDevice reference_device;
    const auto            vulkan    = Render(vulkan_device, plan, input, document);
    const auto            reference = Render(reference_device, plan, input, document);
    EXPECT_EQ(vulkan.result.post_neighborhood_count, expected_post) << label;
    EXPECT_EQ(vulkan.drt_delta.submits, 0U) << label;
    ExpectFiniteDisplay(label, vulkan.display);
    ExpectEqualToReference(label, reference.display, vulkan.display,
                           plan.geometry.render_extent.width,
                           ToleranceFor(document.Drt()->Params().Method()));
  }
};

// ---- Parameter block ------------------------------------------------------------------------

auto Word(const VulkanDrtParamBlock& block, int word) -> float {
  return std::bit_cast<float>(block[static_cast<std::size_t>(word)]);
}

TEST_F(VulkanDrtTest, ParameterBlockHoldsTheResolvedTransform) {
  const std::vector<Encoding> encodings = {
      {DrtMethod::Aces20, DrtColorSpace::Rec709, DrtEotf::Gamma22, 100.0F},
      {DrtMethod::Aces20, DrtColorSpace::Rec2020, DrtEotf::St2084, 1000.0F},
      {DrtMethod::OpenDrt, DrtColorSpace::P3D65, DrtEotf::SrgbPiecewise, 100.0F},
      {DrtMethod::OpenDrt, DrtColorSpace::Rec2020, DrtEotf::Hlg, 1000.0F},
  };
  for (const auto& encoding : encodings) {
    auto document = MakeDocument();
    SetEncoding(document, encoding);
    const auto resolved = DrtOutputResolver::ResolveNode(*document.Drt(), std::nullopt, "test");
    const auto block    = PackVulkanDrtParams(resolved);
    const auto label    = std::to_string(static_cast<int>(encoding.method)) + "/" +
                       std::to_string(static_cast<int>(encoding.eotf));
    EXPECT_EQ(static_cast<std::int32_t>(block[ALCEDO_VK_DRT_METHOD]),
              static_cast<std::int32_t>(resolved.method_))
        << label;
    EXPECT_EQ(static_cast<std::int32_t>(block[ALCEDO_VK_DRT_EOTF]),
              static_cast<std::int32_t>(resolved.eotf_))
        << label;
    EXPECT_EQ(Word(block, ALCEDO_VK_DRT_DISPLAY_LINEAR_SCALE), resolved.display_linear_scale_)
        << label;
    EXPECT_EQ(Word(block, ALCEDO_VK_DRT_LIMIT_TO_DISPLAY + 1),
              resolved.limit_to_display_matx_(0, 1))
        << label;
    EXPECT_EQ(Word(block, ALCEDO_VK_DRT_ODRT_TS_S1), resolved.open_drt_params_.ts_s1_) << label;
    EXPECT_EQ(static_cast<std::int32_t>(block[ALCEDO_VK_DRT_ODRT_DISPLAY_GAMUT]),
              resolved.open_drt_params_.display_gamut_)
        << label;
    if (encoding.method == DrtMethod::Aces20) {
      const auto& aces = resolved.aces_params_;
      ASSERT_TRUE(aces.table_gamut_cusps_) << label;
      EXPECT_EQ(Word(block, ALCEDO_VK_DRT_ACES_PEAK_LUMINANCE), aces.peak_luminance_) << label;
      EXPECT_EQ(Word(block, ALCEDO_VK_DRT_ACES_TABLE_HUES + 200), (*aces.table_hues_)[200])
          << label;
      EXPECT_EQ(Word(block, ALCEDO_VK_DRT_ACES_TABLE_GAMUT_CUSPS + 4 * 361 + 1),
                (*aces.table_gamut_cusps_)[361](1))
          << label;
      EXPECT_EQ(block[ALCEDO_VK_DRT_ACES_TABLE_GAMUT_CUSPS + 4 * 17 + 3], 0U) << label;
      EXPECT_EQ(Word(block, ALCEDO_VK_DRT_ACES_INPUT + ALCEDO_VK_DRT_JMH_A_W_J),
                1.0F / aces.input_params_.inv_A_w_J_)
          << label;
    } else {
      EXPECT_EQ(block[ALCEDO_VK_DRT_ACES_TABLE_REACH_M + 100], 0U) << label;
    }
#if defined(HAVE_OPENCL)
    // The bytes of the OpenCL packing: the OpenCL program and the Vulkan kernels read one layout.
    const auto opencl = PackOpenClDrtParams(resolved);
    static_assert(sizeof(opencl) == sizeof(block));
    EXPECT_EQ(std::memcmp(&opencl, block.data(), sizeof(block)), 0) << label;
#endif
  }
}

TEST_F(VulkanDrtTest, ParameterBlockOfAnUnresolvedAcesTransformFails) {
  ColorUtils::TO_OUTPUT_Params params;
  params.method_ = ColorUtils::ODTMethod::ACES_2_0;
  EXPECT_THROW((void)PackVulkanDrtParams(params), std::runtime_error);
}

// ---- Against the reference backend on the same GPU (B9) ---------------------------------------

TEST_F(VulkanDrtComparisonTest, DisplayTransformsEqualTheReferenceBackend) {
  struct Case {
    const char* name;
    Encoding    encoding;
  };
  const std::vector<Case> cases = {
      {"ACES 2.0, Rec.709 gamma 2.2", {DrtMethod::Aces20, DrtColorSpace::Rec709, DrtEotf::Gamma22}},
      {"ACES 2.0, Rec.2020 ST 2084 1000 nits",
       {DrtMethod::Aces20, DrtColorSpace::Rec2020, DrtEotf::St2084, 1000.0F}},
      {"ACES 2.0, P3-D65 sRGB, diffusion 0.6",
       {DrtMethod::Aces20, DrtColorSpace::P3D65, DrtEotf::SrgbPiecewise, 100.0F, 0.6F}},
      {"OpenDRT, Rec.709 gamma 2.2", {DrtMethod::OpenDrt, DrtColorSpace::Rec709, DrtEotf::Gamma22}},
      {"OpenDRT, Rec.2020 HLG 1000 nits",
       {DrtMethod::OpenDrt, DrtColorSpace::Rec2020, DrtEotf::Hlg, 1000.0F}},
      {"OpenDRT, P3-D65 sRGB, diffusion 1",
       {DrtMethod::OpenDrt, DrtColorSpace::P3D65, DrtEotf::SrgbPiecewise, 100.0F, 1.0F}},
      {"OpenDRT, Rec.2020 ST 2084 1000 nits",
       {DrtMethod::OpenDrt, DrtColorSpace::Rec2020, DrtEotf::St2084, 1000.0F}},
  };
  const auto input = MakeDisplayInput();
  for (const auto& c : cases) {
    auto document = MakeDocument();
    SetEncoding(document, c.encoding);
    ExpectEqualRenders(c.name, document, input, Compile(document, input));
  }
}

TEST_F(VulkanDrtComparisonTest, DisplayTransformOfOneInputEqualsTheReferenceBackend) {
  constexpr std::uint32_t kHues   = 360;
  constexpr std::uint32_t kLevels = 96;
  struct Case {
    const char* name;
    Encoding    encoding;
  };
  const std::vector<Case> cases = {
      {"ACES 2.0, Rec.709 gamma 2.2", {DrtMethod::Aces20, DrtColorSpace::Rec709, DrtEotf::Gamma22}},
      {"ACES 2.0, Rec.2020 ST 2084 1000 nits",
       {DrtMethod::Aces20, DrtColorSpace::Rec2020, DrtEotf::St2084, 1000.0F}},
      {"OpenDRT, Rec.709 gamma 2.2", {DrtMethod::OpenDrt, DrtColorSpace::Rec709, DrtEotf::Gamma22}},
      {"OpenDRT, P3-D65 ST 2084 1000 nits",
       {DrtMethod::OpenDrt, DrtColorSpace::P3D65, DrtEotf::St2084, 1000.0F}},
  };
  const auto input  = MakeDisplayInput(kHues, kLevels);
  const auto linear = MakeLinearAp1(kHues, kLevels);
  // The same input one ulp up in each channel: how far the reference moves for the smallest
  // change of its input.
  auto       nudged = linear;
  for (auto& value : nudged) {
    value = std::nextafter(value, INFINITY);
  }
  for (const auto& c : cases) {
    auto document = MakeDocument();
    SetEncoding(document, c.encoding);
    const auto            plan = Compile(document, input);
    VulkanRenderDevice    vulkan_device;
    ReferenceRenderDevice reference_device;
    const auto            vulkan = RenderDrtOnly(vulkan_device, plan, document, input, linear);
    const auto reference         = RenderDrtOnly(reference_device, plan, document, input, linear);
    const auto moved             = RenderDrtOnly(reference_device, plan, document, input, nudged);
    ExpectFiniteDisplay(c.name, vulkan);
    ExpectEqualToReference(std::string(c.name) + " (reference, input one ulp up)", reference, moved,
                           kHues, Uniform(1.0), kReferenceBackend);
    std::size_t first_row = 0;
    std::size_t last_row  = kLevels;
#if !defined(HAVE_OPENCL)
    // Metal OpenDRT gives 0 for an exactly neutral input (#1235): rows 0 and 95, the two rows of
    // chroma 0, are not compared with Metal; their Vulkan pixels must not be black.
    if (c.encoding.method == DrtMethod::OpenDrt) {
      first_row = 1;
      last_row  = kLevels - 1;
      for (const std::size_t row : {std::size_t{0}, std::size_t{kLevels - 1}}) {
        for (std::size_t i = row * kHues * 4; i < (row + 1) * kHues * 4; i += 4) {
          ASSERT_GT(std::max({vulkan[i], vulkan[i + 1], vulkan[i + 2]}), 0.01F)
              << c.name << ": a neutral pixel is black";
        }
      }
    }
#endif
    const auto offset = first_row * kHues * 4;
    const auto values = (last_row - first_row) * kHues * 4;
    ExpectEqualToReference(c.name, std::span(reference).subspan(offset, values),
                           std::span(vulkan).subspan(offset, values), kHues,
                           ToleranceFor(c.encoding.method));
  }
}

TEST_F(VulkanDrtComparisonTest, ExportColorOverrideAndRegionEqualTheReferenceBackend) {
  const auto input = MakeDisplayInput();
  for (const auto method : {DrtMethod::Aces20, DrtMethod::OpenDrt}) {
    const std::string name     = method == DrtMethod::Aces20 ? "ACES 2.0" : "OpenDRT";
    auto              document = MakeDocument();
    SetEncoding(document, {method, DrtColorSpace::Rec709, DrtEotf::Gamma22, 100.0F, 0.5F});
    auto plan                  = Compile(document, input);
    plan.output_color_override = ExportColorProfileConfig{ColorUtils::ColorSpace::REC2020,
                                                          ColorUtils::EOTF::ST2084, 1000.0F};
    ExpectEqualRenders(name + ", export override Rec.2020 ST 2084", document, input, plan);
    const auto region = Compile(document, input, RegionRequest(kWidth, kHeight));
    ASSERT_EQ(region.geometry.render_extent.width, kWidth / 2);
    ExpectEqualRenders(name + ", region of interest", document, input, region);
  }
}

TEST_F(VulkanDrtComparisonTest, DrtFinishEqualsTheReferenceBackend) {
  struct Case {
    const char*   name;
    float         sharpen;
    float         clarity;
    float         halation;
    std::uint32_t count;
  };
  // An odd count starts on the free scene-work member, an even count on the display output.
  const std::vector<Case> cases = {
      {"Sharpen", 60.0F, 0.0F, 0.0F, 1},
      {"Clarity and Halation", 0.0F, 40.0F, 50.0F, 2},
      {"Sharpen, Clarity, and Halation", 80.0F, -30.0F, 70.0F, 3},
  };
  const auto input = MakeDisplayInput();
  for (const auto method : {DrtMethod::Aces20, DrtMethod::OpenDrt}) {
    for (const auto& c : cases) {
      auto document = MakeDocument();
      SetEncoding(document, {method, DrtColorSpace::Rec709, DrtEotf::Gamma22});
      if (c.sharpen != 0.0F) {
        SetSharpen(document, c.sharpen);
      }
      if (c.clarity != 0.0F) {
        DrtAdjustment<ClarityModel>(document, type_ids::Clarity()).SetValue(c.clarity);
      }
      if (c.halation != 0.0F) {
        DrtAdjustment<HalationModel>(document, type_ids::Halation()).SetValue(c.halation);
      }
      const std::string name =
          std::string(method == DrtMethod::Aces20 ? "ACES 2.0, " : "OpenDRT, ") + c.name;
      ExpectEqualRenders(name, document, input, Compile(document, input), c.count);
    }
  }
}

// ---- Shared checks of the OpenCL DRT test on Vulkan -----------------------------------------

TEST_F(VulkanDrtTest, Aces20HueSweepStaysFiniteWithoutIsolatedBlackPixels) {
  constexpr std::uint32_t kHues    = 360;
  auto                    document = MakeDocument();
  SetEncoding(document, {DrtMethod::Aces20, DrtColorSpace::Rec709, DrtEotf::Gamma22});
  const auto input = RawInputLoader::FromDirectRgb(
      gpu_dag_test::MakeSaturatedHueWheelPlane(kHues, 4.0F), gpu_dag_test::FullSensor(kHues, 1));
  VulkanRenderDevice device;
  const auto         display = Render(device, Compile(document, input), input, document).display;
  ASSERT_EQ(display.size(), static_cast<std::size_t>(kHues) * 4);
  auto luma = [&](std::uint32_t i) {
    return 0.2126F * display[4 * i] + 0.7152F * display[4 * i + 1] + 0.0722F * display[4 * i + 2];
  };
  std::size_t isolated_black = 0;
  for (std::uint32_t i = 0; i < kHues; ++i) {
    ASSERT_TRUE(std::isfinite(display[4 * i]) && std::isfinite(display[4 * i + 1]) &&
                std::isfinite(display[4 * i + 2]));
    const float neighbor_floor = std::min(luma((i + kHues - 1) % kHues), luma((i + 1) % kHues));
    if (neighbor_floor > 0.08F && luma(i) < 0.25F * neighbor_floor) {
      ++isolated_black;
    }
  }
  EXPECT_EQ(isolated_black, 0U);
}

// ---- Steady state, recording, and failure rules ---------------------------------------------

TEST_F(VulkanDrtTest, StableRenderPacksOnceAndCreatesNothing) {
  const auto input    = MakeDisplayInput();
  auto       document = MakeDocument();
  SetEncoding(document, {DrtMethod::Aces20, DrtColorSpace::Rec709, DrtEotf::Gamma22, 100.0F, 0.6F});
  SetSharpen(document, 50.0F);
  const auto         plan = Compile(document, input);
  VulkanRenderDevice device;
  const auto         first = Render(device, plan, input, document);
  EXPECT_GE(first.drt_delta.upload_bytes, sizeof(VulkanDrtParamBlock));
  const ParameterSlotKey key{document.Drt()->Id(), AdjustmentInstanceId{"drt.output"}};
  EXPECT_EQ(device.Workspace().Parameters().AppliedRevision(key),
            document.Drt()->Params().Revision());

  // The same frame again: no new pipeline, buffer, or image, and no new parameter bytes.
  const auto second = Render(device, plan, input, document);
  EXPECT_EQ(second.drt_delta.pipeline_creations, 0U);
  EXPECT_EQ(second.drt_delta.module_loads, 0U);
  EXPECT_EQ(second.drt_delta.buffer_creations, 0U);
  EXPECT_EQ(second.drt_delta.image_creations, 0U);
  EXPECT_LT(second.drt_delta.upload_bytes, sizeof(VulkanDrtParamBlock));
  EXPECT_EQ(second.display, first.display);

  // An export override packs its own transform without a revision; the next plain frame packs
  // the stored transform again.
  auto exported = plan;
  exported.output_color_override =
      ExportColorProfileConfig{ColorUtils::ColorSpace::REC2020, ColorUtils::EOTF::ST2084, 1000.0F};
  const auto overridden = Render(device, exported, input, document);
  EXPECT_GE(overridden.drt_delta.upload_bytes, sizeof(VulkanDrtParamBlock));
  EXPECT_EQ(device.Workspace().Parameters().AppliedRevision(key), kNoParameterRevision);
  EXPECT_NE(overridden.display, first.display);
  const auto plain = Render(device, plan, input, document);
  EXPECT_GE(plain.drt_delta.upload_bytes, sizeof(VulkanDrtParamBlock));
  EXPECT_EQ(plain.display, first.display);
}

TEST_F(VulkanDrtTest, DrtRecordsIntoTheOpenRecordingWithoutASubmission) {
  const auto input    = MakeDisplayInput();
  auto       document = MakeDocument();
  SetEncoding(document, {DrtMethod::OpenDrt, DrtColorSpace::Rec709, DrtEotf::Gamma22});
  SetSharpen(document, 50.0F);
  DrtAdjustment<HalationModel>(document, type_ids::Halation()).SetValue(40.0F);
  const auto         plan = Compile(document, input);
  VulkanRenderDevice device;
  (void)Render(device, plan, input, document);  // creates the pipelines

  auto& backend = device.Workspace().Device();
  device.BeginRender();
  ExecuteVulkanDevelop(device, plan, input, document);
  ExecuteVulkanGeometryResample(device, plan);
  ExecuteVulkanCameraColor(device, plan, document);
  const auto scene   = ExecuteVulkanPrimaryGrade(device, plan, input, document).output_binding;
  const auto linear  = ExecuteVulkanDiffusionFilter(device, plan, document, scene);
  const auto submits = backend.SubmitCount();
  const auto waits   = backend.WaitCount();
  const auto before  = vulkan::ReadVulkanApiCounters();
  const auto result  = ExecuteVulkanDrt(device, plan, document, linear);
  const auto delta   = vulkan::VulkanApiCounterDelta(before, vulkan::ReadVulkanApiCounters());
  EXPECT_TRUE(device.CommandContext().IsRecording());
  EXPECT_EQ(backend.SubmitCount(), submits);
  EXPECT_EQ(backend.WaitCount(), waits);
  EXPECT_EQ(delta.submits, 0U);
  EXPECT_EQ(result.post_neighborhood_count, 2U);
  EXPECT_EQ(result.trace.enabled_neighborhood_count, 2U);
  EXPECT_FALSE(result.trace.copy_scene_to_post);
  // The display transform, then a horizontal and a vertical pass for each finish adjustment.
  EXPECT_EQ(delta.dispatches, 5U);
  EXPECT_EQ(delta.pipeline_creations, 0U);
  EXPECT_EQ(result.output, plan.display_output);
  device.EndRender();
  device.WaitIdle();
  device.PublishResults();
}

TEST_F(VulkanDrtTest, UnsupportedAndInvalidRequestsFailBeforeACommand) {
  const auto         input = MakeDisplayInput(64, 48);
  VulkanRenderDevice device;

  auto expect_failure = [&](const std::string& label, const PipelineDocument& document,
                            const char* phase) {
    const auto plan = Compile(document, input);
    device.BeginRender();
    ExecuteVulkanDevelop(device, plan, input, document);
    ExecuteVulkanGeometryResample(device, plan);
    ExecuteVulkanCameraColor(device, plan, document);
    const auto scene  = ExecuteVulkanPrimaryGrade(device, plan, input, document).output_binding;
    const auto linear = ExecuteVulkanDiffusionFilter(device, plan, document, scene);
    const auto before = vulkan::ReadVulkanApiCounters();
    try {
      (void)ExecuteVulkanDrt(device, plan, document, linear);
      ADD_FAILURE() << label << ": no error";
    } catch (const std::runtime_error& error) {
      std::printf("%s: %s\n", label.c_str(), error.what());
      if (phase != nullptr) {
        EXPECT_NE(std::string(error.what()).find(phase), std::string::npos) << error.what();
      }
    }
    const auto delta = vulkan::VulkanApiCounterDelta(before, vulkan::ReadVulkanApiCounters());
    EXPECT_EQ(delta.dispatches, 0U) << label;
    device.CancelRender();
  };

  {
    auto document = MakeDocument();
    DrtAdjustment<FilmGrainModel>(document, type_ids::FilmGrain()).SetValue(0.5F);
    expect_failure("Film Grain of the finish", document, "VK14");
  }
  {
    auto document  = MakeDocument();
    auto payload   = document.Drt()->Params().Params();
    payload.method = DrtMethod::FilmPrint;
    document.Drt()->Params().ReplaceParams(payload);
    // Without the film stock resources the output resolution fails first; both fail before a
    // command.
    expect_failure("film print", document, nullptr);
  }

  // Outside BeginRender, and without a DRT endpoint.
  const auto       document = MakeDocument();
  const auto       plan     = Compile(document, input);
  PipelineDocument empty;
  EXPECT_THROW((void)ExecuteVulkanDrt(device, plan, document,
                                      FrameSceneBinding::WorkImage(SceneWorkMember::Member0)),
               std::runtime_error);
  device.BeginRender();
  EXPECT_THROW((void)ExecuteVulkanDrt(device, plan, empty,
                                      FrameSceneBinding::WorkImage(SceneWorkMember::Member0)),
               std::runtime_error);
  device.CancelRender();
}

}  // namespace
}  // namespace alcedo
