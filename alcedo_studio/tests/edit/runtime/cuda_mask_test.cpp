//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <cuda_runtime.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <vector>

#include "../input/prepared_raw_test_support.hpp"
#include "edit/graph/analytic_mask_node_model.hpp"
#include "edit/graph/raster_mask_node_model.hpp"
#include "edit/input/raw_input_loader.hpp"
#include "edit/operators/models/scalar_operator_model.hpp"
#include "edit/runtime/cuda/cuda_develop_pass.hpp"
#include "edit/runtime/cuda/cuda_mask_pass.hpp"
#include "edit/runtime/cuda/cuda_primary_grade_pass.hpp"
#include "edit/runtime/graph_compiler.hpp"

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

  auto RenderMask(std::span<const RectI> dirty = {}) -> CudaMaskResult {
    device_.BeginRender();
    auto result = ExecuteCudaMask(device_, plan_, document_, store_.get(), dirty);
    device_.EndRender();
    device_.WaitIdle();
    return result;
  }

  auto RenderGrade() -> CudaPrimaryGradeResult {
    device_.BeginRender();
    (void)ExecuteCudaDevelop(device_, plan_, prepared_, document_);
    ExecuteCudaGeometryResample(device_, plan_);
    ExecuteCudaCameraColor(device_, plan_, prepared_.color_context, document_);
    (void)ExecuteCudaMask(device_, plan_, document_, store_.get());
    auto result = ExecuteCudaPrimaryGrade(device_, plan_, prepared_.color_context, document_);
    device_.EndRender();
    device_.WaitIdle();
    return result;
  }

  auto DownloadMask() -> std::vector<std::uint8_t> {
    auto* lease = device_.Workspace().Images().Find(plan_.mask_output);
    EXPECT_NE(lease, nullptr);
    if (lease == nullptr) return {};
    std::vector<std::uint8_t> pixels(lease->Texture().Bytes());
    device_.Workspace().Device().DownloadTexture2D(
        lease->Texture(),
        std::span<std::byte>(reinterpret_cast<std::byte*>(pixels.data()), pixels.size()),
        device_.CommandContext());
    return pixels;
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
  AttachRaster(MakeRaster("reuse", 255));
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

TEST_F(CudaMaskFixture, CudaRasterMaskUploadsOnlyUnionedDirtyRectangle) {
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

TEST_F(CudaMaskFixture, CudaRadialMaskMatchesReferenceSpaceEllipseAtPreviewScales) {
  auto&            node = AttachAnalytic(AnalyticMaskKind::Radial);
  RadialMaskParams radial;
  radial.major_radius = 0.3f;
  radial.minor_radius = 0.2f;
  node.SetRadial(radial);
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

TEST_F(CudaMaskFixture, CudaGraduatedNdMaskFollowsReferenceSpaceNormal) {
  auto&                 node = AttachAnalytic(AnalyticMaskKind::GraduatedNd);
  GraduatedNdMaskParams params;
  params.normal_x            = 0.0f;
  params.normal_y            = 1.0f;
  params.transition_distance = 1.0f;
  node.SetGraduatedNd(params);
  Compile();
  RenderMask();
  const auto pixels = DownloadMask();
  EXPECT_GT(pixels[width_ / 2], pixels[(height_ - 1) * width_ + width_ / 2]);
}

TEST_F(CudaMaskFixture, CudaFeatherPreservesZeroAndOnePlateaus) {
  auto asset = MakeRaster("plateaus", 0);
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
  auto asset = MakeRaster("exact", 0);
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
  auto asset                   = MakeRaster("antialias", 0);
  asset.pixels[6 * width_ + 8] = 128;
  AttachRaster(asset, 2.0f);
  Compile();
  RenderMask();
  const auto pixels = DownloadMask();
  EXPECT_NEAR(pixels[6 * width_ + 8], 128, 8);
}

TEST_F(CudaMaskFixture, CudaFeatherRadiusIsStableAcrossDynamicRenderScales) {
  auto asset = MakeRaster("scale", 0);
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
  auto asset = MakeRaster("radius", 0);
  for (std::uint32_t y = 3; y < 9; ++y)
    for (std::uint32_t x = 4; x < 12; ++x) asset.pixels[y * width_ + x] = 255;
  auto& node = AttachRaster(asset, 1.0f);
  Compile();
  const auto first = RenderMask();
  node.SetFeatherRadius(4.0f);
  const auto second = RenderMask();
  EXPECT_NE(first.signed_distance_resource_id, 0U);
  EXPECT_EQ(first.signed_distance_resource_id, second.signed_distance_resource_id);
}

TEST_F(CudaMaskFixture, CudaColorGradeMixUsesInputAtMaskZeroAndAdjustedAtMaskOne) {
  auto asset = MakeRaster("mix", 255);
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
  store_->Save(asset);
  device_.BeginRender();
  (void)ExecuteCudaDevelop(device_, plan_, prepared_, document_);
  ExecuteCudaGeometryResample(device_, plan_);
  ExecuteCudaCameraColor(device_, plan_, prepared_.color_context, document_);
  const RectI dirty{0, 0, static_cast<std::int32_t>(width_ / 2),
                    static_cast<std::int32_t>(height_)};
  (void)ExecuteCudaMask(device_, plan_, document_, store_.get(), std::span{&dirty, 1});
  const auto result = ExecuteCudaPrimaryGrade(device_, plan_, prepared_.color_context, document_);
  device_.EndRender();
  device_.WaitIdle();
  const auto source = DownloadImage(plan_.develop_output);
  const auto output = DownloadImage(result.output);
  ASSERT_EQ(source.size(), output.size());
  const auto left  = 5 * width_ + 2;
  const auto right = 5 * width_ + width_ - 2;
  EXPECT_NEAR(output[left].r, source[left].r, 1.0e-6f);
  EXPECT_NEAR(output[right].r, full[right].r, 1.0e-6f);
}

}  // namespace
}  // namespace alcedo
