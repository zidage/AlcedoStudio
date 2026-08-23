//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <cuda_runtime.h>
#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "../graph/test_camera_profile.hpp"
#include "../input/prepared_raw_test_support.hpp"
#include "edit/graph/legacy_pipeline_importer.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/input/raw_input_loader.hpp"
#include "edit/operators/models/scalar_operator_model.hpp"
#include "edit/runtime/cuda/cuda_render_device.hpp"
#include "edit/runtime/graph_compiler.hpp"

namespace alcedo {
namespace {

struct Rgba {
  float r;
  float g;
  float b;
  float a;
};

auto HasCudaDevice() -> bool {
  int count = 0;
  return ::cudaGetDeviceCount(&count) == cudaSuccess && count > 0;
}

auto Download(CudaRenderDevice& device, const GraphValueId& id) -> std::vector<Rgba> {
  device.WaitIdle();
  auto* image = device.Workspace().Images().Find(id);
  EXPECT_NE(image, nullptr);
  if (image == nullptr) return {};
  const auto&       texture = image->Texture();
  std::vector<Rgba> pixels(static_cast<std::size_t>(texture.Width()) * texture.Height());
  device.Workspace().Device().DownloadTexture2D(
      texture,
      std::span<std::byte>(reinterpret_cast<std::byte*>(pixels.data()),
                           pixels.size() * sizeof(Rgba)),
      device.CommandContext());
  return pixels;
}

auto AllFiniteDisplayValues(const std::vector<Rgba>& pixels) -> bool {
  if (pixels.empty()) return false;
  for (const auto& pixel : pixels) {
    if (!std::isfinite(pixel.r) || !std::isfinite(pixel.g) || !std::isfinite(pixel.b) ||
        !std::isfinite(pixel.a)) {
      return false;
    }
  }
  return true;
}

class CudaDrtProductFixture : public ::testing::Test {
 protected:
  void SetUp() override {
    if (!HasCudaDevice()) GTEST_SKIP() << "No CUDA device available.";
    input_ = RawInputLoader::FromDirectRgb(gpu_dag_test::MakeF32RgbaPlane(16, 12),
                                           gpu_dag_test::FullSensor(16, 12));
  }

  auto Render(PipelineDocument& document) -> std::vector<Rgba> {
    gpu_dag_test::EnsureTestCameraProfile(document);
    const auto plan   = GraphCompiler::Compile(document, input_.CompileSource(), RenderRequest{});
    const auto output = device_.Execute(plan, input_, document);
    EXPECT_TRUE(plan.Contains(GpuPassKind::Drt));
    return Download(device_, output);
  }

