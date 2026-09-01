//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "../graph/test_camera_profile.hpp"
#include "../input/prepared_raw_test_support.hpp"
#include "edit/geometry/texture_sampling_plan.hpp"
#include "edit/graph/analytic_mask_node_model.hpp"
#include "edit/graph/color_grade_node_model.hpp"
#include "edit/graph/raster_mask_node_model.hpp"
#include "edit/input/raw_input_loader.hpp"
#include "edit/mask/mask_asset.hpp"
#include "edit/mask/mask_store.hpp"
#include "edit/operators/models/builtin_type_ids.hpp"
#include "edit/operators/models/scalar_operator_model.hpp"
#include "edit/runtime/graph_compiler.hpp"
#include "edit/runtime/opencl/opencl_develop_pass.hpp"
#include "edit/runtime/opencl/opencl_mask_pass.hpp"
#include "edit/runtime/opencl/opencl_pass_encoder.hpp"
#include "edit/runtime/opencl/opencl_primary_grade_pass.hpp"
#include "opencl/opencl_runtime.hpp"

namespace alcedo {
namespace {

struct Rgba {
  float r = 0.0f;
  float g = 0.0f;
  float b = 0.0f;
  float a = 1.0f;
};

auto Transform(const Matrix3x3& matrix, float x, float y) -> Vector2 {
  return {matrix.m[0] * x + matrix.m[1] * y + matrix.m[2],
          matrix.m[3] * x + matrix.m[4] * y + matrix.m[5]};
}

auto SampleR8(const std::vector<std::uint8_t>& pixels, std::uint32_t width, std::uint32_t height,
              float u, float v) -> float {
  if (u < 0.0f || v < 0.0f || u > 1.0f || v > 1.0f) {
    return 0.0f;
  }
  const float x  = u * static_cast<float>(width) - 0.5f;
  const float y  = v * static_cast<float>(height) - 0.5f;
  const int   x0 = std::clamp(static_cast<int>(std::floor(x)), 0, static_cast<int>(width) - 1);
  const int   y0 = std::clamp(static_cast<int>(std::floor(y)), 0, static_cast<int>(height) - 1);
  const int   x1 = std::min(x0 + 1, static_cast<int>(width) - 1);
  const int   y1 = std::min(y0 + 1, static_cast<int>(height) - 1);
  const float tx = std::clamp(x - std::floor(x), 0.0f, 1.0f);
  const float ty = std::clamp(y - std::floor(y), 0.0f, 1.0f);
  const float a =
      static_cast<float>(pixels[static_cast<std::size_t>(y0) * width + x0]) * (1.0f - tx) +
      static_cast<float>(pixels[static_cast<std::size_t>(y0) * width + x1]) * tx;
  const float b =
      static_cast<float>(pixels[static_cast<std::size_t>(y1) * width + x0]) * (1.0f - tx) +
      static_cast<float>(pixels[static_cast<std::size_t>(y1) * width + x1]) * tx;
  return (a * (1.0f - ty) + b * ty) / 255.0f;
}

auto CpuAnalytic(const AnalyticMaskNodeModel& node, const ResolvedRenderGeometry& geometry,
                 std::uint32_t x, std::uint32_t y) -> float {
  const auto  reference = Transform(geometry.render_to_reference, static_cast<float>(x) + 0.5f,
                                    static_cast<float>(y) + 0.5f);
  const float nx        = reference.x / static_cast<float>(geometry.full_reference_extent.width);
  const float ny        = reference.y / static_cast<float>(geometry.full_reference_extent.height);
  float       value;
  bool        invert;
  if (node.Kind() == AnalyticMaskKind::Radial) {
    const auto& radial = node.Radial();
    const float c      = std::cos(radial.rotation);
    const float s      = std::sin(radial.rotation);
    const float dx     = nx - radial.center_x;
    const float dy     = ny - radial.center_y;
    const float rx     = (c * dx + s * dy) / std::max(radial.major_radius, 1.0e-6f);
    const float ry     = (-s * dx + c * dy) / std::max(radial.minor_radius, 1.0e-6f);
    const float radius = std::sqrt(rx * rx + ry * ry);
    const float inner  = std::max(0.0f, 1.0f - radial.inner_feather);
    const float outer  = 1.0f + radial.outer_feather;
    value  = 1.0f - std::clamp((radius - inner) / std::max(outer - inner, 1.0e-6f), 0.0f, 1.0f);
    invert = radial.invert;
  } else {
    const auto& graduated = node.GraduatedNd();
    const float length    = std::hypot(graduated.normal_x, graduated.normal_y);
    const float normal_x  = graduated.normal_x / std::max(length, 1.0e-6f);
    const float normal_y  = graduated.normal_y / std::max(length, 1.0e-6f);
    const float distance =
        (nx - graduated.origin_x) * normal_x + (ny - graduated.origin_y) * normal_y;
    const float t =
        std::clamp(distance / std::max(graduated.transition_distance, 1.0e-6f) + 0.5f, 0.0f, 1.0f);
    value  = graduated.start_value + (graduated.end_value - graduated.start_value) * t;
    invert = graduated.invert;
  }
  return invert ? 1.0f - value : value;
}

auto BuildR8MipChain(const std::vector<std::uint8_t>& base, std::uint32_t width,
                     std::uint32_t height) -> std::vector<std::vector<std::uint8_t>> {
  std::vector<std::vector<std::uint8_t>> levels;
  levels.push_back(base);
  while (width != 1 || height != 1) {
    const auto                next_width  = std::max<std::uint32_t>(width / 2, 1);
    const auto                next_height = std::max<std::uint32_t>(height / 2, 1);
    std::vector<std::uint8_t> next(static_cast<std::size_t>(next_width) * next_height);
    for (std::uint32_t y = 0; y < next_height; ++y) {
      for (std::uint32_t x = 0; x < next_width; ++x) {
        std::uint32_t sum   = 0;
        std::uint32_t count = 0;
        for (std::uint32_t dy = 0; dy < 2; ++dy) {
          for (std::uint32_t dx = 0; dx < 2; ++dx) {
            const auto sx = x * 2 + dx;
            const auto sy = y * 2 + dy;
            if (sx < width && sy < height) {
              sum += levels.back()[static_cast<std::size_t>(sy) * width + sx];
              ++count;
            }
          }
        }
        next[static_cast<std::size_t>(y) * next_width + x] =
            static_cast<std::uint8_t>((sum + count / 2) / count);
      }
    }
    levels.push_back(std::move(next));
    width  = next_width;
    height = next_height;
  }
  return levels;
}

auto CpuExactFeather(const MaskAsset& asset, float radius_texels) -> std::vector<std::uint8_t> {
  const auto                width  = asset.descriptor.extent.width;
  const auto                height = asset.descriptor.extent.height;
  std::vector<std::uint8_t> output(asset.pixels.size());
  for (std::uint32_t y = 0; y < height; ++y) {
    for (std::uint32_t x = 0; x < width; ++x) {
      const auto  index    = static_cast<std::size_t>(y) * width + x;
      const float coverage = asset.pixels[index] / 255.0f;
      const bool  inside   = coverage >= 0.5f;
      float       signed_distance;
      if (coverage > 0.0f && coverage < 1.0f) {
        signed_distance = coverage - 0.5f;
      } else {
        float nearest_squared = (std::numeric_limits<float>::max)();
        for (std::uint32_t target_y = 0; target_y < height; ++target_y) {
          for (std::uint32_t target_x = 0; target_x < width; ++target_x) {
            const auto target_index = static_cast<std::size_t>(target_y) * width + target_x;
            if ((asset.pixels[target_index] >= 128) == inside) {
              continue;
            }
            const float dx  = static_cast<float>(x) - static_cast<float>(target_x);
            const float dy  = static_cast<float>(y) - static_cast<float>(target_y);
            nearest_squared = std::min(nearest_squared, dx * dx + dy * dy);
          }
        }
        const float exact       = std::sqrt(nearest_squared);
        const float to_boundary = std::max(exact - 0.5f, 0.0f);
        signed_distance         = inside ? to_boundary : -to_boundary;
      }
      float value   = radius_texels <= 0.0f
                          ? (signed_distance >= 0.0f ? 1.0f : 0.0f)
                          : std::clamp(0.5f + signed_distance / (2.0f * radius_texels), 0.0f, 1.0f);
      value         = value * value * (3.0f - 2.0f * value);
      output[index] = static_cast<std::uint8_t>(std::clamp(value * 255.0f + 0.5f, 0.0f, 255.0f));
    }
  }
  return output;
}

struct GradeFrame {
  OpenClPrimaryGradeResult result;
  std::vector<Rgba>        source;
  std::vector<Rgba>        output;
};

class OpenClMaskFixture : public ::testing::Test {
 protected:
  void SetUp() override {
    if (!TryInitializeOpenClRuntime()) {
      GTEST_SKIP() << "No OpenCL device available.";
    }
    device_ = std::make_unique<OpenClRenderDevice>();
    SetExtent(16, 12);
    root_ = std::filesystem::path{"build/tmp/o4_opencl_mask"} /
            ::testing::UnitTest::GetInstance()->current_test_info()->name();
    std::error_code ignored;
    std::filesystem::remove_all(root_, ignored);
    store_ = std::make_unique<MaskStore>(root_);
  }

