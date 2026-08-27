//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "../graph/test_camera_profile.hpp"
#include "../input/prepared_raw_test_support.hpp"
#include "edit/geometry/texture_sampling_plan.hpp"
#include "edit/graph/analytic_mask_node_model.hpp"
#include "edit/graph/raster_mask_node_model.hpp"
#include "edit/input/raw_input_loader.hpp"
#include "edit/operators/models/builtin_type_ids.hpp"
#include "edit/operators/models/scalar_operator_model.hpp"
#include "edit/runtime/graph_compiler.hpp"
#include "edit/runtime/metal/metal_develop_pass.hpp"
#include "edit/runtime/metal/metal_mask_pass.hpp"
#include "edit/runtime/metal/metal_primary_grade_pass.hpp"

namespace alcedo {
namespace {

struct Rgba {
  float r = 0.0f;
  float g = 0.0f;
  float b = 0.0f;
  float a = 1.0f;
};

auto HasMetalDevice() -> bool {
  try {
    return BindSystemDefaultMetalPresentationDevice() != nullptr;
  } catch (...) {
    return false;
  }
}

auto Transform(const Matrix3x3& matrix, float x, float y) -> Vector2 {
  return {matrix.m[0] * x + matrix.m[1] * y + matrix.m[2],
          matrix.m[3] * x + matrix.m[4] * y + matrix.m[5]};
}

auto SampleR8(const std::vector<std::uint8_t>& pixels, std::uint32_t width, std::uint32_t height,
              float u, float v) -> float {
  if (u < 0.0f || v < 0.0f || u > 1.0f || v > 1.0f) {
    return 0.0f;
  }
  const float x = u * static_cast<float>(width) - 0.5f;
  const float y = v * static_cast<float>(height) - 0.5f;
  const int   x0 =
      std::max(0, std::min(static_cast<int>(width) - 1, static_cast<int>(std::floor(x))));
  const int y0 =
      std::max(0, std::min(static_cast<int>(height) - 1, static_cast<int>(std::floor(y))));
  const int   x1 = std::min(x0 + 1, static_cast<int>(width) - 1);
  const int   y1 = std::min(y0 + 1, static_cast<int>(height) - 1);
  const float tx = std::min(std::max(x - std::floor(x), 0.0f), 1.0f);
  const float ty = std::min(std::max(y - std::floor(y), 0.0f), 1.0f);
  const float a =
      pixels[static_cast<std::size_t>(y0) * width + static_cast<std::size_t>(x0)] * (1.0f - tx) +
      pixels[static_cast<std::size_t>(y0) * width + static_cast<std::size_t>(x1)] * tx;
  const float b =
      pixels[static_cast<std::size_t>(y1) * width + static_cast<std::size_t>(x0)] * (1.0f - tx) +
      pixels[static_cast<std::size_t>(y1) * width + static_cast<std::size_t>(x1)] * tx;
  return (a * (1.0f - ty) + b * ty) / 255.0f;
}

auto CpuAnalytic(const AnalyticMaskNodeModel& node, const ResolvedRenderGeometry& geometry,
                 std::uint32_t x, std::uint32_t y) -> float {
  const auto  reference = Transform(geometry.render_to_reference, static_cast<float>(x) + 0.5f,
                                    static_cast<float>(y) + 0.5f);
  const float nx        = reference.x / static_cast<float>(geometry.full_reference_extent.width);
  const float ny        = reference.y / static_cast<float>(geometry.full_reference_extent.height);
  float       value     = 0.0f;
  bool        invert    = false;
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
    value =
        1.0f - std::min(std::max((radius - inner) / std::max(outer - inner, 1.0e-6f), 0.0f), 1.0f);
    invert = radial.invert;
  } else {
    const auto& graduated     = node.GraduatedNd();
    const float normal_length = std::hypot(graduated.normal_x, graduated.normal_y);
    const float normal_x      = graduated.normal_x / std::max(normal_length, 1.0e-6f);
    const float normal_y      = graduated.normal_y / std::max(normal_length, 1.0e-6f);
    const float distance =
        (nx - graduated.origin_x) * normal_x + (ny - graduated.origin_y) * normal_y;
    const float t = std::min(
        std::max(distance / std::max(graduated.transition_distance, 1.0e-6f) + 0.5f, 0.0f), 1.0f);
    value  = graduated.start_value + (graduated.end_value - graduated.start_value) * t;
    invert = graduated.invert;
  }
  if (invert) {
    value = 1.0f - value;
  }
  return value;
}

class MetalMaskFixture : public ::testing::Test {
 protected:
  void SetUp() override {
    if (!HasMetalDevice()) {
      GTEST_SKIP() << "No Metal device available.";
    }
    (void)BindSystemDefaultMetalPresentationDevice();
    SetExtent(16, 12);
    root_ = std::filesystem::path{"build/tmp/m5_metal_mask"} /
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

  auto RenderMask(std::span<const RectI> dirty = {}) -> MetalMaskResult {
    device_.BeginRender();
    auto result = ExecuteMetalMask(device_, plan_, document_, store_.get(), dirty);
    device_.EndRender();
    device_.WaitIdle();
    return result;
  }

  auto RenderGrade() -> MetalPrimaryGradeResult {
    device_.BeginRender();
    ExecuteMetalDevelop(device_, plan_, prepared_, document_);
    ExecuteMetalGeometryResample(device_, plan_);
    ExecuteMetalCameraColor(device_, plan_, document_);
    if (plan_.primary_grade_mask) {
      (void)ExecuteMetalMask(device_, plan_, document_, store_.get());
    }
    auto result = ExecuteMetalPrimaryGrade(device_, plan_, prepared_, document_);
    device_.EndRender();
    device_.WaitIdle();
    return result;
  }

  auto DownloadMask() -> std::vector<std::uint8_t> {
    auto* lease = device_.Workspace().Images().Find(plan_.mask_output);
    EXPECT_NE(lease, nullptr);
    if (lease == nullptr) {
      return {};
    }
    std::vector<std::uint8_t> pixels(lease->Texture().Bytes());
    device_.Workspace().Device().DownloadTexture2D(
        lease->Texture(),
        std::span<std::byte>(reinterpret_cast<std::byte*>(pixels.data()), pixels.size()),
        device_.CommandContext());
    return pixels;
  }

  auto DownloadImage(const GraphValueId& id) -> std::vector<Rgba> {
    auto* lease = device_.Workspace().Images().Find(id);
    EXPECT_NE(lease, nullptr);
    if (lease == nullptr) {
      return {};
    }
    std::vector<Rgba> pixels(static_cast<std::size_t>(lease->Texture().Width()) *
                             lease->Texture().Height());
    device_.Workspace().Device().DownloadTexture2D(
        lease->Texture(),
        std::span<std::byte>(reinterpret_cast<std::byte*>(pixels.data()),
                             pixels.size() * sizeof(Rgba)),
        device_.CommandContext());
    return pixels;
  }

  std::uint32_t              width_  = 0;
  std::uint32_t              height_ = 0;
  std::filesystem::path      root_;
  std::unique_ptr<MaskStore> store_;
  PreparedRawInput           prepared_;
  PipelineDocument           document_;
  ExecutionPlan              plan_;
  MetalRenderDevice          device_;
};

TEST_F(MetalMaskFixture, MetalRasterMaskUploadsOnlyChangedR8Rectangle) {
  auto asset = MakeRaster("dirty", 0);
  AttachRaster(asset);
  Compile();
  RenderMask();
  std::fill(asset.pixels.begin(), asset.pixels.end(), 200);
  store_->Save(asset);
  device_.Workspace().Device().ResetCounters();
  const RectI dirty[] = {{1, 2, 3, 2}, {3, 1, 2, 4}};
  RenderMask(dirty);
  ASSERT_EQ(device_.Workspace().Device().LastTextureRectangles().size(), 1U);
  EXPECT_EQ(device_.Workspace().Device().LastTextureRectangles().front(), (RectI{1, 1, 4, 4}));
  EXPECT_EQ(device_.Workspace().Device().HostToDeviceBytes(), 16U);
}

TEST_F(MetalMaskFixture, MetalRasterMaskMipChainUsesWorkspaceCache) {
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
}

TEST_F(MetalMaskFixture, MetalMaskFeatherMatchesExactSignedDistanceReference) {
  SetExtent(9, 9);
  auto asset = MakeRaster("exact", 0);
  for (std::uint32_t y = 3; y <= 5; ++y) {
    for (std::uint32_t x = 3; x <= 5; ++x) {
      asset.pixels[y * width_ + x] = 255;
    }
  }
  AttachRaster(asset, 2.0f);
  Compile();
  RenderMask();
  const auto pixels = DownloadMask();
  EXPECT_NEAR(pixels[4 * width_ + 2], 81, 8);
  EXPECT_NEAR(pixels[2 * width_ + 2], 46, 10);
  EXPECT_GT(pixels[4 * width_ + 4], 240);
}

TEST_F(MetalMaskFixture, MetalFeatherRadiusEditReusesSignedDistanceResult) {
  auto asset = MakeRaster("radius", 0);
  for (std::uint32_t y = 3; y < 9; ++y) {
    for (std::uint32_t x = 4; x < 12; ++x) {
      asset.pixels[y * width_ + x] = 255;
    }
  }
  auto& node = AttachRaster(asset, 1.0f);
  Compile();
  const auto first = RenderMask();
  node.SetFeatherRadius(4.0f);
  const auto second = RenderMask();
  EXPECT_NE(first.signed_distance_resource_id, 0U);
  EXPECT_EQ(first.signed_distance_resource_id, second.signed_distance_resource_id);
  EXPECT_EQ(second.transient_bytes, 0U);
}

TEST_F(MetalMaskFixture, MetalMaskSamplingMatchesCudaAtCropRotationAndDynamicResolution) {
  auto&            node = AttachAnalytic(AnalyticMaskKind::Radial);
  RadialMaskParams radial;
  radial.major_radius = 0.35f;
  radial.minor_radius = 0.25f;
  node.SetRadial(radial);
  document_.Geometry().SetCropRect({0.1f, 0.1f, 0.8f, 0.8f});
  document_.Geometry().SetRotationDegrees(15.0f);
  Compile();
  RenderMask();
  auto check = [&](const std::vector<std::uint8_t>& pixels) {
    ASSERT_EQ(pixels.size(), static_cast<std::size_t>(plan_.geometry.render_extent.width) *
                                 plan_.geometry.render_extent.height);
    float max_err = 0.0f;
    for (std::uint32_t y = 0; y < plan_.geometry.render_extent.height; ++y) {
      for (std::uint32_t x = 0; x < plan_.geometry.render_extent.width; ++x) {
        const float expected = CpuAnalytic(node, plan_.geometry, x, y);
        const float got =
            pixels[static_cast<std::size_t>(y) * plan_.geometry.render_extent.width + x] / 255.0f;
        max_err = std::max(max_err, std::fabs(expected - got));
      }
    }
    EXPECT_LT(max_err, 2.0f / 255.0f);
  };
  check(DownloadMask());

  RenderRequest request;
  request.resolution.render_scale = 0.5f;
  Compile(request);
  RenderMask();
  check(DownloadMask());

  auto asset = MakeRaster("sample", 0);
  for (std::uint32_t y = 0; y < height_; ++y) {
    for (std::uint32_t x = width_ / 2; x < width_; ++x) {
      asset.pixels[y * width_ + x] = 255;
    }
  }
  document_ = CreateDefaultPipelineDocument();
  gpu_dag_test::EnsureTestCameraProfile(document_);
  document_.Geometry().SetCropRect({0.05f, 0.1f, 0.9f, 0.8f});
  document_.Geometry().SetRotationDegrees(-20.0f);
  AttachRaster(asset);
  Compile();
  RenderMask();
  const auto raster     = DownloadMask();
  const auto sampling   = MakeRasterMaskSamplingPlan(plan_.geometry, {}, asset.descriptor.extent);
  float      raster_err = 0.0f;
  for (std::uint32_t y = 0; y < plan_.geometry.render_extent.height; ++y) {
    for (std::uint32_t x = 0; x < plan_.geometry.render_extent.width; ++x) {
      const auto  uv       = Transform(sampling.render_to_texture_uv, static_cast<float>(x) + 0.5f,
                                       static_cast<float>(y) + 0.5f);
      const float expected = SampleR8(asset.pixels, width_, height_, uv.x, uv.y);
      const float got =
          raster[static_cast<std::size_t>(y) * plan_.geometry.render_extent.width + x] / 255.0f;
      raster_err = std::max(raster_err, std::fabs(expected - got));
    }
  }
  EXPECT_LT(raster_err, 2.0f / 255.0f);
}

TEST_F(MetalMaskFixture, MetalDisconnectedMaskUsesConstantOneWithoutTextureAllocation) {
  auto* exposure = dynamic_cast<ExposureModel*>(
      document_.PrimaryGrade()->FindAdjustmentByType(type_ids::Exposure()));
  ASSERT_NE(exposure, nullptr);
  exposure->SetValue(1.0f);
  Compile();
  document_.PrimaryGrade()->SetMix(0.5f);
  const auto result = RenderGrade();
  EXPECT_EQ(device_.Workspace().MaskTextures().EntryCount(), 0U);
  EXPECT_EQ(device_.Workspace().Images().Find(plan_.mask_output), nullptr);
  const auto source = DownloadImage(plan_.develop_output);
  const auto mixed  = DownloadImage(result.output);
  document_.PrimaryGrade()->SetMix(1.0f);
  const auto full = DownloadImage(RenderGrade().output);
  ASSERT_EQ(source.size(), mixed.size());
  ASSERT_EQ(full.size(), mixed.size());
  float max_err = 0.0f;
  for (std::size_t i = 0; i < mixed.size(); ++i) {
    max_err =
        std::max(max_err, std::fabs(mixed[i].r - (source[i].r + (full[i].r - source[i].r) * 0.5f)));
  }
  EXPECT_LT(max_err, 2.0e-3f);
  EXPECT_EQ(device_.Workspace().MaskTextures().EntryCount(), 0U);
}

TEST_F(MetalMaskFixture, MetalNormalMixMatchesCudaReferenceWithinTolerance) {
  auto asset = MakeRaster("mix", 255);
  AttachRaster(asset);
  auto* exposure = dynamic_cast<ExposureModel*>(
      document_.PrimaryGrade()->FindAdjustmentByType(type_ids::Exposure()));
  ASSERT_NE(exposure, nullptr);
  exposure->SetValue(1.0f);
  Compile();
  const auto full_result = RenderGrade();
  const auto full        = DownloadImage(full_result.output);
  for (std::uint32_t y = 0; y < height_; ++y) {
    for (std::uint32_t x = 0; x < width_ / 2; ++x) {
      asset.pixels[y * width_ + x] = 0;
    }
  }
  store_->Save(asset);
  device_.BeginRender();
  ExecuteMetalDevelop(device_, plan_, prepared_, document_);
  ExecuteMetalGeometryResample(device_, plan_);
  ExecuteMetalCameraColor(device_, plan_, document_);
  const RectI dirty{0, 0, static_cast<std::int32_t>(width_ / 2),
                    static_cast<std::int32_t>(height_)};
  (void)ExecuteMetalMask(device_, plan_, document_, store_.get(), std::span{&dirty, 1});
  const auto result = ExecuteMetalPrimaryGrade(device_, plan_, prepared_, document_);
  device_.EndRender();
  device_.WaitIdle();
  const auto source = DownloadImage(plan_.develop_output);
  const auto output = DownloadImage(result.output);
  ASSERT_EQ(source.size(), output.size());
  const auto left  = 5 * width_ + 2;
  const auto right = 5 * width_ + width_ - 2;
  EXPECT_NEAR(output[left].r, source[left].r, 1.0e-5f);
  EXPECT_NEAR(output[right].r, full[right].r, 1.0e-5f);
}

TEST_F(MetalMaskFixture, MetalMaskCacheDoesNotEvictBusyTextures) {
  auto& cache = device_.Workspace().MaskTextures();
  cache.SetByteBudget(1);
  device_.BeginRender();
  {
    auto active = cache.Acquire(MaskAssetKey{"active"}, {2, 2});
  }
  device_.EndRender();
  {
    auto next = cache.Acquire(MaskAssetKey{"next"}, {2, 2});
  }
  EXPECT_TRUE(cache.Contains(MaskAssetKey{"active"}));
  EXPECT_TRUE(cache.Contains(MaskAssetKey{"next"}));
  device_.WaitIdle();
}

}  // namespace
}  // namespace alcedo
