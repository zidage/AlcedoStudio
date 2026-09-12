//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <cuda_runtime.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <span>
#include <string>
#include <variant>
#include <vector>

#include "../graph/grade_owned_mask_support.hpp"
#include "../graph/test_camera_profile.hpp"
#include "../input/prepared_raw_test_support.hpp"
#include "edit/graph/color_grade_node_model.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/input/raw_input_loader.hpp"
#include "edit/mask/active_raster_mask.hpp"
#include "edit/mask/brush_raster_encoding.hpp"
#include "edit/mask/brush_stroke.hpp"
#include "edit/mask/mask_model.hpp"
#include "edit/mask/parameterized_brush_raster.hpp"
#include "edit/runtime/cuda/cuda_mask_pass.hpp"
#include "edit/runtime/cuda/cuda_render_device.hpp"
#include "edit/runtime/execution_plan.hpp"
#include "edit/runtime/graph_compiler.hpp"

namespace alcedo {
namespace {

auto HasCudaDevice() -> bool {
  int count = 0;
  return ::cudaGetDeviceCount(&count) == cudaSuccess && count > 0;
}

class CudaParameterizedBrushGpuFixture : public ::testing::Test {
 protected:
  static const MaskId kMask;
  static constexpr std::uint32_t kWidth  = 64;
  static constexpr std::uint32_t kHeight = 48;

  void SetUp() override {
    if (!HasCudaDevice()) {
      GTEST_SKIP() << "No CUDA device available.";
    }
    prepared_ = RawInputLoader::FromDirectRgb(gpu_dag_test::MakeF32RgbaPlane(kWidth, kHeight),
                                              gpu_dag_test::FullSensor(kWidth, kHeight));
    document_ = CreateDefaultPipelineDocument();
    gpu_dag_test::EnsureTestCameraProfile(document_);
  }

  void AttachBrush(std::vector<BrushStroke> strokes) {
    grade_mask_test::AddParameterizedBrushMask(document_, kMask, std::move(strokes));
    Compile();
  }

  void ReplaceBrush(BrushMaskSource source) {
    document_.PrimaryGrade()->ReplaceMaskSource(kMask, MaskSource{std::move(source)});
  }

  void Compile() {
    plan_ = GraphCompiler::Compile(document_, prepared_.CompileSource(), {});
    ASSERT_NE(plan_.FirstGrade(), nullptr);
  }

  void RenderMask() {
    device_.BeginRender();
    (void)ExecuteCudaMask(device_, plan_, document_, nullptr);
    device_.EndRender();
    device_.WaitIdle();
  }

  auto DownloadCanonicalSource() -> std::vector<std::uint8_t> {
    const auto raster = CanonicalBrushRasterExtent(plan_.geometry.full_reference_extent);
    auto&      cache  = device_.Workspace().ActiveRasterTextures();
    const ActiveRasterTextureKey key{document_.PrimaryGrade()->Id(), kMask, 1};
    EXPECT_TRUE(cache.Contains(key));
    auto lease = cache.Acquire(key, raster);
    std::vector<std::uint8_t> pixels(lease.Texture().Bytes());
    device_.Workspace().Device().DownloadTexture2D(
        lease.Texture(),
        std::span<std::byte>(reinterpret_cast<std::byte*>(pixels.data()), pixels.size()),
        device_.CommandContext());
    return pixels;
  }