  void SetExtent(std::uint32_t width, std::uint32_t height) {
    width_    = width;
    height_   = height;
    prepared_ = RawInputLoader::FromDirectRgb(gpu_dag_test::MakeF32RgbaPlane(width, height),
                                              gpu_dag_test::FullSensor(width, height));
    document_ = CreateDefaultPipelineDocument();
    gpu_dag_test::EnsureTestCameraProfile(document_);
  }

  auto MakeRaster(std::string key, std::uint8_t fill = 0) -> MaskAsset {
    MaskAsset asset;
    asset.key                         = MaskAssetKey{std::move(key)};
    asset.descriptor.extent           = {width_, height_};
    asset.descriptor.reference_bounds = {};
    asset.pixels.assign(static_cast<std::size_t>(width_) * height_, fill);
    return asset;
  }

  auto AttachRaster(MaskAsset asset, float feather = 0.0f) -> RasterMaskNodeModel& {
    store_->Save(asset);
    auto node = std::make_unique<RasterMaskNodeModel>(NodeId{"mask.raster"});
    node->SetAssetKey(asset.key);
    node->SetReferenceBounds({});
    node->SetFeatherRadius(feather);
    auto* result = node.get();
    document_.Graph().AddNode(std::move(node));
    document_.Graph().Connect(NodeId{"mask.raster"}, PortId{"mask"}, NodeId{"grade.primary"},
                              PortId{"mask"});
    document_.MarkTopologyDirty();
    return *result;
  }

