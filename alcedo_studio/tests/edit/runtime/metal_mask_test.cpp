//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "../graph/grade_owned_mask_support.hpp"
#include "../graph/test_camera_profile.hpp"
#include "../input/prepared_raw_test_support.hpp"
#include "edit/geometry/texture_sampling_plan.hpp"
#include "edit/input/raw_input_loader.hpp"
#include "edit/mask/active_raster_mask.hpp"
#include "edit/mask/mask_store.hpp"
#include "edit/operators/models/builtin_type_ids.hpp"
#include "edit/operators/models/scalar_operator_model.hpp"
#include "edit/runtime/graph_compiler.hpp"
#include "edit/runtime/metal/metal_develop_pass.hpp"
#include "edit/runtime/metal/metal_mask_pass.hpp"
#include "edit/runtime/metal/metal_pass_encoder.hpp"
#include "edit/runtime/metal/metal_primary_grade_pass.hpp"
#include "edit/runtime/pass_kind.hpp"
#include "edit/runtime/result_content_key.hpp"
#include "edit/runtime/texture_format.hpp"
#include "gpu/transient_allocation_policy.hpp"
#include "multi_grade_runtime_test_support.hpp"
#include "multi_mask_runtime_test_support.hpp"

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

auto CpuAnalytic(const MaskModel& mask, const ResolvedRenderGeometry& geometry, std::uint32_t x,
                 std::uint32_t y) -> float {
  const auto  reference = Transform(geometry.render_to_reference, static_cast<float>(x) + 0.5f,
                                    static_cast<float>(y) + 0.5f);
  const float nx        = reference.x / static_cast<float>(geometry.full_reference_extent.width);
  const float ny        = reference.y / static_cast<float>(geometry.full_reference_extent.height);
  float       value     = 0.0f;
  if (const auto* radial = std::get_if<RadialMaskSource>(&mask.source)) {
    const float c      = std::cos(radial->rotation);
    const float s      = std::sin(radial->rotation);
    const float dx     = nx - radial->center_x;
    const float dy     = ny - radial->center_y;
    const float rx     = (c * dx + s * dy) / std::max(radial->major_radius, 1.0e-6f);
    const float ry     = (-s * dx + c * dy) / std::max(radial->minor_radius, 1.0e-6f);
    const float radius = std::sqrt(rx * rx + ry * ry);
    const float inner  = std::max(0.0f, 1.0f - radial->inner_feather);
    const float outer  = 1.0f + radial->outer_feather;
    value =
        1.0f - std::min(std::max((radius - inner) / std::max(outer - inner, 1.0e-6f), 0.0f), 1.0f);
  } else {
    const auto& graduated     = std::get<LinearGradientMaskSource>(mask.source);
    const float normal_length = std::hypot(graduated.normal_x, graduated.normal_y);
    const float normal_x      = graduated.normal_x / std::max(normal_length, 1.0e-6f);
    const float normal_y      = graduated.normal_y / std::max(normal_length, 1.0e-6f);
    const float distance =
        (nx - graduated.origin_x) * normal_x + (ny - graduated.origin_y) * normal_y;
    const float t = std::min(
        std::max(distance / std::max(graduated.transition_distance, 1.0e-6f) + 0.5f, 0.0f), 1.0f);
    value = graduated.start_value + (graduated.end_value - graduated.start_value) * t;
  }
  if (mask.invert) {
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

  auto MakeRaster(std::uint8_t fill = 0) -> MaskAsset {
    MaskAsset asset;
    asset.descriptor.extent           = {width_, height_};
    asset.descriptor.reference_bounds = {};
    asset.pixels.assign(static_cast<std::size_t>(width_) * height_, fill);
    return asset;
  }

  auto AttachRaster(MaskAsset asset, float feather = 0.0f) -> MaskModel& {
    asset.key  = store_->Put(asset.descriptor, asset.pixels);
    auto& mask = grade_mask_test::AddMask(
        *document_.PrimaryGrade(),
        grade_mask_test::MakeBrushMask(MaskId{"mask.raster"}, asset, feather));
    document_.MarkTopologyDirty();
    return mask;
  }

  auto AttachedBrushKey() const -> MaskAssetKey {
    const auto* mask = document_.PrimaryGrade()->FindMask(MaskId{"mask.raster"});
    return *std::get<BrushMaskSource>(mask->source).asset_key;
  }

  auto MakeActive(const MaskAsset& asset, RectI dirty, std::uint64_t revision,
                  std::uint64_t generation = 1) -> ActiveRasterMaskInput {
    ActiveRasterMaskInput input;
    input.owner_node_id      = document_.PrimaryGrade()->Id();
    input.mask_id            = MaskId{"mask.raster"};
    input.session_generation = generation;
    input.content_revision   = revision;
    input.descriptor         = asset.descriptor;
    input.pixels             = std::make_shared<const std::vector<std::uint8_t>>(asset.pixels);
    input.dirty_rectangle    = dirty;
    return input;
  }

  auto AttachAnalytic(MaskSourceKind kind) -> MaskModel& {
    MaskModel mask;
    mask.id = MaskId{"mask.analytic"};
    if (kind == MaskSourceKind::Radial) {
      mask.source = RadialMaskSource{};
    } else {
      mask.source = LinearGradientMaskSource{};
    }
    auto& result = grade_mask_test::AddMask(*document_.PrimaryGrade(), std::move(mask));
    document_.MarkTopologyDirty();
    return result;
  }

  void Compile(RenderRequest request = {}) {
    plan_ = GraphCompiler::Compile(document_, prepared_.CompileSource(), request);
  }

  auto RenderMask(std::span<const ActiveRasterMaskInput> active = {}) -> MetalMaskResult {
    device_.BeginRender();
    auto result = ExecuteMetalMask(device_, plan_, document_, store_.get(), active);
    device_.EndRender();
    device_.WaitIdle();
    return result;
  }

  auto RenderGrade() -> MetalPrimaryGradeResult {
    device_.BeginRender();
    ExecuteMetalDevelop(device_, plan_, prepared_, document_);
    ExecuteMetalGeometryResample(device_, plan_);
    ExecuteMetalCameraColor(device_, plan_, document_);
    if (plan_.FirstGrade() != nullptr && plan_.FirstGrade()->mask_stack.has_value()) {
      (void)ExecuteMetalMask(device_, plan_, document_, store_.get());
    }
    auto result = ExecuteMetalPrimaryGrade(device_, plan_, prepared_, document_);
    device_.EndRender();
    device_.WaitIdle();
    return result;
  }

  void ExecutePlan(std::span<const ActiveRasterMaskInput> active = {}) {
    ASSERT_EQ(device_.Execute(plan_, prepared_, document_, store_.get(), true,
                              TransientAllocationPolicy::SessionPacked, active),
              plan_.display_output);
    device_.WaitIdle();
  }

  void ExpectPrimaryUnionMatchesReference(
      std::span<const multi_mask_test::BrushRasterView> rasters = {}) {
    const auto loaded = rasters.empty()
                            ? multi_mask_test::LoadBrushRasters(*store_, document_.PrimaryGrade()->Masks())
                            : multi_mask_test::LoadedBrushRasters{};
    const auto views =
        rasters.empty() ? std::span<const multi_mask_test::BrushRasterView>(loaded.views)
                        : rasters;
    const auto expected = multi_mask_test::EvaluateEnabledUnionR8(
        document_.PrimaryGrade()->Masks(), views, plan_.geometry);
    multi_mask_test::ExpectR8WithinTolerance(DownloadMask(), expected);
  }

  auto ResourceIdOf(const GraphValueId& id) -> std::uint64_t {
    auto* lease = device_.Workspace().Images().Find(id);
    EXPECT_NE(lease, nullptr);
    return lease == nullptr ? 0 : lease->Texture().ResourceId();
  }

  auto DownloadR8(const GraphValueId& id) -> std::vector<std::uint8_t> {
    auto* lease = device_.Workspace().Images().Find(id);
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

  auto DownloadMask() -> std::vector<std::uint8_t> {
    return DownloadR8(plan_.FirstGrade()->mask_output);
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

TEST_F(MetalMaskFixture, MetalActiveRasterRevisionUploadsOnlyDirtyRectangle) {
  auto asset = MakeRaster(0);
  AttachRaster(asset);
  Compile();
  RenderMask();
  std::fill(asset.pixels.begin(), asset.pixels.end(), 200);
  const auto full = MakeActive(
      asset, {0, 0, static_cast<std::int32_t>(width_), static_cast<std::int32_t>(height_)}, 1);
  RenderMask(std::span{&full, 1});
  device_.Workspace().Device().ResetCounters();
  const auto dirty = MakeActive(asset, {1, 1, 4, 4}, 2);
  RenderMask(std::span{&dirty, 1});
  ASSERT_EQ(device_.Workspace().Device().LastTextureRectangles().size(), 1U);
  EXPECT_EQ(device_.Workspace().Device().LastTextureRectangles().front(), (RectI{1, 1, 4, 4}));
  EXPECT_EQ(device_.Workspace().Device().HostToDeviceBytes(), 16U);
}

TEST_F(MetalMaskFixture, MetalActiveRasterUpdateNeverPatchesPersistentTexture) {
  auto asset = MakeRaster(17);
  AttachRaster(asset);
  Compile();
  const auto persistent = RenderMask();
  ASSERT_NE(persistent.persistent_texture_resource_id, 0U);
  auto& texture = device_.Workspace().MaskTextures().TextureAt(AttachedBrushKey());
  std::vector<std::uint8_t> before(texture.Bytes());
  device_.Workspace().Device().DownloadTexture2D(
      texture, std::span<std::byte>(reinterpret_cast<std::byte*>(before.data()), before.size()),
      device_.CommandContext());
  EXPECT_EQ(before, asset.pixels);

  auto preview = asset;
  std::fill(preview.pixels.begin(), preview.pixels.end(), 200);
  const auto active = MakeActive(
      preview, {0, 0, static_cast<std::int32_t>(width_), static_cast<std::int32_t>(height_)}, 1);
  const auto result = RenderMask(std::span{&active, 1});
  EXPECT_EQ(result.persistent_texture_resource_id, 0U);
  EXPECT_NE(result.active_texture_resource_id, 0U);
  EXPECT_NE(result.active_texture_resource_id, persistent.persistent_texture_resource_id);
  EXPECT_TRUE(device_.Workspace().MaskTextures().Contains(AttachedBrushKey()));
  auto& still = device_.Workspace().MaskTextures().TextureAt(AttachedBrushKey());
  std::vector<std::uint8_t> after(still.Bytes());
  device_.Workspace().Device().DownloadTexture2D(
      still, std::span<std::byte>(reinterpret_cast<std::byte*>(after.data()), after.size()),
      device_.CommandContext());
  EXPECT_EQ(after, before);
  EXPECT_EQ(after.front(), 17);
  const auto coverage = DownloadMask();
  EXPECT_GT(coverage[(height_ / 2) * width_ + width_ / 2], 190);
}

TEST_F(MetalMaskFixture, MetalNewActiveRasterGenerationReplacesOldPreviewTexture) {
  auto asset = MakeRaster(40);
  AttachRaster(asset);
  Compile();
  const auto first_input = MakeActive(
      asset, {0, 0, static_cast<std::int32_t>(width_), static_cast<std::int32_t>(height_)}, 1, 1);
  const auto first = RenderMask(std::span{&first_input, 1});
  const auto first_key =
      ActiveRasterTextureKey{document_.PrimaryGrade()->Id(), MaskId{"mask.raster"}, 1};
  EXPECT_TRUE(device_.Workspace().ActiveRasterTextures().Contains(first_key));
  const auto second_input = MakeActive(
      asset, {0, 0, static_cast<std::int32_t>(width_), static_cast<std::int32_t>(height_)}, 1, 2);
  const auto second = RenderMask(std::span{&second_input, 1});
  const auto second_key =
      ActiveRasterTextureKey{document_.PrimaryGrade()->Id(), MaskId{"mask.raster"}, 2};
  EXPECT_NE(first.active_texture_resource_id, second.active_texture_resource_id);
  EXPECT_FALSE(device_.Workspace().ActiveRasterTextures().Contains(first_key));
  EXPECT_TRUE(device_.Workspace().ActiveRasterTextures().Contains(second_key));
}

TEST_F(MetalMaskFixture, MetalRasterMaskMipChainUsesWorkspaceCache) {
  AttachRaster(MakeRaster(255));
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
  auto asset = MakeRaster(0);
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
  auto asset = MakeRaster(0);
  for (std::uint32_t y = 3; y < 9; ++y) {
    for (std::uint32_t x = 4; x < 12; ++x) {
      asset.pixels[y * width_ + x] = 255;
    }
  }
  auto& node = AttachRaster(asset, 1.0f);
  Compile();
  const auto first                                      = RenderMask();
  std::get<BrushMaskSource>(node.source).feather_radius = 4.0f;
  const auto second                                     = RenderMask();
  EXPECT_NE(first.signed_distance_resource_id, 0U);
  EXPECT_EQ(first.signed_distance_resource_id, second.signed_distance_resource_id);
  EXPECT_EQ(second.transient_bytes, 0U);
}

TEST_F(MetalMaskFixture, MetalMaskSamplingMatchesCudaAtCropRotationAndDynamicResolution) {
  auto& node          = AttachAnalytic(MaskSourceKind::Radial);
  auto  radial        = std::get<RadialMaskSource>(node.source);
  radial.major_radius = 0.35f;
  radial.minor_radius = 0.25f;
  node.source         = radial;
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

  auto asset = MakeRaster(0);
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
  EXPECT_EQ(device_.Workspace().Images().Find(plan_.FirstGrade()->mask_output), nullptr);
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
  auto asset = MakeRaster(255);
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
  asset.key = store_->Put(asset.descriptor, asset.pixels);
  document_.PrimaryGrade()->ReplaceMaskSource(
      MaskId{"mask.raster"}, grade_mask_test::MakeBrushMask(MaskId{"mask.raster"}, asset).source);
  Compile();
  const auto mixed  = RenderGrade();
  const auto source = DownloadImage(plan_.develop_output);
  const auto output = DownloadImage(mixed.output);
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

TEST_F(MetalMaskFixture, MetalMaskWarmupCachesFillZeroAndUnionMax) {
  std::vector<MetalPipelineWarmup> pipelines;
  AppendMetalMaskWarmup(pipelines);
  bool has_fill_zero = false;
  bool has_union_max = false;
  for (const auto& pipeline : pipelines) {
    const auto name =
        std::string_view{pipeline.function_name == nullptr ? "" : pipeline.function_name};
    has_fill_zero = has_fill_zero || name == "mask_fill_zero";
    has_union_max = has_union_max || name == "mask_union_max";
  }
  EXPECT_TRUE(has_fill_zero);
  EXPECT_TRUE(has_union_max);
  ASSERT_FALSE(pipelines.empty());

  auto& backend = device_.Workspace().Device();
  backend.ResetCounters();
  backend.WarmUpPipelines(pipelines);
  EXPECT_EQ(backend.PipelineCreateCount() + backend.PipelineHitCount(), pipelines.size());
  backend.ResetCounters();
  backend.WarmUpPipelines(pipelines);
  EXPECT_EQ(backend.PipelineCreateCount(), 0U);
  EXPECT_EQ(backend.PipelineHitCount(), pipelines.size());
}

TEST_F(MetalMaskFixture, EmptyMaskListUsesFullGradeCoverage) {
  auto* exposure = dynamic_cast<ExposureModel*>(
      document_.PrimaryGrade()->FindAdjustmentByType(type_ids::Exposure()));
  ASSERT_NE(exposure, nullptr);
  exposure->SetValue(1.0f);
  Compile();
  EXPECT_FALSE(plan_.Contains(GpuPassKind::MaskEvaluate));
  EXPECT_FALSE(plan_.Contains(GpuPassKind::MaskUnion));
  ExecutePlan();
  const auto empty_grade = DownloadImage(plan_.FirstGrade()->scene_output);
  const auto empty_keys  = BuildFrameResultContentKeys(plan_, prepared_, document_);
  EXPECT_TRUE(empty_keys.mask.Empty());
  EXPECT_EQ(device_.Workspace().Images().Find(plan_.FirstGrade()->mask_output), nullptr);

  grade_mask_test::AddRadialMask(document_, MaskId{"mask.radial"});
  Compile();
  ExecutePlan();
  const auto masked_grade = DownloadImage(plan_.FirstGrade()->scene_output);

  document_.PrimaryGrade()->RemoveMask(MaskId{"mask.radial"});
  Compile();
  ExecutePlan();
  const auto restored_grade = DownloadImage(plan_.FirstGrade()->scene_output);
  const auto restored_keys  = BuildFrameResultContentKeys(plan_, prepared_, document_);
  ASSERT_EQ(empty_grade.size(), restored_grade.size());
  EXPECT_NEAR(empty_grade.front().r, restored_grade.front().r, 1.0e-5f);
  EXPECT_EQ(restored_keys.primary_grade, empty_keys.primary_grade);
  EXPECT_GT(std::abs(empty_grade.front().r - masked_grade.front().r), 1.0e-4f);
}

TEST_F(MetalMaskFixture, AllDisabledMasksUseZeroGradeCoverage) {
  auto* exposure = dynamic_cast<ExposureModel*>(
      document_.PrimaryGrade()->FindAdjustmentByType(type_ids::Exposure()));
  ASSERT_NE(exposure, nullptr);
  exposure->SetValue(1.0f);
  grade_mask_test::AddRadialMask(document_, MaskId{"mask.a"});
  grade_mask_test::AddRadialMask(document_, MaskId{"mask.z"});
  document_.PrimaryGrade()->SetMaskEnabled(MaskId{"mask.a"}, false);
  document_.PrimaryGrade()->SetMaskEnabled(MaskId{"mask.z"}, false);
  Compile();
  ASSERT_TRUE(plan_.FirstGrade()->mask_stack.has_value());
  ASSERT_TRUE(plan_.Contains(GpuPassKind::MaskUnion));
  device_.ResetPassStats();
  ExecutePlan();
  EXPECT_EQ(device_.PassStats().mask_execute, 0U);
  EXPECT_EQ(device_.PassStats().mask_union_execute, 1U);
  const auto keys = BuildFrameResultContentKeys(plan_, prepared_, document_);
  EXPECT_EQ(keys.Value(plan_.FirstGrade()->mask_output), AllDisabledMaskUnionKey());
  const auto coverage = DownloadMask();
  ASSERT_FALSE(coverage.empty());
  EXPECT_TRUE(
      std::all_of(coverage.begin(), coverage.end(), [](std::uint8_t value) { return value == 0; }));
  const auto scene = DownloadImage(plan_.FirstGrade()->scene_input);
  const auto grade = DownloadImage(plan_.FirstGrade()->scene_output);
  ASSERT_EQ(scene.size(), grade.size());
  EXPECT_NEAR(grade.front().r, scene.front().r, 1.0e-5f);
  EXPECT_NEAR(grade[grade.size() / 2].r, scene[scene.size() / 2].r, 1.0e-5f);
}

TEST_F(MetalMaskFixture, SingleEnabledMaskUnionAliasesSourceTexture) {
  auto asset = MakeRaster(180);
  asset.key  = store_->Put(asset.descriptor, asset.pixels);
  grade_mask_test::AddMask(*document_.PrimaryGrade(),
                           grade_mask_test::MakeBrushMask(MaskId{"mask.a"}, asset));
  document_.MarkTopologyDirty();
  Compile();
  ASSERT_TRUE(plan_.FirstGrade()->mask_stack.has_value());
  ExecutePlan();
  const auto source_id = plan_.FirstGrade()->mask_stack->sources.front().effective_output;
  const auto union_id  = plan_.FirstGrade()->mask_output;
  EXPECT_EQ(ResourceIdOf(union_id), ResourceIdOf(source_id));
  const auto coverage = DownloadR8(union_id);
  ASSERT_EQ(coverage.size(), asset.pixels.size());
  EXPECT_EQ(coverage, asset.pixels);
}

TEST_F(MetalMaskFixture, EnabledMasksUseMaximumCoverage) {
  auto left_high  = MakeRaster(0);
  auto right_high = MakeRaster(0);
  for (std::uint32_t y = 0; y < height_; ++y) {
    for (std::uint32_t x = 0; x < width_; ++x) {
      const auto index = y * width_ + x;
      if (x < width_ / 2) {
        left_high.pixels[index] = 180;
      } else {
        right_high.pixels[index] = 200;
      }
    }
  }
  left_high.key  = store_->Put(left_high.descriptor, left_high.pixels);
  right_high.key = store_->Put(right_high.descriptor, right_high.pixels);
  grade_mask_test::AddMask(*document_.PrimaryGrade(),
                           grade_mask_test::MakeBrushMask(MaskId{"mask.a"}, left_high));
  grade_mask_test::AddMask(*document_.PrimaryGrade(),
                           grade_mask_test::MakeBrushMask(MaskId{"mask.z"}, right_high));
  document_.MarkTopologyDirty();
  Compile();
  ASSERT_TRUE(plan_.FirstGrade()->mask_stack.has_value());
  ASSERT_EQ(plan_.FirstGrade()->mask_stack->sources.size(), 2U);
  device_.ResetPassStats();
  ExecutePlan();
  EXPECT_EQ(device_.PassStats().mask_execute, 2U);
  EXPECT_EQ(device_.PassStats().mask_union_execute, 1U);
  const auto source_a = plan_.FirstGrade()->mask_stack->sources[0].effective_output;
  const auto source_z = plan_.FirstGrade()->mask_stack->sources[1].effective_output;
  const auto union_id = plan_.FirstGrade()->mask_output;
  EXPECT_NE(ResourceIdOf(union_id), ResourceIdOf(source_a));
  EXPECT_NE(ResourceIdOf(union_id), ResourceIdOf(source_z));
  EXPECT_NE(ResourceIdOf(source_a), ResourceIdOf(source_z));
  const auto a       = DownloadR8(source_a);
  const auto z       = DownloadR8(source_z);
  const auto unified = DownloadR8(union_id);
  ASSERT_EQ(a.size(), z.size());
  ASSERT_EQ(unified.size(), a.size());
  for (std::size_t i = 0; i < unified.size(); ++i) {
    EXPECT_EQ(unified[i], std::max(a[i], z[i]));
  }
  const auto left  = 5 * width_ + 2;
  const auto right = 5 * width_ + width_ - 2;
  EXPECT_EQ(unified[left], 180);
  EXPECT_EQ(unified[right], 200);
  EXPECT_NE(unified[left], static_cast<std::uint8_t>(180 + 200));
  ExpectPrimaryUnionMatchesReference();
}

TEST_F(MetalMaskFixture, OneMaskEditReusesSiblingAndUpstreamResults) {
  RadialMaskSource wide;
  wide.major_radius = 0.45f;
  RadialMaskSource narrow;
  narrow.major_radius = 0.2f;
  grade_mask_test::AddRadialMask(document_, MaskId{"mask.a"}, wide);
  grade_mask_test::AddRadialMask(document_, MaskId{"mask.z"}, narrow);
  Compile();
  ExecutePlan();
  const auto before = BuildFrameResultContentKeys(plan_, prepared_, document_);
  ASSERT_TRUE(plan_.FirstGrade()->mask_stack.has_value());
  const auto sibling = plan_.FirstGrade()->mask_stack->sources[1].effective_output;
  document_.PrimaryGrade()->SetMaskOpacity(MaskId{"mask.a"}, 0.4f);
  const auto after = BuildFrameResultContentKeys(plan_, prepared_, document_);
  EXPECT_EQ(after.develop_image, before.develop_image);
  EXPECT_EQ(after.Value(sibling), before.Value(sibling));
  EXPECT_NE(after.mask, before.mask);
  device_.ResetPassStats();
  ExecutePlan();
  EXPECT_GE(device_.PassStats().camera_color_skip, 1U);
  EXPECT_EQ(device_.PassStats().mask_skip, 1U);
  EXPECT_EQ(device_.PassStats().mask_execute, 1U);
  EXPECT_EQ(device_.PassStats().mask_union_execute, 1U);
  EXPECT_EQ(device_.PassStats().mask_union_skip, 0U);
}

TEST_F(MetalMaskFixture, MaskFailurePublishesNoSourceUnionOrGradeWrites) {
  auto first_asset  = MakeRaster(200);
  auto second_asset = MakeRaster(40);
  first_asset.key   = store_->Put(first_asset.descriptor, first_asset.pixels);
  second_asset.key  = store_->Put(second_asset.descriptor, second_asset.pixels);
  grade_mask_test::AddMask(*document_.PrimaryGrade(),
                           grade_mask_test::MakeBrushMask(MaskId{"mask.a"}, first_asset));
  grade_mask_test::AddMask(*document_.PrimaryGrade(),
                           grade_mask_test::MakeBrushMask(MaskId{"mask.z"}, second_asset));
  document_.MarkTopologyDirty();
  Compile();
  ExecutePlan();
  auto& images       = device_.Workspace().Images();
  auto& invalidation = device_.Workspace().ResultInvalidation();
  ASSERT_TRUE(plan_.FirstGrade()->mask_stack.has_value());
  const auto source_a    = plan_.FirstGrade()->mask_stack->sources[0].effective_output;
  const auto union_id    = plan_.FirstGrade()->mask_output;
  const auto grade_id    = plan_.FirstGrade()->scene_output;
  const auto source_rev  = images.PublishedRevision(source_a);
  const auto union_rev   = images.PublishedRevision(union_id);
  const auto grade_rev   = images.PublishedRevision(grade_id);
  const auto source_repr = images.PublishedRepresentation(source_a);
  const auto union_repr  = images.PublishedRepresentation(union_id);
  const auto grade_repr  = images.PublishedRepresentation(grade_id);
  ASSERT_NE(source_rev, 0U);

  BrushMaskSource missing;
  missing.asset_key         = MaskAssetKey{"missing.asset"};
  missing.descriptor.extent = {1, 1};
  document_.PrimaryGrade()->ReplaceMaskSource(MaskId{"mask.a"}, missing);
  EXPECT_THROW((void)device_.Execute(plan_, prepared_, document_, store_.get()),
               std::runtime_error);
  device_.WaitIdle();
  const auto completed = device_.Workspace().Device().CompletedSubmission();
  EXPECT_TRUE(images.FindValidResult(source_a, source_rev, source_repr, completed));
  EXPECT_TRUE(images.FindValidResult(union_id, union_rev, union_repr, completed));
  EXPECT_TRUE(images.FindValidResult(grade_id, grade_rev, grade_repr, completed));
  EXPECT_EQ(images.PublishedRevision(source_a), source_rev);
  EXPECT_NE(invalidation.RequiredRevision(source_a), source_rev);
  EXPECT_NE(invalidation.RequiredRevision(union_id), union_rev);
  EXPECT_NE(invalidation.RequiredRevision(grade_id), grade_rev);
}

TEST_F(MetalMaskFixture, ActiveMaskTexturesReleaseAfterGpuCompletion) {
  auto asset = MakeRaster(90);
  AttachRaster(asset);
  Compile();
  const auto first_input = MakeActive(
      asset, {0, 0, static_cast<std::int32_t>(width_), static_cast<std::int32_t>(height_)}, 1, 1);
  ExecutePlan(std::span{&first_input, 1});
  const auto first_key =
      ActiveRasterTextureKey{document_.PrimaryGrade()->Id(), MaskId{"mask.raster"}, 1};
  EXPECT_TRUE(device_.Workspace().ActiveRasterTextures().Contains(first_key));
  const auto second_input = MakeActive(
      asset, {0, 0, static_cast<std::int32_t>(width_), static_cast<std::int32_t>(height_)}, 1, 2);
  ExecutePlan(std::span{&second_input, 1});
  const auto second_key =
      ActiveRasterTextureKey{document_.PrimaryGrade()->Id(), MaskId{"mask.raster"}, 2};
  EXPECT_FALSE(device_.Workspace().ActiveRasterTextures().Contains(first_key));
  EXPECT_TRUE(device_.Workspace().ActiveRasterTextures().Contains(second_key));
}

TEST_F(MetalMaskFixture, BrushRadialAndLinearGradientShareUnionRules) {
  auto brush = MakeRaster(0);
  for (std::uint32_t y = 0; y < height_; ++y) {
    for (std::uint32_t x = 0; x < width_ / 2; ++x) {
      brush.pixels[y * width_ + x] = 220;
    }
  }
  brush.key = store_->Put(brush.descriptor, brush.pixels);
  grade_mask_test::AddMask(*document_.PrimaryGrade(),
                           grade_mask_test::MakeBrushMask(MaskId{"mask.brush"}, brush));
  RadialMaskSource radial;
  radial.major_radius = 0.25f;
  radial.minor_radius = 0.25f;
  grade_mask_test::AddRadialMask(document_, MaskId{"mask.radial"}, radial);
  LinearGradientMaskSource gradient;
  gradient.transition_distance = 1.0f;
  grade_mask_test::AddLinearGradientMask(document_, MaskId{"mask.linear"}, gradient);
  Compile();
  ExecutePlan();
  const auto combined = DownloadMask();
  ExpectPrimaryUnionMatchesReference();
  document_.PrimaryGrade()->SetMaskEnabled(MaskId{"mask.radial"}, false);
  document_.PrimaryGrade()->SetMaskEnabled(MaskId{"mask.linear"}, false);
  Compile();
  ExecutePlan();
  const auto brush_only = DownloadMask();
  document_.PrimaryGrade()->SetMaskEnabled(MaskId{"mask.brush"}, false);
  document_.PrimaryGrade()->SetMaskEnabled(MaskId{"mask.radial"}, true);
  Compile();
  ExecutePlan();
  const auto radial_only = DownloadMask();
  document_.PrimaryGrade()->SetMaskEnabled(MaskId{"mask.radial"}, false);
  document_.PrimaryGrade()->SetMaskEnabled(MaskId{"mask.linear"}, true);
  Compile();
  ExecutePlan();
  const auto linear_only = DownloadMask();
  ASSERT_EQ(combined.size(), brush_only.size());
  for (std::size_t i = 0; i < combined.size(); ++i) {
    const auto separate_max =
        static_cast<std::uint8_t>(std::max({brush_only[i], radial_only[i], linear_only[i]}));
    EXPECT_NEAR(combined[i], separate_max, multi_mask_test::kR8ToleranceCodes) << "index " << i;
  }
}

TEST_F(MetalMaskFixture, MaskOpacityAndInvertApplyBeforeUnion) {
  RadialMaskSource radial;
  radial.major_radius = 0.45f;
  radial.minor_radius = 0.45f;
  auto& inverted = grade_mask_test::AddRadialMask(document_, MaskId{"mask.invert"}, radial, true);
  inverted.opacity = 0.25f;
  auto fill        = MakeRaster(128);
  fill.key         = store_->Put(fill.descriptor, fill.pixels);
  grade_mask_test::AddMask(*document_.PrimaryGrade(),
                           grade_mask_test::MakeBrushMask(MaskId{"mask.flat"}, fill));
  Compile();
  ExecutePlan();
  ExpectPrimaryUnionMatchesReference();
  EXPECT_NEAR(DownloadMask().front(), 128, multi_mask_test::kR8ToleranceCodes);
}

TEST_F(MetalMaskFixture, DirtyUnionRegionCanDecreaseCoverage) {
  auto high = MakeRaster(255);
  auto low  = MakeRaster(80);
  high.key  = store_->Put(high.descriptor, high.pixels);
  low.key   = store_->Put(low.descriptor, low.pixels);
  grade_mask_test::AddMask(*document_.PrimaryGrade(),
                           grade_mask_test::MakeBrushMask(MaskId{"mask.raster"}, high));
  grade_mask_test::AddMask(*document_.PrimaryGrade(),
                           grade_mask_test::MakeBrushMask(MaskId{"mask.low"}, low));
  document_.MarkTopologyDirty();
  Compile();
  ExecutePlan();
  const auto full = MakeActive(high, {0, 0, static_cast<std::int32_t>(width_),
                                      static_cast<std::int32_t>(height_)},
                               1);
  ExecutePlan(std::span{&full, 1});
  const auto before = DownloadMask();
  EXPECT_EQ(before[2 * width_ + 2], 255);
  for (std::uint32_t y = 1; y < 5; ++y) {
    for (std::uint32_t x = 1; x < 5; ++x) {
      high.pixels[y * width_ + x] = 0;
    }
  }
  device_.Workspace().Device().ResetCounters();
  const auto dirty = MakeActive(high, {1, 1, 4, 4}, 2);
  ExecutePlan(std::span{&dirty, 1});
  multi_mask_test::ExpectDirtyR8RectangleUpload(
      device_.Workspace().Device().LastTextureRectangles(), {1, 1, 4, 4},
      device_.Workspace().Device().HostToDeviceBytes(), width_, height_);
  const auto after = DownloadMask();
  EXPECT_EQ(after[2 * width_ + 2], 80);
  EXPECT_LT(after[2 * width_ + 2], before[2 * width_ + 2]);
  std::vector<multi_mask_test::BrushRasterView> views{
      {MaskId{"mask.raster"}, high.pixels, high.descriptor.extent},
      {MaskId{"mask.low"}, low.pixels, low.descriptor.extent},
  };
  multi_mask_test::ExpectR8WithinTolerance(
      after, multi_mask_test::EvaluateEnabledUnionR8(document_.PrimaryGrade()->Masks(), views,
                                                    plan_.geometry));
}

TEST_F(MetalMaskFixture, MetalMultiMaskUnionMatchesReference) {
  {
    SCOPED_TRACE("empty list");
    Compile();
    EXPECT_FALSE(plan_.Contains(GpuPassKind::MaskEvaluate));
    ExecutePlan();
  }
  {
    SCOPED_TRACE("three disabled");
    SetExtent(16, 12);
    grade_mask_test::AddRadialMask(document_, MaskId{"mask.a"});
    grade_mask_test::AddRadialMask(document_, MaskId{"mask.b"});
    grade_mask_test::AddRadialMask(document_, MaskId{"mask.c"});
    document_.PrimaryGrade()->SetMaskEnabled(MaskId{"mask.a"}, false);
    document_.PrimaryGrade()->SetMaskEnabled(MaskId{"mask.b"}, false);
    document_.PrimaryGrade()->SetMaskEnabled(MaskId{"mask.c"}, false);
    Compile();
    ExecutePlan();
    ExpectPrimaryUnionMatchesReference();
  }
  {
    SCOPED_TRACE("one radial");
    SetExtent(16, 12);
    RadialMaskSource radial;
    radial.major_radius = 0.35f;
    grade_mask_test::AddRadialMask(document_, MaskId{"mask.radial"}, radial);
    Compile();
    ExecutePlan();
    ExpectPrimaryUnionMatchesReference();
  }
  {
    SCOPED_TRACE("one linear gradient");
    SetExtent(16, 12);
    LinearGradientMaskSource gradient;
    gradient.transition_distance = 0.8f;
    grade_mask_test::AddLinearGradientMask(document_, MaskId{"mask.linear"}, gradient);
    Compile();
    ExecutePlan();
    ExpectPrimaryUnionMatchesReference();
  }
  {
    SCOPED_TRACE("one settled brush");
    SetExtent(16, 12);
    auto asset = MakeRaster(0);
    asset.pixels[(height_ / 2) * width_ + width_ / 2] = 200;
    AttachRaster(asset);
    Compile();
    ExecutePlan();
    ExpectPrimaryUnionMatchesReference();
  }
  {
    SCOPED_TRACE("three source kinds");
    SetExtent(16, 12);
    auto asset = MakeRaster(40);
    asset.key  = store_->Put(asset.descriptor, asset.pixels);
    grade_mask_test::AddMask(*document_.PrimaryGrade(),
                             grade_mask_test::MakeBrushMask(MaskId{"mask.brush"}, asset));
    grade_mask_test::AddRadialMask(document_, MaskId{"mask.radial"});
    grade_mask_test::AddLinearGradientMask(document_, MaskId{"mask.linear"});
    Compile();
    ExecutePlan();
    ExpectPrimaryUnionMatchesReference();
  }
  {
    SCOPED_TRACE("two grades");
    SetExtent(16, 12);
    multi_grade_test::AddCleanGradesBeforeDrt(document_, {"grade.b"});
    RadialMaskSource wide;
    wide.major_radius = 0.4f;
    grade_mask_test::AddRadialMask(document_, MaskId{"mask.primary"}, wide);
    auto* extra = multi_grade_test::GradeNode(document_, "grade.b");
    ASSERT_NE(extra, nullptr);
    LinearGradientMaskSource gradient;
    gradient.transition_distance = 1.0f;
    grade_mask_test::AddMask(
        *extra, grade_mask_test::MakeLinearGradientMask(MaskId{"mask.second"}, gradient));
    document_.MarkTopologyDirty();
    Compile();
    ExecutePlan();
    ASSERT_EQ(plan_.grade_nodes.size(), 2U);
    multi_mask_test::ExpectR8WithinTolerance(
        DownloadR8(plan_.grade_nodes[0].mask_output),
        multi_mask_test::EvaluateEnabledUnionR8(document_.PrimaryGrade()->Masks(), {},
                                                plan_.geometry));
    multi_mask_test::ExpectR8WithinTolerance(
        DownloadR8(plan_.grade_nodes[1].mask_output),
        multi_mask_test::EvaluateEnabledUnionR8(extra->Masks(), {}, plan_.geometry));
  }
  {
    SCOPED_TRACE("active dirty update");
    SetExtent(16, 12);
    auto asset = MakeRaster(30);
    AttachRaster(asset);
    Compile();
    ExecutePlan();
    std::fill(asset.pixels.begin(), asset.pixels.end(), 210);
    const auto full = MakeActive(asset, {0, 0, static_cast<std::int32_t>(width_),
                                         static_cast<std::int32_t>(height_)},
                                 1);
    ExecutePlan(std::span{&full, 1});
    device_.Workspace().Device().ResetCounters();
    for (std::uint32_t y = 2; y < 6; ++y) {
      for (std::uint32_t x = 2; x < 6; ++x) {
        asset.pixels[y * width_ + x] = 10;
      }
    }
    const auto dirty = MakeActive(asset, {2, 2, 4, 4}, 2);
    ExecutePlan(std::span{&dirty, 1});
    multi_mask_test::ExpectDirtyR8RectangleUpload(
        device_.Workspace().Device().LastTextureRectangles(), {2, 2, 4, 4},
        device_.Workspace().Device().HostToDeviceBytes(), width_, height_);
    std::vector<multi_mask_test::BrushRasterView> views{
        {MaskId{"mask.raster"}, asset.pixels, asset.descriptor.extent}};
    ExpectPrimaryUnionMatchesReference(views);
  }
  {
    SCOPED_TRACE("missing asset fails without replacement");
    SetExtent(16, 12);
    BrushMaskSource missing;
    missing.descriptor.extent = {1, 1};
    grade_mask_test::AddMask(
        *document_.PrimaryGrade(),
        grade_mask_test::MakeBrushMask(MaskId{"mask.missing"}, MaskAssetKey{"missing.asset"},
                                       missing.descriptor));
    document_.MarkTopologyDirty();
    Compile();
    EXPECT_THROW(ExecutePlan(), std::runtime_error);
  }
}

}  // namespace
}  // namespace alcedo
