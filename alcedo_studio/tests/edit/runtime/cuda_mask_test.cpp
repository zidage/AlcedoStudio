//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <cuda_runtime.h>
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
#include <variant>
#include <vector>

#include "../graph/grade_owned_mask_support.hpp"
#include "../graph/test_camera_profile.hpp"
#include "../input/prepared_raw_test_support.hpp"
#include "edit/input/raw_input_loader.hpp"
#include "edit/mask/active_raster_mask.hpp"
#include "edit/operators/models/scalar_operator_model.hpp"
#include "edit/runtime/cuda/cuda_develop_pass.hpp"
#include "edit/runtime/cuda/cuda_mask_pass.hpp"
#include "edit/runtime/cuda/cuda_primary_grade_pass.hpp"
#include "edit/runtime/graph_compiler.hpp"
#include "edit/runtime/pass_kind.hpp"
#include "edit/runtime/result_content_key.hpp"
#include "edit/runtime/texture_format.hpp"
#include "gpu/transient_allocation_policy.hpp"
#include "multi_grade_runtime_test_support.hpp"
#include "multi_mask_runtime_test_support.hpp"

namespace alcedo {
namespace {

auto HasCudaDevice() -> bool {
  int count = 0;
  return ::cudaGetDeviceCount(&count) == cudaSuccess && count > 0;
}

class CudaMaskFixture : public ::testing::Test {
 protected:
  void SetUp() override {
    if (!HasCudaDevice()) GTEST_SKIP() << "No CUDA device available.";
    SetExtent(16, 12);
    root_ = std::filesystem::path{"build/tmp/g6_cuda_mask"} /
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
    asset.key = store_->Put(asset.descriptor, asset.pixels);
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

  auto RenderMask(std::span<const ActiveRasterMaskInput> active = {}) -> CudaMaskResult {
    device_.BeginRender();
    auto result = ExecuteCudaMask(device_, plan_, document_, store_.get(), active);
    device_.EndRender();
    device_.WaitIdle();
    return result;
  }

  auto RenderGrade() -> CudaPrimaryGradeResult {
    device_.BeginRender();
    (void)ExecuteCudaDevelop(device_, plan_, prepared_, document_);
    ExecuteCudaGeometryResample(device_, plan_);
    ExecuteCudaCameraColor(device_, plan_, document_);
    (void)ExecuteCudaMask(device_, plan_, document_, store_.get());
    auto result = ExecuteCudaPrimaryGrade(device_, plan_, prepared_, document_);
    device_.EndRender();
    device_.WaitIdle();
    return result;
  }

  auto DownloadMask() -> std::vector<std::uint8_t> {
    return DownloadR8(plan_.FirstGrade()->mask_output);
  }

  auto DownloadR8(const GraphValueId& id) -> std::vector<std::uint8_t> {
    auto* lease = device_.Workspace().Images().Find(id);
    EXPECT_NE(lease, nullptr);
    if (lease == nullptr) return {};
    std::vector<std::uint8_t> pixels(lease->Texture().Bytes());
    device_.Workspace().Device().DownloadTexture2D(
        lease->Texture(),
        std::span<std::byte>(reinterpret_cast<std::byte*>(pixels.data()), pixels.size()),
        device_.CommandContext());
    return pixels;
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

  struct Rgba {
    float r, g, b, a;
  };
  auto DownloadImage(const GraphValueId& id) -> std::vector<Rgba> {
    auto* lease = device_.Workspace().Images().Find(id);
    EXPECT_NE(lease, nullptr);
    if (lease == nullptr) return {};
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
  CudaRenderDevice           device_;
};

TEST_F(CudaMaskFixture, CudaMaskTextureCacheReusesTextureForSameMaskAssetKey) {
  AttachRaster(MakeRaster(255));
  Compile();
  const auto first = RenderMask();
  EXPECT_GT(first.mip_level_count, 1U);
  RenderRequest request;
  request.resolution.render_scale = 0.5f;
  Compile(request);
  const auto second = RenderMask();
  EXPECT_EQ(first.persistent_texture_resource_id, second.persistent_texture_resource_id);
}

TEST_F(CudaMaskFixture, CudaMaskTextureCacheDoesNotEvictTextureUsedByActiveSubmission) {
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

TEST_F(CudaMaskFixture, CudaActiveRasterRevisionUploadsOnlyDirtyRectangle) {
  auto asset = MakeRaster(0);
  AttachRaster(asset);
  Compile();
  RenderMask();
  std::fill(asset.pixels.begin(), asset.pixels.end(), 200);
  const auto full = MakeActive(asset, {0, 0, static_cast<std::int32_t>(width_),
                                       static_cast<std::int32_t>(height_)},
                               1);
  RenderMask(std::span{&full, 1});
  device_.Workspace().Device().ResetCounters();
  const auto dirty = MakeActive(asset, {1, 1, 4, 4}, 2);
  RenderMask(std::span{&dirty, 1});
  ASSERT_EQ(device_.Workspace().Device().LastTextureRectangles().size(), 1U);
  EXPECT_EQ(device_.Workspace().Device().LastTextureRectangles().front(), (RectI{1, 1, 4, 4}));
  EXPECT_EQ(device_.Workspace().Device().HostToDeviceBytes(), 16U);
}

TEST_F(CudaMaskFixture, CudaActiveRasterUpdateNeverPatchesPersistentTexture) {
  auto asset = MakeRaster(17);
  AttachRaster(asset);
  Compile();
  const auto persistent = RenderMask();
  ASSERT_NE(persistent.persistent_texture_resource_id, 0U);
  auto&      texture = device_.Workspace().MaskTextures().TextureAt(AttachedBrushKey());
  std::vector<std::uint8_t> before(texture.Bytes());
  device_.Workspace().Device().DownloadTexture2D(
      texture, std::span<std::byte>(reinterpret_cast<std::byte*>(before.data()), before.size()),
      device_.CommandContext());
  EXPECT_EQ(before, asset.pixels);

  auto preview = asset;
  std::fill(preview.pixels.begin(), preview.pixels.end(), 200);
  const auto active = MakeActive(preview, {0, 0, static_cast<std::int32_t>(width_),
                                           static_cast<std::int32_t>(height_)},
                                 1);
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

TEST_F(CudaMaskFixture, CudaNewActiveRasterGenerationReplacesOldPreviewTexture) {
  auto asset = MakeRaster(40);
  AttachRaster(asset);
  Compile();
  const auto first_input = MakeActive(asset, {0, 0, static_cast<std::int32_t>(width_),
                                              static_cast<std::int32_t>(height_)},
                                      1, 1);
  const auto first       = RenderMask(std::span{&first_input, 1});
  const auto first_key =
      ActiveRasterTextureKey{document_.PrimaryGrade()->Id(), MaskId{"mask.raster"}, 1};
  EXPECT_TRUE(device_.Workspace().ActiveRasterTextures().Contains(first_key));
  const auto second_input = MakeActive(asset, {0, 0, static_cast<std::int32_t>(width_),
                                               static_cast<std::int32_t>(height_)},
                                       1, 2);
  const auto second       = RenderMask(std::span{&second_input, 1});
  const auto second_key =
      ActiveRasterTextureKey{document_.PrimaryGrade()->Id(), MaskId{"mask.raster"}, 2};
  EXPECT_NE(first.active_texture_resource_id, second.active_texture_resource_id);
  EXPECT_FALSE(device_.Workspace().ActiveRasterTextures().Contains(first_key));
  EXPECT_TRUE(device_.Workspace().ActiveRasterTextures().Contains(second_key));
}

TEST_F(CudaMaskFixture, CudaRadialMaskMatchesReferenceSpaceEllipseAtPreviewScales) {
  auto&            node = AttachAnalytic(MaskSourceKind::Radial);
  auto             radial = std::get<RadialMaskSource>(node.source);
  radial.major_radius = 0.3f;
  radial.minor_radius = 0.2f;
  node.source = radial;
  Compile();
  RenderMask();
  const auto full = DownloadMask();
  EXPECT_GT(full[(height_ / 2) * width_ + width_ / 2], 240);
  EXPECT_LT(full.front(), 10);
  RenderRequest request;
  request.resolution.render_scale = 0.5f;
  Compile(request);
  RenderMask();
  const auto preview = DownloadMask();
  EXPECT_GT(preview[(plan_.geometry.render_extent.height / 2) * plan_.geometry.render_extent.width +
                    plan_.geometry.render_extent.width / 2],
            230);
  EXPECT_LT(preview.front(), 20);
}

TEST_F(CudaMaskFixture, CudaLinearGradientMaskFollowsReferenceSpaceNormal) {
  auto&                    node = AttachAnalytic(MaskSourceKind::LinearGradient);
  LinearGradientMaskSource params;
  params.normal_x            = 0.0f;
  params.normal_y            = 1.0f;
  params.transition_distance = 1.0f;
  node.source = params;
  Compile();
  RenderMask();
  const auto pixels = DownloadMask();
  EXPECT_GT(pixels[width_ / 2], pixels[(height_ - 1) * width_ + width_ / 2]);
}

TEST_F(CudaMaskFixture, CudaFeatherPreservesZeroAndOnePlateaus) {
  auto asset = MakeRaster(0);
  for (std::uint32_t y = 3; y < 9; ++y)
    for (std::uint32_t x = 4; x < 12; ++x) asset.pixels[y * width_ + x] = 255;
  AttachRaster(asset, 1.0f);
  Compile();
  RenderMask();
  const auto pixels = DownloadMask();
  EXPECT_EQ(pixels.front(), 0);
  EXPECT_EQ(pixels[6 * width_ + 8], 255);
}

TEST_F(CudaMaskFixture, CudaSignedDistanceFeatherMatchesExactEuclideanReferenceWithinTolerance) {
  SetExtent(9, 9);
  auto asset = MakeRaster(0);
  for (std::uint32_t y = 3; y <= 5; ++y)
    for (std::uint32_t x = 3; x <= 5; ++x) asset.pixels[y * width_ + x] = 255;
  AttachRaster(asset, 2.0f);
  Compile();
  RenderMask();
  const auto pixels = DownloadMask();
  EXPECT_NEAR(pixels[4 * width_ + 2], 81, 8);
  EXPECT_NEAR(pixels[2 * width_ + 2], 46, 10);
  EXPECT_GT(pixels[4 * width_ + 4], 240);
}

TEST_F(CudaMaskFixture, CudaFeatherPreservesAntialiasedSourceBoundary) {
  auto asset                   = MakeRaster(0);
  asset.pixels[6 * width_ + 8] = 128;
  AttachRaster(asset, 2.0f);
  Compile();
  RenderMask();
  const auto pixels = DownloadMask();
  EXPECT_NEAR(pixels[6 * width_ + 8], 128, 8);
}

TEST_F(CudaMaskFixture, CudaFeatherRadiusIsStableAcrossDynamicRenderScales) {
  auto asset = MakeRaster(0);
  for (std::uint32_t y = 0; y < height_; ++y)
    for (std::uint32_t x = 8; x < width_; ++x) asset.pixels[y * width_ + x] = 255;
  AttachRaster(asset, 3.0f);
  Compile();
  RenderMask();
  const auto    full       = DownloadMask();
  const auto    full_value = full[6 * width_ + 7];
  RenderRequest request;
  request.resolution.render_scale = 0.5f;
  Compile(request);
  RenderMask();
  const auto half       = DownloadMask();
  const auto half_value = half[3 * plan_.geometry.render_extent.width + 3];
  EXPECT_NEAR(full_value, half_value, 35);
}

TEST_F(CudaMaskFixture, ChangingFeatherRadiusReusesSignedDistanceTexture) {
  auto asset = MakeRaster(0);
  for (std::uint32_t y = 3; y < 9; ++y)
    for (std::uint32_t x = 4; x < 12; ++x) asset.pixels[y * width_ + x] = 255;
  auto& node = AttachRaster(asset, 1.0f);
  Compile();
  const auto first = RenderMask();
  std::get<BrushMaskSource>(node.source).feather_radius = 4.0f;
  const auto second = RenderMask();
  EXPECT_NE(first.signed_distance_resource_id, 0U);
  EXPECT_EQ(first.signed_distance_resource_id, second.signed_distance_resource_id);
}

TEST_F(CudaMaskFixture, CudaColorGradeMixUsesInputAtMaskZeroAndAdjustedAtMaskOne) {
  auto asset = MakeRaster(255);
  AttachRaster(asset);
  auto* exposure = dynamic_cast<ExposureModel*>(
      document_.PrimaryGrade()->FindAdjustmentByType(type_ids::Exposure()));
  ASSERT_NE(exposure, nullptr);
  exposure->SetValue(1.0f);
  Compile();
  const auto full_result = RenderGrade();
  const auto full        = DownloadImage(full_result.output);
  for (std::uint32_t y = 0; y < height_; ++y)
    for (std::uint32_t x = 0; x < width_ / 2; ++x) asset.pixels[y * width_ + x] = 0;
  asset.key = store_->Put(asset.descriptor, asset.pixels);
  document_.PrimaryGrade()->ReplaceMaskSource(
      MaskId{"mask.raster"},
      grade_mask_test::MakeBrushMask(MaskId{"mask.raster"}, asset).source);
  Compile();
  const auto mixed  = RenderGrade();
  const auto source = DownloadImage(plan_.develop_output);
  const auto output = DownloadImage(mixed.output);
  ASSERT_EQ(source.size(), output.size());
  const auto left  = 5 * width_ + 2;
  const auto right = 5 * width_ + width_ - 2;
  EXPECT_NEAR(output[left].r, source[left].r, 1.0e-6f);
  EXPECT_NEAR(output[right].r, full[right].r, 1.0e-6f);
}

TEST_F(CudaMaskFixture, EmptyMaskListUsesFullGradeCoverage) {
  auto* exposure = dynamic_cast<ExposureModel*>(
      document_.PrimaryGrade()->FindAdjustmentByType(type_ids::Exposure()));
  ASSERT_NE(exposure, nullptr);
  exposure->SetValue(1.0f);
  Compile();
  EXPECT_FALSE(plan_.Contains(GpuPassKind::MaskEvaluate));
  ASSERT_EQ(device_.Execute(plan_, prepared_, document_, store_.get()), plan_.display_output);
  device_.WaitIdle();
  const auto empty_grade = DownloadImage(plan_.FirstGrade()->scene_output);
  const auto empty_keys  = BuildFrameResultContentKeys(plan_, prepared_, document_);

  grade_mask_test::AddRadialMask(document_, MaskId{"mask.radial"});
  Compile();
  ASSERT_EQ(device_.Execute(plan_, prepared_, document_, store_.get()), plan_.display_output);
  device_.WaitIdle();
  const auto masked_grade = DownloadImage(plan_.FirstGrade()->scene_output);

  document_.PrimaryGrade()->RemoveMask(MaskId{"mask.radial"});
  Compile();
  ASSERT_EQ(device_.Execute(plan_, prepared_, document_, store_.get()), plan_.display_output);
  device_.WaitIdle();
  const auto restored_grade = DownloadImage(plan_.FirstGrade()->scene_output);
  const auto restored_keys  = BuildFrameResultContentKeys(plan_, prepared_, document_);
  ASSERT_EQ(empty_grade.size(), restored_grade.size());
  EXPECT_NEAR(empty_grade.front().r, restored_grade.front().r, 1.0e-5f);
  EXPECT_EQ(restored_keys.primary_grade, empty_keys.primary_grade);
  EXPECT_GT(std::abs(empty_grade.front().r - masked_grade.front().r), 1.0e-4f);
}

TEST_F(CudaMaskFixture, AllDisabledMasksUseZeroGradeCoverage) {
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
  ASSERT_EQ(device_.Execute(plan_, prepared_, document_, store_.get()), plan_.display_output);
  device_.WaitIdle();
  const auto keys = BuildFrameResultContentKeys(plan_, prepared_, document_);
  EXPECT_EQ(keys.Value(plan_.FirstGrade()->mask_output), AllDisabledMaskUnionKey());
  const auto scene = DownloadImage(plan_.FirstGrade()->scene_input);
  const auto grade = DownloadImage(plan_.FirstGrade()->scene_output);
  ASSERT_EQ(scene.size(), grade.size());
  EXPECT_NEAR(grade.front().r, scene.front().r, 1.0e-5f);
  EXPECT_NEAR(grade[grade.size() / 2].r, scene[scene.size() / 2].r, 1.0e-5f);
}

TEST_F(CudaMaskFixture, OneMaskEditReusesSiblingAndUpstreamResults) {
  RadialMaskSource wide;
  wide.major_radius = 0.45f;
  RadialMaskSource narrow;
  narrow.major_radius = 0.2f;
  grade_mask_test::AddRadialMask(document_, MaskId{"mask.a"}, wide);
  grade_mask_test::AddRadialMask(document_, MaskId{"mask.z"}, narrow);
  Compile();
  ASSERT_EQ(device_.Execute(plan_, prepared_, document_, store_.get()), plan_.display_output);
  device_.WaitIdle();
  const auto before = BuildFrameResultContentKeys(plan_, prepared_, document_);
  ASSERT_TRUE(plan_.FirstGrade()->mask_stack.has_value());
  const auto sibling = plan_.FirstGrade()->mask_stack->sources[1].effective_output;
  document_.PrimaryGrade()->SetMaskOpacity(MaskId{"mask.a"}, 0.4f);
  const auto after = BuildFrameResultContentKeys(plan_, prepared_, document_);
  EXPECT_EQ(after.develop_image, before.develop_image);
  EXPECT_EQ(after.Value(sibling), before.Value(sibling));
  EXPECT_NE(after.mask, before.mask);
  device_.ResetPassStats();
  ASSERT_EQ(device_.Execute(plan_, prepared_, document_, store_.get()), plan_.display_output);
  device_.WaitIdle();
  EXPECT_GE(device_.PassStats().camera_color_skip, 1U);
  EXPECT_EQ(device_.PassStats().mask_skip, 1U);
  EXPECT_EQ(device_.PassStats().mask_execute, 1U);
  EXPECT_EQ(device_.PassStats().mask_union_execute, 1U);
  EXPECT_EQ(device_.PassStats().mask_union_skip, 0U);
}

TEST_F(CudaMaskFixture, MaskFailurePublishesNoSourceUnionOrGradeWrites) {
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
  ASSERT_EQ(device_.Execute(plan_, prepared_, document_, store_.get()), plan_.display_output);
  device_.WaitIdle();
  const auto before = BuildFrameResultContentKeys(plan_, prepared_, document_);
  ASSERT_TRUE(plan_.FirstGrade()->mask_stack.has_value());
  const auto source_a = plan_.FirstGrade()->mask_stack->sources[0].effective_output;
  const auto union_id = plan_.FirstGrade()->mask_output;
  const auto grade_id = plan_.FirstGrade()->scene_output;

  BrushMaskSource missing;
  missing.asset_key         = MaskAssetKey{"missing.asset"};
  missing.descriptor.extent = {1, 1};
  document_.PrimaryGrade()->ReplaceMaskSource(MaskId{"mask.a"}, missing);
  const auto after = BuildFrameResultContentKeys(plan_, prepared_, document_);
  EXPECT_NE(after.Value(source_a), before.Value(source_a));
  EXPECT_THROW((void)device_.Execute(plan_, prepared_, document_, store_.get()),
               std::runtime_error);
  device_.WaitIdle();
  const auto completed = device_.Workspace().Device().CompletedSubmission();
  EXPECT_TRUE(device_.Workspace().Images().FindValidResult(
      source_a, before.Value(source_a), before.geometry_extent, TextureFormat::R8, completed));
  EXPECT_TRUE(device_.Workspace().Images().FindValidResult(
      union_id, before.mask, before.geometry_extent, TextureFormat::R8, completed));
  EXPECT_TRUE(device_.Workspace().Images().FindValidResult(
      grade_id, before.primary_grade, before.geometry_extent, TextureFormat::Rgba32f, completed));
  EXPECT_FALSE(device_.Workspace().Images().FindValidResult(
      source_a, after.Value(source_a), after.geometry_extent, TextureFormat::R8, completed));
  EXPECT_FALSE(device_.Workspace().Images().FindValidResult(
      union_id, after.mask, after.geometry_extent, TextureFormat::R8, completed));
  EXPECT_FALSE(device_.Workspace().Images().FindValidResult(
      grade_id, after.primary_grade, after.geometry_extent, TextureFormat::Rgba32f, completed));
}

TEST_F(CudaMaskFixture, ActiveMaskTexturesReleaseAfterGpuCompletion) {
  auto asset = MakeRaster(90);
  AttachRaster(asset);
  Compile();
  const auto first_input = MakeActive(asset, {0, 0, static_cast<std::int32_t>(width_),
                                              static_cast<std::int32_t>(height_)},
                                      1, 1);
  ASSERT_EQ(device_.Execute(plan_, prepared_, document_, store_.get(), true,
                            TransientAllocationPolicy::SessionPacked, std::span{&first_input, 1}),
            plan_.display_output);
  device_.WaitIdle();
  const auto first_key =
      ActiveRasterTextureKey{document_.PrimaryGrade()->Id(), MaskId{"mask.raster"}, 1};
  EXPECT_TRUE(device_.Workspace().ActiveRasterTextures().Contains(first_key));
  const auto second_input = MakeActive(asset, {0, 0, static_cast<std::int32_t>(width_),
                                               static_cast<std::int32_t>(height_)},
                                       1, 2);
  ASSERT_EQ(device_.Execute(plan_, prepared_, document_, store_.get(), true,
                            TransientAllocationPolicy::SessionPacked, std::span{&second_input, 1}),
            plan_.display_output);
  device_.WaitIdle();
  const auto second_key =
      ActiveRasterTextureKey{document_.PrimaryGrade()->Id(), MaskId{"mask.raster"}, 2};
  EXPECT_FALSE(device_.Workspace().ActiveRasterTextures().Contains(first_key));
  EXPECT_TRUE(device_.Workspace().ActiveRasterTextures().Contains(second_key));
}

TEST_F(CudaMaskFixture, EnabledMasksUseMaximumCoverage) {
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
  ExecutePlan();
  ExpectPrimaryUnionMatchesReference();
  const auto unified = DownloadMask();
  const auto left    = 5 * width_ + 2;
  const auto right   = 5 * width_ + width_ - 2;
  EXPECT_EQ(unified[left], 180);
  EXPECT_EQ(unified[right], 200);
  EXPECT_NE(unified[left], static_cast<std::uint8_t>(180 + 200));
}

TEST_F(CudaMaskFixture, BrushRadialAndLinearGradientShareUnionRules) {
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

TEST_F(CudaMaskFixture, MaskOpacityAndInvertApplyBeforeUnion) {
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
  const auto unified = DownloadMask();
  EXPECT_NEAR(unified.front(), 128, multi_mask_test::kR8ToleranceCodes);
  EXPECT_LT(unified.front(), 200);
  EXPECT_NEAR(unified[(height_ / 2) * width_ + width_ / 2], 128,
              multi_mask_test::kR8ToleranceCodes);
}

TEST_F(CudaMaskFixture, DirtyUnionRegionCanDecreaseCoverage) {
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
  EXPECT_EQ(after.front(), 255);
  std::vector<multi_mask_test::BrushRasterView> views{
      {MaskId{"mask.raster"}, high.pixels, high.descriptor.extent},
      {MaskId{"mask.low"}, low.pixels, low.descriptor.extent},
  };
  const auto expected =
      multi_mask_test::EvaluateEnabledUnionR8(document_.PrimaryGrade()->Masks(), views, plan_.geometry);
  multi_mask_test::ExpectR8WithinTolerance(after, expected);
}

TEST_F(CudaMaskFixture, CudaMultiMaskUnionMatchesReference) {
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
    const auto disabled = DownloadMask();
    EXPECT_TRUE(std::all_of(disabled.begin(), disabled.end(),
                            [](std::uint8_t value) { return value == 0; }));
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
    grade_mask_test::AddMask(*extra, grade_mask_test::MakeLinearGradientMask(
                                         MaskId{"mask.second"}, gradient));
    document_.MarkTopologyDirty();
    Compile();
    ExecutePlan();
    ASSERT_EQ(plan_.grade_nodes.size(), 2U);
    const auto primary = multi_mask_test::EvaluateEnabledUnionR8(
        document_.PrimaryGrade()->Masks(), {}, plan_.geometry);
    multi_mask_test::ExpectR8WithinTolerance(
        DownloadR8(plan_.grade_nodes[0].mask_output), primary);
    const auto second = multi_mask_test::EvaluateEnabledUnionR8(extra->Masks(), {}, plan_.geometry);
    multi_mask_test::ExpectR8WithinTolerance(
        DownloadR8(plan_.grade_nodes[1].mask_output), second);
    EXPECT_NE(primary, second);
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
    missing.asset_key         = MaskAssetKey{"missing.asset"};
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