  auto AttachAnalytic(AnalyticMaskKind kind) -> AnalyticMaskNodeModel& {
    auto  node   = std::make_unique<AnalyticMaskNodeModel>(NodeId{"mask.analytic"}, kind);
    auto* result = node.get();
    document_.Graph().AddNode(std::move(node));
    document_.Graph().Connect(NodeId{"mask.analytic"}, PortId{"mask"}, NodeId{"grade.primary"},
                              PortId{"mask"});
    document_.MarkTopologyDirty();
    return *result;
  }

  void Compile(RenderRequest request = {}) {
    plan_ = GraphCompiler::Compile(document_, prepared_.CompileSource(), request);
  }

  auto DownloadMask() -> std::vector<std::uint8_t> {
    auto* lease = device_->Workspace().Images().Find(plan_.FirstGrade()->mask_output);
    EXPECT_NE(lease, nullptr);
    if (lease == nullptr) {
      return {};
    }
    std::vector<std::uint8_t> pixels(lease->Texture().Bytes());
    device_->Workspace().Device().DownloadTexture2D(
        lease->Texture(),
        std::span<std::byte>(reinterpret_cast<std::byte*>(pixels.data()), pixels.size()),
        device_->CommandContext());
    return pixels;
  }

  auto DownloadImage(const GraphValueId& id) -> std::vector<Rgba> {
    auto* lease = device_->Workspace().Images().Find(id);
    EXPECT_NE(lease, nullptr);
    if (lease == nullptr) {
      return {};
    }
    std::vector<Rgba> pixels(static_cast<std::size_t>(lease->Texture().Width()) *
                             lease->Texture().Height());
    device_->Workspace().Device().DownloadTexture2D(
        lease->Texture(),
        std::span<std::byte>(reinterpret_cast<std::byte*>(pixels.data()),
                             pixels.size() * sizeof(Rgba)),
        device_->CommandContext());
    return pixels;
  }

