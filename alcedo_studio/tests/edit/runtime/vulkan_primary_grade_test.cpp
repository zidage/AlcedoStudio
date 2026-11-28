//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// The point stages of the Color Grade on the Vulkan backend (#1036, phase VK8): each point
// adjustment, the mix and the mask, a chain of Grades, and the curve input histogram against the
// same passes of the reference backend on the same GPU (decision B9) and the host counts; the LUT
// cache by content key; the recording and failure rules of the pass. The default Grade has no
// neighborhood adjustment (Sharpen, Clarity, and Halation are in the DRT finish, phase VK9).

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "../graph/grade_owned_mask_support.hpp"
#include "../graph/test_camera_profile.hpp"
#include "../input/prepared_raw_test_support.hpp"
#include "curve_input_histogram_test_support.hpp"
#include "edit/graph/color_grade_node_model.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/input/raw_input_loader.hpp"
#include "edit/mask/mask_model.hpp"
#include "edit/operators/models/cat02_white_balance_model.hpp"
#include "edit/operators/models/color_wheel_model.hpp"
#include "edit/operators/models/curve_model.hpp"
#include "edit/operators/models/hls_model.hpp"
#include "edit/operators/models/lmt_model.hpp"
#include "edit/operators/models/scalar_operator_model.hpp"
#include "edit/runtime/curve_input_histogram.hpp"
#include "edit/runtime/graph_compiler.hpp"
#include "edit/runtime/texture_format.hpp"
#include "edit/runtime/vulkan/vulkan_develop_pass.hpp"
#include "edit/runtime/vulkan/vulkan_geometry_camera_pass.hpp"
#include "edit/runtime/vulkan/vulkan_pass_encoder.hpp"
#include "edit/runtime/vulkan/vulkan_primary_grade_pass.hpp"
#include "multi_grade_runtime_test_support.hpp"
#include "tone_curve_test_curves.hpp"
#include "vulkan_reference_backend_support.hpp"
#include "vulkan_workspace_test_support.hpp"