  PreparedRawInput   prepared_;
  PipelineDocument   document_;
  ExecutionPlan      plan_;
  CudaRenderDevice   device_;
};

const MaskId CudaParameterizedBrushGpuFixture::kMask{"mask.brush"};

TEST_F(CudaParameterizedBrushGpuFixture, GpuOrderedBrushRasterMatchesCanonicalR8Bytes) {
  AttachBrush({grade_mask_test::MakePaintStroke("paint.a", 18.0f, 12.0f, 6.0f),
               MakeBrushStroke(StrokeId{"erase.b"}, BrushStrokeMode::Erase,
                               {{16.0f, 11.0f, 4.0f, 1.0f, 1.0f}}),
               MakeBrushStroke(StrokeId{"paint.c"}, BrushStrokeMode::Paint,
                               {{22.0f, 14.0f, 5.0f, 0.5f, 0.25f}})});
  RenderMask();
  const auto* brush = std::get_if<BrushMaskSource>(&document_.PrimaryGrade()->FindMask(kMask)->source);
  ASSERT_NE(brush, nullptr);
  const auto expected =
      RasterizeParameterizedBrushSource(*brush, plan_.geometry.full_reference_extent);
  const auto actual = DownloadCanonicalSource();
  ASSERT_EQ(actual.size(), expected.pixels.size());
  EXPECT_EQ(actual, expected.pixels);
}

TEST_F(CudaParameterizedBrushGpuFixture, GrowingBrushStampsNewDabsWithoutHostRasterUpload) {
  AttachBrush({MakeBrushStroke(StrokeId{"draft"}, BrushStrokeMode::Paint,
                               {{12.0f, 10.0f, 6.0f, 1.0f, 1.0f}})});
  RenderMask();

  auto* brush = std::get_if<BrushMaskSource>(&document_.PrimaryGrade()->FindMask(kMask)->source);
  ASSERT_NE(brush, nullptr);
  auto grown = *brush;
  grown.strokes[0] = MakeBrushStroke(StrokeId{"draft"}, BrushStrokeMode::Paint,
                                     {{12.0f, 10.0f, 6.0f, 1.0f, 1.0f},
                                      {40.0f, 30.0f, 5.0f, 0.75f, 0.5f}});
  ReplaceBrush(grown);

  auto& backend = device_.Workspace().Device();
  backend.ResetCounters();
  backend.NoteHostToDeviceBegin();
  RenderMask();
  EXPECT_EQ(backend.HostToDeviceBytes(), 0u);

  brush = std::get_if<BrushMaskSource>(&document_.PrimaryGrade()->FindMask(kMask)->source);
  ASSERT_NE(brush, nullptr);
  const auto expected =
      RasterizeParameterizedBrushSource(*brush, plan_.geometry.full_reference_extent);
  EXPECT_EQ(DownloadCanonicalSource(), expected.pixels);
}

TEST_F(CudaParameterizedBrushGpuFixture, EraseAppendStampsWithoutHostRasterUpload) {
  AttachBrush({grade_mask_test::MakePaintStroke("paint", 20.0f, 16.0f, 10.0f)});
  RenderMask();
  auto* brush = std::get_if<BrushMaskSource>(&document_.PrimaryGrade()->FindMask(kMask)->source);
  ASSERT_NE(brush, nullptr);
  auto grown = *brush;
  grown.strokes.push_back(MakeBrushStroke(StrokeId{"erase"}, BrushStrokeMode::Erase,
                                          {{20.0f, 16.0f, 10.0f, 1.0f, 1.0f}}));
  ReplaceBrush(grown);

  auto& backend = device_.Workspace().Device();
  backend.ResetCounters();
  backend.NoteHostToDeviceBegin();
  RenderMask();
  EXPECT_EQ(backend.HostToDeviceBytes(), 0u);

  brush = std::get_if<BrushMaskSource>(&document_.PrimaryGrade()->FindMask(kMask)->source);
  ASSERT_NE(brush, nullptr);
  const auto expected =
      RasterizeParameterizedBrushSource(*brush, plan_.geometry.full_reference_extent);
  EXPECT_EQ(DownloadCanonicalSource(), expected.pixels);
}

TEST_F(CudaParameterizedBrushGpuFixture, PlacementMoveReplaysDirtyAndMatchesCanonicalR8) {
  AttachBrush({grade_mask_test::MakePaintStroke("paint", 20.0f, 16.0f, 8.0f)});
  RenderMask();
  auto* brush = std::get_if<BrushMaskSource>(&document_.PrimaryGrade()->FindMask(kMask)->source);
  ASSERT_NE(brush, nullptr);
  auto moved                     = *brush;
  moved.placement_translation    = {12.0f, -4.0f};
  ReplaceBrush(moved);
  RenderMask();
  brush = std::get_if<BrushMaskSource>(&document_.PrimaryGrade()->FindMask(kMask)->source);
  ASSERT_NE(brush, nullptr);
  const auto expected =
      RasterizeParameterizedBrushSource(*brush, plan_.geometry.full_reference_extent);
  EXPECT_EQ(DownloadCanonicalSource(), expected.pixels);
}

TEST_F(CudaParameterizedBrushGpuFixture, ReleasedGpuSourceRestampsFromCommands) {
  AttachBrush({grade_mask_test::MakePaintStroke("paint", 18.0f, 12.0f, 6.0f)});
  RenderMask();
  device_.Workspace().ReleaseSessionResources();
  RenderMask();
  const auto* brush =
      std::get_if<BrushMaskSource>(&document_.PrimaryGrade()->FindMask(kMask)->source);
  ASSERT_NE(brush, nullptr);
  const auto expected =
      RasterizeParameterizedBrushSource(*brush, plan_.geometry.full_reference_extent);
  EXPECT_EQ(DownloadCanonicalSource(), expected.pixels);
}

TEST_F(CudaParameterizedBrushGpuFixture, FeatherChangeReusesGpuSourceWithoutHostRasterUpload) {
  AttachBrush({grade_mask_test::MakePaintStroke("paint", 20.0f, 16.0f, 8.0f)});
  RenderMask();
  const auto source_before = DownloadCanonicalSource();

  auto* brush = std::get_if<BrushMaskSource>(&document_.PrimaryGrade()->FindMask(kMask)->source);
  ASSERT_NE(brush, nullptr);
  auto with_feather          = *brush;
  with_feather.feather_radius = 4.0f;
  ReplaceBrush(with_feather);

  auto& backend = device_.Workspace().Device();
  backend.ResetCounters();
  backend.NoteHostToDeviceBegin();
  RenderMask();
  EXPECT_EQ(backend.HostToDeviceBytes(), 0u);
  EXPECT_EQ(DownloadCanonicalSource(), source_before);
}

}  // namespace
}  // namespace alcedo