  auto RenderMask(std::span<const RectI> dirty = {}) -> OpenClMaskResult {
    device_->BeginRender();
    try {
      const auto result = ExecuteOpenClMask(*device_, plan_, document_, store_.get(), dirty);
      device_->EndRender();
      device_->WaitIdle();
      return result;
    } catch (...) {
      device_->CancelRender();
      throw;
    }
  }

  auto RenderGrade(std::span<const RectI> dirty = {}) -> GradeFrame {
    device_->BeginRender();
    try {
      ExecuteOpenClDevelop(*device_, plan_, prepared_, document_);
      ExecuteOpenClGeometryResample(*device_, plan_);
      ExecuteOpenClCameraColor(*device_, plan_, document_);
      if (plan_.FirstGrade() != nullptr && plan_.FirstGrade()->mask.has_value()) {
        (void)ExecuteOpenClMask(*device_, plan_, document_, store_.get(), dirty);
        device_->Workspace().TransientBuffers().Reset();
      }
      auto result = ExecuteOpenClPrimaryGrade(*device_, plan_, prepared_, document_);
      device_->EndRender();
      device_->WaitIdle();
      GradeFrame frame;
      frame.result = result;
      frame.source = DownloadImage(plan_.develop_output);
      frame.output = DownloadImage(result.output);
      device_->PublishResults();
      return frame;
    } catch (...) {
      device_->CancelRender();
      throw;
    }
  }