namespace alcedo {
namespace {

using namespace vulkan_reference;

/// The OpenCL grade test bound of a Grade against the host evaluation (ACEScc).
constexpr double        kGradeTolerance = 2.0e-4;
/// The OpenCL curve test bound against the spline reference (kGpuCurveTolerance).
constexpr double        kCurveTolerance = 1.0e-5;

constexpr std::uint32_t kWidth          = 64;
constexpr std::uint32_t kHeight         = 48;

/// Scene-linear camera RGB from 2^-7 to 2^2 along x, with hues that change along y: ACEScc from
/// about 0.15 to 0.67 with saturated and neutral texels.
auto MakeGradePlane(std::uint32_t width, std::uint32_t height) -> HostImagePlane {
  HostImagePlane plane;
  plane.extent       = {width, height};
  plane.stride_bytes = width * 16U;
  plane.format       = HostPixelFormat::F32Rgba;
  auto  storage      = std::shared_ptr<std::byte>(new std::byte[plane.ByteCount()],
                                                  [](std::byte* pointer) { delete[] pointer; });
  auto* pixels       = reinterpret_cast<float*>(storage.get());
  for (std::uint32_t y = 0; y < height; ++y) {
    const float hue = 6.2831853F * static_cast<float>(y) / static_cast<float>(height);
    const float sat = static_cast<float>(y % 4) / 3.0F;
    for (std::uint32_t x = 0; x < width; ++x) {
      const float level =
          std::exp2(-7.0F + 9.0F * static_cast<float>(x) / static_cast<float>(width - 1));
      const auto index  = (static_cast<std::size_t>(y) * width + x) * 4;
      pixels[index + 0] = level * (1.0F + 0.6F * sat * std::cos(hue));
      pixels[index + 1] = level * (1.0F + 0.6F * sat * std::cos(hue - 2.0943951F));
      pixels[index + 2] = level * (1.0F + 0.6F * sat * std::cos(hue + 2.0943951F));
      pixels[index + 3] = 1.0F;
    }
  }
  plane.bytes = std::const_pointer_cast<const std::byte>(storage);
  return plane;
}

auto MakeGradeInput(std::uint32_t width = kWidth, std::uint32_t height = kHeight)
    -> PreparedRawInput {
  return RawInputLoader::FromDirectRgb(MakeGradePlane(width, height),
                                       gpu_dag_test::FullSensor(width, height));
}

auto Compile(const PipelineDocument& document, const PreparedRawInput& input) -> ExecutionPlan {
  return GraphCompiler::Compile(document, input.CompileSource(), RenderRequest{});
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

/// The images of one render through the Color Grades.
struct GradeImages {
  std::vector<float> develop;
  std::vector<float> output;
  FrameSceneBinding  binding{};
};

/// Develop, GeometryResample, CameraToAp1, then @p grade on @p device. With @p mask, the mask
/// output of the first Grade is that R8 image, the same on both backends (VulkanMaskTest compares
/// MaskEvaluate).
template <class Device, class Grade>
auto RenderGrades(Device& device, const ExecutionPlan& plan, const PreparedRawInput& input,
                  const PipelineDocument& document, Grade grade,
                  const std::vector<std::byte>* mask = nullptr) -> GradeImages {
  device.BeginRender();
  GradeImages images;
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
    images.binding = grade(device);
    device.EndRender();
    device.WaitIdle();
  } catch (...) {
    device.CancelRender();
    throw;
  }
  images.develop = DownloadImage(device, plan.develop_output);
  images.output  = DownloadScene(device, images.binding);
  device.PublishResults();
  return images;
}

auto RenderVulkan(VulkanRenderDevice& device, const ExecutionPlan& plan,
                  const PreparedRawInput& input, const PipelineDocument& document,
                  const std::vector<std::byte>* mask      = nullptr,
                  VulkanPrimaryGradeResult*     result    = nullptr,
                  CurveInputHistogramRecord*    histogram = nullptr) -> GradeImages {
  return RenderGrades(
      device, plan, input, document,
      [&](VulkanRenderDevice& d) {
        const auto executed = ExecuteVulkanPrimaryGrade(d, plan, input, document, histogram);
        if (result != nullptr) {
          *result = executed;
        }
        return executed.output_binding;
      },
      mask);
}

auto RenderReference(const ExecutionPlan& plan, const PreparedRawInput& input,
                     const PipelineDocument& document, const std::vector<std::byte>* mask = nullptr)
    -> GradeImages {
  ReferenceRenderDevice device;
  return RenderGrades(
      device, plan, input, document,
      [&](ReferenceRenderDevice& d) {
        return ExecuteReferencePrimaryGrade(d, plan, input, document);
      },
      mask);
}

/// Renders @p document on Vulkan and on the reference backend and expects equal Grade outputs.
void ExpectGradeEqualsReference(const std::string& label, const PipelineDocument& document,
                                const PreparedRawInput& input, double tolerance,
                                const std::vector<std::byte>* mask = nullptr) {
  const auto         plan = Compile(document, input);
  VulkanRenderDevice device;
  const auto         vulkan    = RenderVulkan(device, plan, input, document, mask);
  const auto         reference = RenderReference(plan, input, document, mask);
  ASSERT_EQ(vulkan.develop.size(), reference.develop.size()) << label;
  ExpectEqualToReference(label, reference.output, vulkan.output, plan.geometry.render_extent.width,
                         Uniform(tolerance));
  // The Grade changed the image: the case tests the adjustment, not an alias.
  double change = 0.0;
  for (std::size_t i = 0; i < vulkan.output.size(); ++i) {
    change = std::max(change, std::abs(static_cast<double>(vulkan.output[i]) - vulkan.develop[i]));
  }
  EXPECT_GT(change, 1.0e-3) << label << ": the Grade output equals its input";
}

template <class Model>
auto PrimaryAdjustment(PipelineDocument& document, const OperatorTypeId& type) -> Model& {
  return multi_grade_test::GradeAdjustment<Model>(document, document.PrimaryGrade()->Id(), type);
}

/// A 17^3 cube with cross-talk and a power on each channel (a smooth table that trilinear
/// interpolation does not reproduce exactly between the voxels).
void WriteSmoothCube(const std::filesystem::path& path, float power) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream out(path);
  constexpr int kEdge = 17;
  out << "LUT_3D_SIZE " << kEdge << '\n';
  for (int b = 0; b < kEdge; ++b) {
    for (int g = 0; g < kEdge; ++g) {
      for (int r = 0; r < kEdge; ++r) {
        const float fr = static_cast<float>(r) / (kEdge - 1);
        const float fg = static_cast<float>(g) / (kEdge - 1);
        const float fb = static_cast<float>(b) / (kEdge - 1);
        out << 0.85F * std::pow(fr, power) + 0.15F * fg << ' '
            << 0.8F * std::pow(fg, power) + 0.1F * fr + 0.1F * fb << ' '
            << 0.9F * std::pow(fb, power) + 0.1F * fg << '\n';
      }
    }
  }
}

auto TempPath(const std::string& name) -> std::filesystem::path {
  return std::filesystem::absolute("build/tmp/vulkan_primary_grade") / name;
}

void UseCube(PipelineDocument& document, const std::filesystem::path& path, float strength) {
  auto& lmt = PrimaryAdjustment<LmtModel>(document, type_ids::Lmt());
  lmt.SetCubePath(path.string());
  lmt.SetStrength(strength);
}

class VulkanPrimaryGradeTest : public vulkan_workspace_test::VulkanWorkspaceFixture {
 protected:
  void SetUp() override {
    VulkanWorkspaceFixture::SetUp();
    const std::string reason = PrepareReferenceBackend();
    ASSERT_TRUE(reason.empty()) << reason;
  }
};

TEST_F(VulkanPrimaryGradeTest, EachPointAdjustmentEqualsTheReferenceBackend) {
  const auto input = MakeGradeInput();
  const auto cube  = TempPath("smooth.cube");
  WriteSmoothCube(cube, 0.8F);

  struct Case {
    const char*                            name;
    double                                 tolerance;
    std::function<void(PipelineDocument&)> apply;
  };
  const auto points = [](std::size_t count, float lift) {
    return tone_curve_test::LiftedMidtonePoints(count, 0.15F, 0.67F, lift);
  };
  const std::vector<Case> cases = {
      {"white balance 4300 K", kGradeTolerance,
       [](PipelineDocument& d) {
         PrimaryAdjustment<Cat02WhiteBalanceModel>(d, type_ids::Cat02WhiteBalance())
             .SetTemperature(4300.0F);
       }},
      {"exposure +1.3", kGradeTolerance,
       [](PipelineDocument& d) {
         PrimaryAdjustment<ExposureModel>(d, type_ids::Exposure()).SetValue(1.3F);
       }},
      {"contrast +100", kGradeTolerance,
       [](PipelineDocument& d) {
         PrimaryAdjustment<ContrastModel>(d, type_ids::Contrast()).SetValue(100.0F);
       }},
      {"contrast -40", kGradeTolerance,
       [](PipelineDocument& d) {
         PrimaryAdjustment<ContrastModel>(d, type_ids::Contrast()).SetValue(-40.0F);
       }},
      {"whites +60", kGradeTolerance,
       [](PipelineDocument& d) {
         PrimaryAdjustment<WhiteModel>(d, type_ids::White()).SetValue(60.0F);
       }},
      {"whites -70", kGradeTolerance,
       [](PipelineDocument& d) {
         PrimaryAdjustment<WhiteModel>(d, type_ids::White()).SetValue(-70.0F);
       }},
      {"blacks -45", kGradeTolerance,
       [](PipelineDocument& d) {
         PrimaryAdjustment<BlackModel>(d, type_ids::Black()).SetValue(-45.0F);
       }},
      {"curve, 16 points", kCurveTolerance,
       [&](PipelineDocument& d) {
         PrimaryAdjustment<CurveModel>(d, type_ids::Curve()).SetPoints(points(16, 0.2F));
       }},
      {"curve, RGB with red and blue", kCurveTolerance,
       [&](PipelineDocument& d) {
         CurveUpdate update;
         update.points = points(4, 0.2F);
         update.red    = points(5, 0.25F);
         update.blue   = points(6, -0.15F);
         PrimaryAdjustment<CurveModel>(d, type_ids::Curve()).ApplyUpdate(update);
       }},
      {"HLS", kGradeTolerance,
       [](PipelineDocument& d) {
         auto& hls   = PrimaryAdjustment<HlsModel>(d, type_ids::Hls());
         auto  table = hls.AdjustmentTable();
         table[0].h  = 0.3F;
         table[2].s  = 0.5F;
         table[4].l  = -0.2F;
         table[5].s  = -0.4F;
         HlsUpdate update;
         update.hls_adj_table = table;
         hls.ApplyUpdate(update);
       }},
      {"saturation 1.6", kGradeTolerance,
       [](PipelineDocument& d) {
         PrimaryAdjustment<SaturationModel>(d, type_ids::Saturation()).SetValue(1.6F);
       }},
      {"vibrance +50", kGradeTolerance,
       [](PipelineDocument& d) {
         PrimaryAdjustment<VibranceModel>(d, type_ids::Vibrance()).SetValue(50.0F);
       }},
      {"color wheels", kGradeTolerance,
       [](PipelineDocument& d) {
         ColorWheelUpdate        wheels;
         ColorWheelControlUpdate lift;
         lift.color_offset = Vec3f{0.4F, -0.2F, -0.2F};
         ColorWheelControlUpdate gamma;
         gamma.luminance_offset = 0.5F;
         ColorWheelControlUpdate gain;
         gain.color_offset = Vec3f{-0.3F, 0.15F, 0.15F};
         ColorWheelControlUpdate offset;
         offset.luminance_offset = -0.25F;
         wheels.lift             = lift;
         wheels.gamma            = gamma;
         wheels.gain             = gain;
         wheels.offset           = offset;
         PrimaryAdjustment<ColorWheelModel>(d, type_ids::ColorWheel()).ApplyUpdate(wheels);
       }},
      {"LUT, strength 1", kGradeTolerance, [&](PipelineDocument& d) { UseCube(d, cube, 1.0F); }},
      {"LUT, strength 0.5", kGradeTolerance, [&](PipelineDocument& d) { UseCube(d, cube, 0.5F); }},
      {"every point adjustment", kGradeTolerance,
       [&](PipelineDocument& d) {
         PrimaryAdjustment<Cat02WhiteBalanceModel>(d, type_ids::Cat02WhiteBalance())
             .SetTemperature(6100.0F);
         PrimaryAdjustment<ExposureModel>(d, type_ids::Exposure()).SetValue(0.5F);
         PrimaryAdjustment<ContrastModel>(d, type_ids::Contrast()).SetValue(40.0F);
         PrimaryAdjustment<WhiteModel>(d, type_ids::White()).SetValue(30.0F);
         PrimaryAdjustment<BlackModel>(d, type_ids::Black()).SetValue(20.0F);
         PrimaryAdjustment<CurveModel>(d, type_ids::Curve()).SetPoints(points(6, 0.1F));
         PrimaryAdjustment<SaturationModel>(d, type_ids::Saturation()).SetValue(1.2F);
         PrimaryAdjustment<VibranceModel>(d, type_ids::Vibrance()).SetValue(-30.0F);
         UseCube(d, cube, 0.7F);
       }},
  };
  for (const auto& c : cases) {
    SCOPED_TRACE(c.name);
    auto document = multi_grade_test::MakeIdentityGradeDocument();
    c.apply(document);
    ExpectGradeEqualsReference(c.name, document, input, c.tolerance);
  }
}

TEST_F(VulkanPrimaryGradeTest, GradeMixAndMaskEqualTheReferenceBackend) {
  const auto input = MakeGradeInput();
  {
    auto document = multi_grade_test::MakeIdentityGradeDocument();
    PrimaryAdjustment<ContrastModel>(document, type_ids::Contrast()).SetValue(80.0F);
    PrimaryAdjustment<ExposureModel>(document, type_ids::Exposure()).SetValue(-0.8F);
    document.PrimaryGrade()->SetMix(0.6F);
    ExpectGradeEqualsReference("mix 0.6", document, input, kGradeTolerance);
  }
  {
    auto document = multi_grade_test::MakeIdentityGradeDocument();
    PrimaryAdjustment<ContrastModel>(document, type_ids::Contrast()).SetValue(80.0F);
    PrimaryAdjustment<ExposureModel>(document, type_ids::Exposure()).SetValue(-0.8F);
    document.PrimaryGrade()->SetMix(0.8F);
    MaskModel mask;
    mask.id     = MaskId{"mask.radial"};
    mask.source = RadialMaskSource{};
    (void)grade_mask_test::AddMask(*document.PrimaryGrade(), std::move(mask));
    document.MarkTopologyChanged();
    const auto plan = Compile(document, input);
    ASSERT_NE(plan.FirstGrade(), nullptr);
    ASSERT_TRUE(plan.FirstGrade()->mask_stack.has_value());
    const auto bytes = MakeMaskBytes(kWidth, kHeight);
    ExpectGradeEqualsReference("mix 0.8 with a mask", document, input, kGradeTolerance, &bytes);
  }
}

TEST_F(VulkanPrimaryGradeTest, ThreeGradesComposeLikeTheReferenceBackend) {
  const auto input    = MakeGradeInput();
  auto       document = multi_grade_test::MakeIdentityGradeDocument();
  multi_grade_test::AddCleanGradesBeforeDrt(document, {"grade.b", "grade.c"});
  multi_grade_test::GradeAdjustment<ExposureModel>(document, NodeId{"grade.primary"},
                                                   type_ids::Exposure())
      .SetValue(1.0F);
  multi_grade_test::GradeAdjustment<ContrastModel>(document, NodeId{"grade.b"},
                                                   type_ids::Contrast())
      .SetValue(100.0F);
  multi_grade_test::GradeNode(document, "grade.b")->SetMix(0.5F);
  multi_grade_test::GradeAdjustment<SaturationModel>(document, NodeId{"grade.c"},
                                                     type_ids::Saturation())
      .SetValue(1.4F);
  const auto plan = Compile(document, input);
  ASSERT_EQ(plan.grade_nodes.size(), 3U);
  VulkanRenderDevice       device;
  VulkanPrimaryGradeResult result;
  const auto               vulkan = RenderVulkan(device, plan, input, document, nullptr, &result);
  const auto               reference = RenderReference(plan, input, document);
  // Member0, Member1, Member0: the third Grade writes the first scene-work member.
  ASSERT_TRUE(vulkan.binding.IsWorkImage());
  EXPECT_EQ(vulkan.binding.member, SceneWorkMember::Member0);
  ExpectEqualToReference("three Grades", reference.output, vulkan.output, kWidth,
                         Uniform(kGradeTolerance));
}

TEST_F(VulkanPrimaryGradeTest, DisabledAndZeroMixGradesAliasTheirInput) {
  const auto input = MakeGradeInput();
  for (const bool disable : {true, false}) {
    auto document = multi_grade_test::MakeIdentityGradeDocument();
    PrimaryAdjustment<ExposureModel>(document, type_ids::Exposure()).SetValue(1.0F);
    if (disable) {
      document.PrimaryGrade()->SetEnabled(false);
    } else {
      document.PrimaryGrade()->SetMix(0.0F);
    }
    const auto plan = Compile(document, input);
    if (plan.grade_nodes.empty()) {
      continue;  // A disabled Grade can leave the plan.
    }
    VulkanRenderDevice       device;
    VulkanPrimaryGradeResult result;
    const auto               images = RenderVulkan(device, plan, input, document, nullptr, &result);
    EXPECT_EQ(images.binding, FrameSceneBinding::CachedImage(plan.develop_output))
        << (disable ? "disabled" : "zero mix");
    EXPECT_EQ(result.pointwise_dispatch_count, 0U);
    EXPECT_EQ(images.output, images.develop);
  }
}

TEST_F(VulkanPrimaryGradeTest, CurveInputHistogramCountsTheGradeInputOnTheCurveAxis) {
  // 640 x 480: more samples than one pass of the 128 workgroups of 256 invocations.
  const auto      input     = MakeGradeInput(640, 480);
  auto            document  = multi_grade_test::MakeIdentityGradeDocument();
  constexpr float kExposure = 0.7F;
  for (const float exposure : {0.0F, kExposure}) {
    PrimaryAdjustment<ExposureModel>(document, type_ids::Exposure()).SetValue(exposure);
    PrimaryAdjustment<CurveModel>(document, type_ids::Curve())
        .SetPoints(tone_curve_test::LiftedMidtonePoints(5, 0.15F, 0.67F, 0.2F));
    const auto                plan = Compile(document, input);
    VulkanRenderDevice        device;
    CurveInputHistogramRecord record{document.PrimaryGrade()->Id()};
    const auto  images = RenderVulkan(device, plan, input, document, nullptr, nullptr, &record);
    const auto& extent = plan.geometry.render_extent;
    ASSERT_TRUE(record.recorded);
    EXPECT_EQ(record.sample_count, CurveInputHistogramSampleCount(extent.width, extent.height));
    const auto counts = ReadCurveInputHistogramCounts(device, record.grade_id);
    struct Rgb {
      float r = 0.0F;
      float g = 0.0F;
      float b = 0.0F;
    };
    std::vector<Rgb> develop(images.develop.size() / 4);
    for (std::size_t i = 0; i < develop.size(); ++i) {
      develop[i] = {images.develop[i * 4], images.develop[i * 4 + 1], images.develop[i * 4 + 2]};
    }
    if (exposure == 0.0F) {
      EXPECT_EQ(counts,
                curve_histogram_test::ReferenceCounts(develop, extent.width, extent.height));
    } else {
      curve_histogram_test::ExpectCountsOfShiftedPixels(
          counts, develop, extent.width, extent.height, exposure / 17.52F, "exposure");
    }
  }
}

TEST_F(VulkanPrimaryGradeTest, LutResourceIsReusedByContentKey) {
  const auto input       = MakeGradeInput();
  const auto first_cube  = TempPath("reuse_a.cube");
  const auto second_cube = TempPath("reuse_b.cube");
  WriteSmoothCube(first_cube, 0.8F);
  WriteSmoothCube(second_cube, 1.25F);
  auto document = multi_grade_test::MakeIdentityGradeDocument();
  UseCube(document, first_cube, 1.0F);
  VulkanRenderDevice       device;
  auto&                    backend = device.Workspace().Device();

  VulkanPrimaryGradeResult first;
  (void)RenderVulkan(device, Compile(document, input), input, document, nullptr, &first);
  EXPECT_NE(first.lut_resource_id, 0U);
  EXPECT_EQ(backend.LutCacheEntryCount(), 1U);

  // An edit of another adjustment reads the cached table: no upload and no new buffer.
  PrimaryAdjustment<ExposureModel>(document, type_ids::Exposure()).SetValue(0.25F);
  backend.ResetCounters();
  VulkanPrimaryGradeResult second;
  (void)RenderVulkan(device, Compile(document, input), input, document, nullptr, &second);
  EXPECT_EQ(second.lut_resource_id, first.lut_resource_id);
  EXPECT_EQ(backend.LutUploadBytes(), 0U);
  EXPECT_EQ(backend.LastLutResourceId(), first.lut_resource_id);

  // Another table is a new entry; with a budget of one table the older entry leaves the cache.
  UseCube(document, second_cube, 1.0F);
  backend.SetLutByteBudget(17U * 17U * 17U * 16U);
  VulkanPrimaryGradeResult third;
  (void)RenderVulkan(device, Compile(document, input), input, document, nullptr, &third);
  EXPECT_NE(third.lut_resource_id, first.lut_resource_id);
  EXPECT_EQ(backend.LutUploadBytes(), 17U * 17U * 17U * 16U);
  EXPECT_EQ(backend.LutCacheEntryCount(), 1U);

  // A Grade without a LUT binds the dummy LUT and uploads nothing.
  auto& lmt = PrimaryAdjustment<LmtModel>(document, type_ids::Lmt());
  lmt.SetStrength(0.0F);
  backend.ResetCounters();
  VulkanPrimaryGradeResult none;
  (void)RenderVulkan(device, Compile(document, input), input, document, nullptr, &none);
  EXPECT_EQ(none.lut_resource_id, backend.DummyLut().resource_id);
  EXPECT_EQ(backend.LutUploadBytes(), 0U);
}

TEST_F(VulkanPrimaryGradeTest, StableGradeUploadsNoCommandsAndCreatesNoResource) {
  const auto input    = MakeGradeInput();
  auto       document = multi_grade_test::MakeIdentityGradeDocument();
  PrimaryAdjustment<ExposureModel>(document, type_ids::Exposure()).SetValue(0.5F);
  VulkanRenderDevice       device;
  VulkanPrimaryGradeResult first;
  (void)RenderVulkan(device, Compile(document, input), input, document, nullptr, &first);
  EXPECT_GT(first.command_upload_bytes, 0U);

  PrimaryAdjustment<ExposureModel>(document, type_ids::Exposure()).SetValue(0.75F);
  const auto plan = Compile(document, input);
  device.BeginRender();
  ExecuteVulkanDevelop(device, plan, input, document);
  ExecuteVulkanGeometryResample(device, plan);
  ExecuteVulkanCameraColor(device, plan, document);
  auto& backend = device.Workspace().Device();
  backend.ResetCounters();
  const auto second = ExecuteVulkanPrimaryGrade(device, plan, input, document);
  EXPECT_EQ(second.command_upload_bytes, 0U);
  EXPECT_EQ(backend.BufferCreateCount(), 0U);
  EXPECT_EQ(backend.TextureCreateCount(), 0U);
  device.EndRender();
  device.WaitIdle();
  device.PublishResults();
}

TEST_F(VulkanPrimaryGradeTest, GradeRecordsIntoTheOpenRecordingWithoutASubmission) {
  const auto input    = MakeGradeInput();
  auto       document = multi_grade_test::MakeIdentityGradeDocument();
  PrimaryAdjustment<ContrastModel>(document, type_ids::Contrast()).SetValue(50.0F);
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

TEST_F(VulkanPrimaryGradeTest, InvalidRequestsFail) {
  const auto input    = MakeGradeInput();
  auto       document = multi_grade_test::MakeIdentityGradeDocument();
  PrimaryAdjustment<ExposureModel>(document, type_ids::Exposure()).SetValue(0.5F);
  {
    const auto         plan = Compile(document, input);
    VulkanRenderDevice device;
    EXPECT_THROW((void)ExecuteVulkanPrimaryGrade(device, plan, input, document),
                 std::runtime_error);
    EXPECT_THROW(
        (void)ExecuteVulkanPrimaryGrade(device, plan, input, document, *plan.FirstGrade(),
                                        FrameSceneBinding::CachedImage(plan.develop_output)),
        std::runtime_error);
    auto no_grades = plan;
    no_grades.grade_nodes.clear();
    device.BeginRender();
    EXPECT_THROW((void)ExecuteVulkanPrimaryGrade(device, no_grades, input, document),
                 std::runtime_error);
    device.CancelRender();
  }
  const auto expect_failure = [&](const PipelineDocument& failing, const std::string& message) {
    const auto         plan = Compile(failing, input);
    VulkanRenderDevice device;
    try {
      (void)RenderVulkan(device, plan, input, failing);
      ADD_FAILURE() << "the render did not fail: " << message;
    } catch (const std::runtime_error& error) {
      EXPECT_NE(std::string(error.what()).find(message), std::string::npos) << error.what();
    }
  };
  {
    // A Grade with a mask whose output the render does not have.
    auto      failing = multi_grade_test::MakeIdentityGradeDocument();
    MaskModel mask;
    mask.id     = MaskId{"mask.radial"};
    mask.source = RadialMaskSource{};
    (void)grade_mask_test::AddMask(*failing.PrimaryGrade(), std::move(mask));
    failing.PrimaryGrade()->SetMix(0.5F);
    PrimaryAdjustment<ExposureModel>(failing, type_ids::Exposure()).SetValue(0.5F);
    failing.MarkTopologyChanged();
    expect_failure(failing, "mask output is missing");
  }
}

}  // namespace
}  // namespace alcedo