  PreparedRawInput input_;
  CudaRenderDevice device_;
};

TEST(GpuDagCudaDrtProduct, DefaultCudaPipelineBuildsThreeVisibleNodes) {
  const auto document = CreateDefaultPipelineDocument();
  EXPECT_EQ(document.Graph().Nodes().size(), 3U);
  EXPECT_EQ(document.Graph().Edges().size(), 2U);
  ASSERT_NE(document.Develop(), nullptr);
  ASSERT_NE(document.PrimaryGrade(), nullptr);
  ASSERT_NE(document.Drt(), nullptr);
  EXPECT_EQ(document.ToJson().at("format_version"), 2);
}

TEST_F(CudaDrtProductFixture, CudaDrtOpenDrtProducesFiniteDisplayReferredOutput) {
  auto       document = CreateDefaultPipelineDocument();
  const auto pixels   = Render(document);
  EXPECT_TRUE(AllFiniteDisplayValues(pixels));
  EXPECT_NEAR(pixels.front().a, 1.0f, 1.0e-6f);
}

TEST_F(CudaDrtProductFixture, CudaDrtAces20ProducesFiniteDisplayReferredOutput) {
  auto document = CreateDefaultPipelineDocument();
  auto params   = document.Drt()->Params().Params();
  params.method = DrtMethod::Aces20;
  document.Drt()->Params().ReplaceParams(params);
  EXPECT_TRUE(AllFiniteDisplayValues(Render(document)));
}

TEST_F(CudaDrtProductFixture, ChangingDrtPeakLuminanceKeepsDevelopAndGradeCacheValid) {
  auto document = CreateDefaultPipelineDocument();
  Render(document);
  auto* develop =
      device_.Workspace().Images().Find(GraphValueId{NodeId{"develop"}, PortId{"image"}});
  auto* grade =
      device_.Workspace().Images().Find(GraphValueId{NodeId{"grade.primary"}, PortId{"image"}});
  ASSERT_NE(develop, nullptr);
  ASSERT_NE(grade, nullptr);
  const auto develop_id = develop->Texture().ResourceId();
  const auto grade_id   = grade->Texture().ResourceId();

  auto       params     = document.Drt()->Params().Params();
  params.peak_luminance = 200.0f;
  document.Drt()->Params().ReplaceParams(params);
  EXPECT_TRUE(AllFiniteDisplayValues(Render(document)));
  EXPECT_EQ(device_.Workspace()
                .Images()
                .Find(GraphValueId{NodeId{"develop"}, PortId{"image"}})
                ->Texture()
                .ResourceId(),
            develop_id);
  EXPECT_EQ(device_.Workspace()
                .Images()
                .Find(GraphValueId{NodeId{"grade.primary"}, PortId{"image"}})
                ->Texture()
                .ResourceId(),
            grade_id);
}

TEST_F(CudaDrtProductFixture, LegacyPipelineImportRendersSameCudaReferenceWithinTolerance) {
  nlohmann::json legacy;
  legacy["Basic Adjustment"]["Basic Adjustment"]["exposure"] = {
      {"type", 2}, {"enable", true}, {"params", {{"exposure", 0.75f}}}};
  legacy["Output Transform"]["Output Transform"]["odt"] = {
      {"type", 17},
      {"enable", true},
      {"params", {{"odt", {{"method", "open_drt"}, {"peak_luminance", 100.0f}}}}}};
  auto imported = LegacyPipelineImporter::Import(legacy);
  ASSERT_TRUE(imported.Ok()) << imported.error;
  auto  reference = CreateDefaultPipelineDocument();
  gpu_dag_test::EnsureTestCameraProfile(reference);
  gpu_dag_test::EnsureTestCameraProfile(*imported.document);
  auto* exposure  = dynamic_cast<ExposureModel*>(
      reference.PrimaryGrade()->FindAdjustmentByType(type_ids::Exposure()));
  ASSERT_NE(exposure, nullptr);
  exposure->SetValue(0.75f);

  const auto       imported_pixels = Render(*imported.document);
  CudaRenderDevice reference_device;
  const auto plan = GraphCompiler::Compile(reference, input_.CompileSource(), RenderRequest{});
  const auto reference_pixels =
      Download(reference_device, reference_device.Execute(plan, input_, reference));
  ASSERT_EQ(imported_pixels.size(), reference_pixels.size());
  for (std::size_t i = 0; i < imported_pixels.size(); ++i) {
    EXPECT_NEAR(imported_pixels[i].r, reference_pixels[i].r, 1.0e-5f);
    EXPECT_NEAR(imported_pixels[i].g, reference_pixels[i].g, 1.0e-5f);
    EXPECT_NEAR(imported_pixels[i].b, reference_pixels[i].b, 1.0e-5f);
  }
}

TEST_F(CudaDrtProductFixture, CudaBackendFailureDoesNotEnterCpuImageProcessing) {
  auto        document = CreateDefaultPipelineDocument();
  gpu_dag_test::EnsureTestCameraProfile(document);
  std::string reported;
  device_.SetErrorReporter([&reported](std::string_view message) { reported = message; });
  device_.Workspace().Device().FailNextUpload();
  EXPECT_THROW(
      {
        const auto plan = GraphCompiler::Compile(document, input_.CompileSource(), RenderRequest{});
        (void)device_.Execute(plan, input_, document);
      },
      std::runtime_error);
  EXPECT_FALSE(reported.empty());
  EXPECT_FALSE(device_.Workspace().IsRendering());
}

TEST_F(CudaDrtProductFixture, CudaDefaultPipelineSecondRenderCreatesNoGpuAllocation) {
  auto document = CreateDefaultPipelineDocument();
  Render(document);
  device_.Workspace().Device().ResetCounters();
  Render(document);
  EXPECT_EQ(device_.Workspace().Device().MallocCount(), 0U);
  EXPECT_EQ(device_.Workspace().Device().FreeCount(), 0U);
}

}  // namespace
}  // namespace alcedo