  std::uint32_t                       width_  = 0;
  std::uint32_t                       height_ = 0;
  std::filesystem::path               root_;
  std::unique_ptr<MaskStore>          store_;
  PreparedRawInput                    prepared_;
  PipelineDocument                    document_;
  ExecutionPlan                       plan_;
  std::unique_ptr<OpenClRenderDevice> device_;
};

TEST_F(OpenClMaskFixture, OpenClRasterMaskUploadsOnlyChangedR8Rectangle) {
  auto asset = MakeRaster("dirty");
  AttachRaster(asset);
  Compile();
  (void)RenderMask();
  std::fill(asset.pixels.begin(), asset.pixels.end(), 200);
  store_->Save(asset);
  device_->Workspace().Device().ResetCounters();
  const RectI dirty[] = {{1, 2, 3, 2}, {3, 1, 2, 4}};
  (void)RenderMask(dirty);
  ASSERT_EQ(device_->Workspace().Device().LastTextureRectangles().size(), 1U);
  EXPECT_EQ(device_->Workspace().Device().LastTextureRectangles().front(), (RectI{1, 1, 4, 4}));
  EXPECT_EQ(device_->Workspace().Device().HostToDeviceBytes(), 16U);
}

TEST_F(OpenClMaskFixture, OpenClRasterMaskLevelsUseWorkspaceCache) {
  AttachRaster(MakeRaster("reuse", 255));
  Compile();
  const auto first = RenderMask();
  EXPECT_GT(first.mip_level_count, 1U);
  RenderRequest request;
  request.resolution.render_scale = 0.5f;
  Compile(request);
  const auto second = RenderMask();
  EXPECT_EQ(first.persistent_texture_resource_id, second.persistent_texture_resource_id);
  EXPECT_EQ(first.mip_level_count, second.mip_level_count);
  EXPECT_EQ(device_->Workspace().MaskTextures().EntryCount(), 1U);
}

TEST_F(OpenClMaskFixture, OpenClMaskFeatherMatchesExactSignedDistanceReference) {
  SetExtent(9, 9);
  auto asset = MakeRaster("exact");
  for (std::uint32_t y = 3; y <= 5; ++y) {
    for (std::uint32_t x = 3; x <= 5; ++x) {
      asset.pixels[static_cast<std::size_t>(y) * width_ + x] = 255;
    }
  }
  asset.pixels[4 * width_ + 2] = 128;
  AttachRaster(asset, 2.0f);
  Compile();
  (void)RenderMask();
  const auto actual   = DownloadMask();
  const auto expected = CpuExactFeather(asset, 2.0f);
  ASSERT_EQ(actual.size(), expected.size());
  int max_error = 0;
  for (std::size_t index = 0; index < actual.size(); ++index) {
    max_error = std::max(
        max_error, std::abs(static_cast<int>(actual[index]) - static_cast<int>(expected[index])));
  }
  EXPECT_LE(max_error, 2);
  EXPECT_NEAR(actual[4 * width_ + 2], 128, 2);
  EXPECT_GT(actual[4 * width_ + 4], 240);
}

TEST_F(OpenClMaskFixture, OpenClFeatherRadiusEditReusesSignedDistanceResult) {
  auto asset = MakeRaster("radius");
  for (std::uint32_t y = 3; y < 9; ++y) {
    for (std::uint32_t x = 4; x < 12; ++x) {
      asset.pixels[static_cast<std::size_t>(y) * width_ + x] = 255;
    }
  }
  auto& node = AttachRaster(asset, 1.0f);
  Compile();
  const auto first = RenderMask();
  const auto first_metadata =
      device_->Workspace().Values().GetMetadata(GraphValueId{node.Id(), PortId{"signed_distance"}});
  node.SetFeatherRadius(4.0f);
  const auto second = RenderMask();
  const auto second_metadata =
      device_->Workspace().Values().GetMetadata(GraphValueId{node.Id(), PortId{"signed_distance"}});
  ASSERT_TRUE(first_metadata.has_value());
  ASSERT_TRUE(second_metadata.has_value());
  EXPECT_NE(first.signed_distance_resource_id, 0U);
  EXPECT_EQ(first.signed_distance_resource_id, second.signed_distance_resource_id);
  EXPECT_EQ(second.transient_bytes, 0U);
  EXPECT_EQ(first_metadata->content_key, second_metadata->content_key);
}

TEST_F(OpenClMaskFixture, OpenClMaskSamplingMatchesCudaAtCropRotationAndDynamicResolution) {
  auto&            node = AttachAnalytic(AnalyticMaskKind::Radial);
  RadialMaskParams radial;
  radial.major_radius = 0.35f;
  radial.minor_radius = 0.25f;
  node.SetRadial(radial);
  document_.Geometry().SetCropRect({0.1f, 0.1f, 0.8f, 0.8f});
  document_.Geometry().SetRotationDegrees(15.0f);
  Compile();
  (void)RenderMask();
  auto check_analytic = [&]() {
    const auto pixels  = DownloadMask();
    float      max_err = 0.0f;
    for (std::uint32_t y = 0; y < plan_.geometry.render_extent.height; ++y) {
      for (std::uint32_t x = 0; x < plan_.geometry.render_extent.width; ++x) {
        const float expected = CpuAnalytic(node, plan_.geometry, x, y);
        const float actual =
            pixels[static_cast<std::size_t>(y) * plan_.geometry.render_extent.width + x] / 255.0f;
        max_err = std::max(max_err, std::fabs(expected - actual));
      }
    }
    EXPECT_LT(max_err, 2.0f / 255.0f);
  };
  check_analytic();

  RenderRequest request;
  request.resolution.render_scale = 0.5f;
  Compile(request);
  (void)RenderMask();
  check_analytic();

  auto asset = MakeRaster("sample");
  for (std::uint32_t y = 0; y < height_; ++y) {
    for (std::uint32_t x = width_ / 2; x < width_; ++x) {
      asset.pixels[static_cast<std::size_t>(y) * width_ + x] = 255;
    }
  }
  document_ = CreateDefaultPipelineDocument();
  gpu_dag_test::EnsureTestCameraProfile(document_);
  document_.Geometry().SetCropRect({0.05f, 0.1f, 0.9f, 0.8f});
  document_.Geometry().SetRotationDegrees(-20.0f);
  AttachRaster(asset);
  Compile();
  (void)RenderMask();
  const auto pixels   = DownloadMask();
  const auto sampling = MakeRasterMaskSamplingPlan(plan_.geometry, {}, asset.descriptor.extent);
  const auto levels   = BuildR8MipChain(asset.pixels, width_, height_);
  const auto selected_level = std::min<std::size_t>(
      static_cast<std::size_t>(std::max(std::floor(sampling.mip_level), 0.0f)), levels.size() - 1);
  const auto selected_width  = std::max<std::uint32_t>(width_ >> selected_level, 1);
  const auto selected_height = std::max<std::uint32_t>(height_ >> selected_level, 1);
  float      max_err         = 0.0f;
  for (std::uint32_t y = 0; y < plan_.geometry.render_extent.height; ++y) {
    for (std::uint32_t x = 0; x < plan_.geometry.render_extent.width; ++x) {
      const auto  uv = Transform(sampling.render_to_texture_uv, static_cast<float>(x) + 0.5f,
                                 static_cast<float>(y) + 0.5f);
      const float expected =
          SampleR8(levels[selected_level], selected_width, selected_height, uv.x, uv.y);
      const float actual =
          pixels[static_cast<std::size_t>(y) * plan_.geometry.render_extent.width + x] / 255.0f;
      max_err = std::max(max_err, std::fabs(expected - actual));
    }
  }
  EXPECT_LT(max_err, 2.0f / 255.0f);
}

TEST_F(OpenClMaskFixture, OpenClGraduatedNdMaskFollowsReferenceSpaceNormal) {
  auto&                 node = AttachAnalytic(AnalyticMaskKind::GraduatedNd);
  GraduatedNdMaskParams params;
  params.origin_x            = 0.35f;
  params.origin_y            = 0.4f;
  params.normal_x            = 0.6f;
  params.normal_y            = 0.8f;
  params.transition_distance = 0.7f;
  params.start_value         = 0.9f;
  params.end_value           = 0.1f;
  params.invert              = true;
  node.SetGraduatedNd(params);
  document_.Geometry().SetCropRect({0.1f, 0.05f, 0.8f, 0.85f});
  document_.Geometry().SetRotationDegrees(12.0f);
  Compile();
  (void)RenderMask();
  const auto pixels  = DownloadMask();
  float      max_err = 0.0f;
  for (std::uint32_t y = 0; y < plan_.geometry.render_extent.height; ++y) {
    for (std::uint32_t x = 0; x < plan_.geometry.render_extent.width; ++x) {
      const float expected = CpuAnalytic(node, plan_.geometry, x, y);
      const float actual =
          pixels[static_cast<std::size_t>(y) * plan_.geometry.render_extent.width + x] / 255.0f;
      max_err = std::max(max_err, std::fabs(expected - actual));
    }
  }
  EXPECT_LT(max_err, 2.0f / 255.0f);
}

TEST_F(OpenClMaskFixture, OpenClDisconnectedMaskUsesConstantOneWithoutImageAllocation) {
  auto* exposure = dynamic_cast<ExposureModel*>(
      document_.PrimaryGrade()->FindAdjustmentByType(type_ids::Exposure()));
  ASSERT_NE(exposure, nullptr);
  exposure->SetValue(1.0f);
  document_.PrimaryGrade()->SetMix(0.5f);
  Compile();
  const auto mixed = RenderGrade();
  EXPECT_EQ(device_->Workspace().MaskTextures().EntryCount(), 0U);
  EXPECT_EQ(device_->Workspace().Images().Find(plan_.FirstGrade()->mask_output), nullptr);
  document_.PrimaryGrade()->SetMix(1.0f);
  const auto full = RenderGrade();
  ASSERT_EQ(mixed.source.size(), mixed.output.size());
  ASSERT_EQ(full.output.size(), mixed.output.size());
  float max_error = 0.0f;
  for (std::size_t index = 0; index < mixed.output.size(); ++index) {
    max_error = std::max(
        max_error,
        std::fabs(mixed.output[index].r -
                  (mixed.source[index].r + (full.output[index].r - mixed.source[index].r) * 0.5f)));
  }
  EXPECT_LT(max_error, 2.0e-3f);
  EXPECT_EQ(device_->Workspace().MaskTextures().EntryCount(), 0U);
}

TEST_F(OpenClMaskFixture, OpenClNormalMixMatchesCudaReferenceWithinTolerance) {
  auto asset = MakeRaster("mix", 255);
  AttachRaster(asset);
  auto* exposure = dynamic_cast<ExposureModel*>(
      document_.PrimaryGrade()->FindAdjustmentByType(type_ids::Exposure()));
  ASSERT_NE(exposure, nullptr);
  exposure->SetValue(1.0f);
  Compile();
  const auto full = RenderGrade();
  for (std::uint32_t y = 0; y < height_; ++y) {
    for (std::uint32_t x = 0; x < width_ / 2; ++x) {
      asset.pixels[static_cast<std::size_t>(y) * width_ + x] = 0;
    }
  }
  store_->Save(asset);
  const RectI dirty{0, 0, static_cast<std::int32_t>(width_ / 2),
                    static_cast<std::int32_t>(height_)};
  const auto  mixed = RenderGrade(std::span{&dirty, 1});
  ASSERT_EQ(mixed.source.size(), mixed.output.size());
  const auto left  = static_cast<std::size_t>(5 * width_ + 2);
  const auto right = static_cast<std::size_t>(5 * width_ + width_ - 2);
  EXPECT_NEAR(mixed.output[left].r, mixed.source[left].r, 1.0e-5f);
  EXPECT_NEAR(mixed.output[right].r, full.output[right].r, 1.0e-5f);
}

TEST_F(OpenClMaskFixture, OpenClMaskCacheDoesNotEvictBusyImages) {
  auto& cache = device_->Workspace().MaskTextures();
  cache.SetByteBudget(1);
  device_->BeginRender();
  {
    auto active = cache.Acquire(MaskAssetKey{"active"}, {2, 2});
  }
  device_->EndRender();
  {
    auto next = cache.Acquire(MaskAssetKey{"next"}, {2, 2});
  }
  EXPECT_TRUE(cache.Contains(MaskAssetKey{"active"}));
  EXPECT_TRUE(cache.Contains(MaskAssetKey{"next"}));
  device_->WaitIdle();
}

TEST_F(OpenClMaskFixture, OpenClMaskLevelsUseSeparateOpenCl12Images) {
  auto& cache = device_->Workspace().MaskTextures();
  auto  lease = cache.Acquire(MaskAssetKey{"levels"}, {7, 5});
  ASSERT_GT(lease.MipLevelCount(), 1U);
  std::vector<std::uint64_t> resources;
  for (std::size_t level = 0; level < lease.MipLevelCount(); ++level) {
    const auto&        texture     = lease.Texture(level);
    cl_mem_object_type object_type = CL_MEM_OBJECT_BUFFER;
    ASSERT_EQ(clGetMemObjectInfo(texture.Native(), CL_MEM_TYPE, sizeof(object_type), &object_type,
                                 nullptr),
              CL_SUCCESS);
    EXPECT_EQ(object_type, CL_MEM_OBJECT_IMAGE2D);
    EXPECT_EQ(texture.Format(), TextureFormat::R8);
    EXPECT_NE(texture.Native(), nullptr);
    resources.push_back(texture.ResourceId());
  }
  for (std::size_t i = 0; i < resources.size(); ++i) {
    for (std::size_t j = i + 1; j < resources.size(); ++j) {
      EXPECT_NE(resources[i], resources[j]);
    }
  }
}

TEST_F(OpenClMaskFixture, OpenClRasterMaskRequiresMaskStoreAndReportsTheFailure) {
  AttachRaster(MakeRaster("missing-store", 255));
  Compile();
  device_->BeginRender();
  EXPECT_THROW((void)ExecuteOpenClMask(*device_, plan_, document_, nullptr), std::invalid_argument);
  device_->CancelRender();
}

TEST_F(OpenClMaskFixture, OpenClPlanExecutorRunsRasterMaskBeforePrimaryGrade) {
  AttachRaster(MakeRaster("plan-mask", 255));
  auto* exposure = dynamic_cast<ExposureModel*>(
      document_.PrimaryGrade()->FindAdjustmentByType(type_ids::Exposure()));
  ASSERT_NE(exposure, nullptr);
  exposure->SetValue(1.0f);
  Compile();
  const auto display = device_->Execute(plan_, prepared_, document_, store_.get());
  device_->WaitIdle();
  ASSERT_EQ(display, plan_.display_output);
  const auto mask = DownloadMask();
  ASSERT_EQ(mask.size(), static_cast<std::size_t>(plan_.geometry.render_extent.width) *
                             plan_.geometry.render_extent.height);
  EXPECT_TRUE(
      std::all_of(mask.begin(), mask.end(), [](std::uint8_t value) { return value == 255; }));
  ASSERT_NE(plan_.FirstGrade(), nullptr);
  const auto output = DownloadImage(plan_.FirstGrade()->scene_output);
  EXPECT_FALSE(output.empty());
}

}  // namespace
}  // namespace alcedo
