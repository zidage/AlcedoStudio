//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <cuda_runtime.h>
#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <memory>
#include <opencv2/core.hpp>
#include <span>
#include <stdexcept>
#include <vector>

#include "../graph/test_camera_profile.hpp"
#include "../input/prepared_raw_test_support.hpp"
#include "edit/graph/analytic_mask_node_model.hpp"
#include "edit/graph/legacy_pipeline_importer.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/input/raw_input_loader.hpp"
#include "edit/operators/models/scalar_operator_model.hpp"
#include "edit/runtime/cuda/cuda_product_renderer.hpp"
#include "edit/runtime/cuda/cuda_render_device.hpp"
#include "edit/runtime/graph_compiler.hpp"
#include "edit/runtime/result_content_key.hpp"
#include "edit/runtime/texture_format.hpp"
#include "image/image_buffer.hpp"
#include "json.hpp"

namespace alcedo {
namespace {

auto HasCudaDevice() -> bool {
  int count = 0;
  return ::cudaGetDeviceCount(&count) == cudaSuccess && count > 0;
}

auto MakeEncodedImage(std::uint8_t tag) -> std::shared_ptr<ImageBuffer> {
  std::vector<std::uint8_t> bytes(64, tag);
  bytes[0] = tag;
  bytes[1] = 0x5A;
  return std::make_shared<ImageBuffer>(std::move(bytes));
}

auto MakeUnpacker() -> PreparedSourceCache::UnpackFn {
  return [](std::span<const std::byte>, DecodeRes decode_res) {
    const auto pattern = gpu_dag_test::MakeRggbPattern();
    return RawInputLoader::FromUnpackedCfa(gpu_dag_test::MakeU16CfaPlane(32, 32, pattern), pattern,
                                           gpu_dag_test::DefaultLinearization(),
                                           gpu_dag_test::FullSensor(32, 32), decode_res);
  };
}

auto RenderHost(CudaProductRenderer& renderer, const std::shared_ptr<ImageBuffer>& input,
                DecodeRes decode_res, const RenderRequest& request)
    -> std::shared_ptr<ImageBuffer> {
  return renderer.Render(input, decode_res, request, nullptr, FrameCompletionSubmission{}, true);
}

auto RenderHostWithoutSessionCache(CudaProductRenderer&                renderer,
                                   const std::shared_ptr<ImageBuffer>& input, DecodeRes decode_res,
                                   const RenderRequest& request) -> std::shared_ptr<ImageBuffer> {
  return renderer.Render(input, decode_res, request, nullptr, FrameCompletionSubmission{}, true,
                         CudaProductCachePolicy::BypassSessionCache);
}

auto OutputIsFinite(const std::shared_ptr<ImageBuffer>& image) -> bool {
  if (!image || !image->cpu_data_valid_) {
    return false;
  }
  const auto& mat = image->GetCPUData();
  if (mat.empty() || mat.type() != CV_32FC4) {
    return false;
  }
  for (int row = 0; row < mat.rows; ++row) {
    const auto* pixels = mat.ptr<cv::Vec4f>(row);
    for (int col = 0; col < mat.cols; ++col) {
      for (int channel = 0; channel < 4; ++channel) {
        if (!std::isfinite(pixels[col][channel])) {
          return false;
        }
      }
    }
  }
  return true;
}

class CudaResultCacheProductFixture : public ::testing::Test {
 protected:
  void SetUp() override {
    if (!HasCudaDevice()) {
      GTEST_SKIP() << "No CUDA device available.";
    }
    document_ = std::make_shared<PipelineDocument>(CreateDefaultPipelineDocument());
    gpu_dag_test::EnsureTestCameraProfile(*document_);
    renderer_ = std::make_unique<CudaProductRenderer>(document_, MakeUnpacker());
    image_    = MakeEncodedImage(71);
  }

  auto Render(const RenderRequest& request = {}) -> std::shared_ptr<ImageBuffer> {
    return RenderHost(*renderer_, image_, DecodeRes::FULL, request);
  }

  auto Exposure() -> ExposureModel* {
    return dynamic_cast<ExposureModel*>(
        document_->PrimaryGrade()->FindAdjustmentByType(type_ids::Exposure()));
  }

  std::shared_ptr<PipelineDocument>    document_;
  std::unique_ptr<CudaProductRenderer> renderer_;
  std::shared_ptr<ImageBuffer>         image_;
};

TEST_F(CudaResultCacheProductFixture,
       SecondUnchangedProductRenderRunsNoLibRawNoSourceUploadAndNoGpuNodePass) {
  ASSERT_TRUE(OutputIsFinite(Render()));
  renderer_->ResetStats();
  ASSERT_TRUE(OutputIsFinite(Render()));
  const auto stats = renderer_->Stats();
  EXPECT_EQ(stats.libraw_open_unpack_count, 0U);
  EXPECT_EQ(stats.plan_compile_count, 0U);
  EXPECT_EQ(stats.pass.source_h2d_count, 0U);
  EXPECT_EQ(stats.pass.source_h2d_bytes, 0U);
  EXPECT_EQ(stats.pass.sensor_develop_execute, 0U);
  EXPECT_EQ(stats.pass.geometry_execute, 0U);
  EXPECT_EQ(stats.pass.camera_color_execute, 0U);
  EXPECT_EQ(stats.pass.primary_grade_execute, 0U);
  EXPECT_EQ(stats.pass.drt_execute, 0U);
  EXPECT_EQ(stats.pass.sensor_develop_skip, 1U);
  EXPECT_EQ(stats.pass.geometry_skip, 1U);
  EXPECT_EQ(stats.pass.camera_color_skip, 1U);
  EXPECT_EQ(stats.pass.primary_grade_skip, 1U);
  EXPECT_EQ(stats.pass.drt_skip, 1U);
}

TEST_F(CudaResultCacheProductFixture, ExposureEditRunsOnlyPrimaryGradeAndDrtPasses) {
  ASSERT_TRUE(OutputIsFinite(Render()));
  renderer_->ResetStats();
  auto* exposure = Exposure();
  ASSERT_NE(exposure, nullptr);
  exposure->SetValue(0.75f);
  ASSERT_TRUE(OutputIsFinite(Render()));
  const auto stats = renderer_->Stats();
  EXPECT_EQ(stats.libraw_open_unpack_count, 0U);
  EXPECT_EQ(stats.pass.source_h2d_count, 0U);
  EXPECT_EQ(stats.pass.sensor_develop_execute, 0U);
  EXPECT_EQ(stats.pass.geometry_execute, 0U);
  EXPECT_EQ(stats.pass.camera_color_execute, 0U);
  EXPECT_EQ(stats.pass.primary_grade_execute, 1U);
  EXPECT_EQ(stats.pass.drt_execute, 1U);
  EXPECT_EQ(stats.pass.sensor_develop_skip, 1U);
  EXPECT_EQ(stats.pass.geometry_skip, 1U);
  EXPECT_EQ(stats.pass.camera_color_skip, 1U);
}

TEST_F(CudaResultCacheProductFixture,
       TemporaryOvalSecondUnchangedRenderSkipsSensorGeometryCameraMaskGradeAndDrt) {
  AttachTemporaryPrimaryGradeOvalMask(*document_);
  ASSERT_TRUE(OutputIsFinite(Render()));
  renderer_->ResetStats();
  ASSERT_TRUE(OutputIsFinite(Render()));
  const auto stats = renderer_->Stats();
  EXPECT_EQ(stats.pass.sensor_develop_execute, 0U);
  EXPECT_EQ(stats.pass.geometry_execute, 0U);
  EXPECT_EQ(stats.pass.camera_color_execute, 0U);
  EXPECT_EQ(stats.pass.mask_execute, 0U);
  EXPECT_EQ(stats.pass.primary_grade_execute, 0U);
  EXPECT_EQ(stats.pass.drt_execute, 0U);
  EXPECT_EQ(stats.pass.sensor_develop_skip, 1U);
  EXPECT_EQ(stats.pass.geometry_skip, 1U);
  EXPECT_EQ(stats.pass.camera_color_skip, 1U);
  EXPECT_EQ(stats.pass.mask_skip, 1U);
  EXPECT_EQ(stats.pass.primary_grade_skip, 1U);
  EXPECT_EQ(stats.pass.drt_skip, 1U);
}

TEST_F(CudaResultCacheProductFixture,
       ApplyOntoExposureWithTemporaryOvalReusesSensorGeometryCameraAndMask) {
  AttachTemporaryPrimaryGradeOvalMask(*document_);
  ASSERT_TRUE(OutputIsFinite(Render()));
  renderer_->ResetStats();
  nlohmann::json json;
  json["Basic Adjustment"]["Basic Adjustment"]["exposure"] = {
      {"type", 2}, {"enable", true}, {"params", {{"exposure", 0.75}}}};
  ASSERT_TRUE(LegacyPipelineImporter::ApplyOnto(*document_, json).empty());
  ASSERT_TRUE(OutputIsFinite(Render()));
  const auto stats = renderer_->Stats();
  EXPECT_EQ(stats.pass.source_h2d_count, 0U);
  EXPECT_EQ(stats.pass.sensor_develop_execute, 0U);
  EXPECT_EQ(stats.pass.geometry_execute, 0U);
  EXPECT_EQ(stats.pass.camera_color_execute, 0U);
  EXPECT_EQ(stats.pass.mask_execute, 0U);
  EXPECT_EQ(stats.pass.primary_grade_execute, 1U);
  EXPECT_EQ(stats.pass.drt_execute, 1U);
  EXPECT_EQ(stats.pass.sensor_develop_skip, 1U);
  EXPECT_EQ(stats.pass.geometry_skip, 1U);
  EXPECT_EQ(stats.pass.camera_color_skip, 1U);
  EXPECT_EQ(stats.pass.mask_skip, 1U);
}

TEST_F(CudaResultCacheProductFixture,
       DevelopCctEditReusesSensorAndGeometryAndRunsCameraColorGradeDrt) {
  ASSERT_TRUE(OutputIsFinite(Render()));
  renderer_->ResetStats();
  auto develop       = document_->Develop()->Params().Params();
  develop.wb_mode    = "custom";
  develop.custom_cct = 4800.0f;
  document_->Develop()->Params().ReplaceParams(develop);
  ASSERT_TRUE(OutputIsFinite(Render()));
  const auto stats = renderer_->Stats();
  EXPECT_EQ(stats.libraw_open_unpack_count, 0U);
  EXPECT_EQ(stats.pass.source_h2d_count, 0U);
  EXPECT_EQ(stats.pass.sensor_develop_execute, 0U);
  EXPECT_EQ(stats.pass.geometry_execute, 0U);
  EXPECT_EQ(stats.pass.camera_color_execute, 1U);
  EXPECT_EQ(stats.pass.primary_grade_execute, 1U);
  EXPECT_EQ(stats.pass.drt_execute, 1U);
  EXPECT_EQ(stats.pass.sensor_develop_skip, 1U);
  EXPECT_EQ(stats.pass.geometry_skip, 1U);
}

TEST_F(CudaResultCacheProductFixture, DrtEditRunsOnlyDrtPass) {
  ASSERT_TRUE(OutputIsFinite(Render()));
  renderer_->ResetStats();
  auto drt           = document_->Drt()->Params().Params();
  drt.peak_luminance = 180.0f;
  document_->Drt()->Params().ReplaceParams(drt);
  ASSERT_TRUE(OutputIsFinite(Render()));
  const auto stats = renderer_->Stats();
  EXPECT_EQ(stats.pass.source_h2d_count, 0U);
  EXPECT_EQ(stats.pass.sensor_develop_execute, 0U);
  EXPECT_EQ(stats.pass.geometry_execute, 0U);
  EXPECT_EQ(stats.pass.camera_color_execute, 0U);
  EXPECT_EQ(stats.pass.primary_grade_execute, 0U);
  EXPECT_EQ(stats.pass.drt_execute, 1U);
  EXPECT_EQ(stats.pass.drt_skip, 0U);
  EXPECT_EQ(stats.pass.primary_grade_skip, 1U);
}

TEST_F(CudaResultCacheProductFixture,
       ViewportChangeReusesSensorDevelopAndRunsGeometryAndDownstream) {
  ASSERT_TRUE(OutputIsFinite(Render()));
  renderer_->ResetStats();
  RenderRequest request;
  request.view.visible_rect_in_edit_space = {0.1f, 0.1f, 0.8f, 0.8f};
  request.view.viewport_extent            = {24, 16};
  ASSERT_TRUE(OutputIsFinite(Render(request)));
  const auto stats = renderer_->Stats();
  EXPECT_EQ(stats.pass.source_h2d_count, 0U);
  EXPECT_EQ(stats.pass.sensor_develop_execute, 0U);
  EXPECT_EQ(stats.pass.geometry_execute, 1U);
  EXPECT_EQ(stats.pass.camera_color_execute, 1U);
  EXPECT_EQ(stats.pass.primary_grade_execute, 1U);
  EXPECT_EQ(stats.pass.drt_execute, 1U);
  EXPECT_EQ(stats.pass.sensor_develop_skip, 1U);
}

TEST_F(CudaResultCacheProductFixture,
       GeometryEditReusesSensorDevelopAndInvalidatesPostGeometryResult) {
  ASSERT_TRUE(OutputIsFinite(Render()));
  renderer_->ResetStats();
  document_->Geometry().SetCropRect({0.05f, 0.05f, 0.9f, 0.9f});
  ASSERT_TRUE(OutputIsFinite(Render()));
  const auto stats = renderer_->Stats();
  EXPECT_EQ(stats.pass.source_h2d_count, 0U);
  EXPECT_EQ(stats.pass.sensor_develop_execute, 0U);
  EXPECT_EQ(stats.pass.geometry_execute, 1U);
  EXPECT_EQ(stats.pass.camera_color_execute, 1U);
  EXPECT_EQ(stats.pass.primary_grade_execute, 1U);
  EXPECT_EQ(stats.pass.drt_execute, 1U);
  EXPECT_EQ(stats.pass.sensor_develop_skip, 1U);
  EXPECT_EQ(stats.pass.geometry_skip, 0U);
}

TEST_F(CudaResultCacheProductFixture,
       RawDevelopEditInvalidatesSensorDevelopAndAllDownstreamResults) {
  ASSERT_TRUE(OutputIsFinite(Render()));
  renderer_->ResetStats();
  auto develop                   = document_->Develop()->Params().Params();
  develop.highlights_reconstruct = !develop.highlights_reconstruct;
  document_->Develop()->Params().ReplaceParams(develop);
  ASSERT_TRUE(OutputIsFinite(Render()));
  const auto stats = renderer_->Stats();
  EXPECT_EQ(stats.pass.source_h2d_count, 1U);
  EXPECT_GT(stats.pass.source_h2d_bytes, 0U);
  EXPECT_EQ(stats.pass.sensor_develop_execute, 1U);
  EXPECT_EQ(stats.pass.geometry_execute, 1U);
  EXPECT_EQ(stats.pass.camera_color_execute, 1U);
  EXPECT_EQ(stats.pass.primary_grade_execute, 1U);
  EXPECT_EQ(stats.pass.drt_execute, 1U);
  EXPECT_EQ(stats.pass.sensor_develop_skip, 0U);
}

TEST_F(CudaResultCacheProductFixture, ImageSwitchBackAfterReleaseSessionCachesMissesAndReexecutes) {
  ASSERT_TRUE(OutputIsFinite(Render()));
  renderer_->ReleaseSessionCaches();
  const auto released = renderer_->SessionResources();
  EXPECT_EQ(released.published_result_count, 0U);
  EXPECT_EQ(released.texture_pool_entry_count, 0U);
  EXPECT_EQ(released.prepared_source_entry_count, 0U);
  renderer_->ResetStats();
  ASSERT_TRUE(OutputIsFinite(Render()));
  const auto stats = renderer_->Stats();
  EXPECT_EQ(stats.prepared_source_misses, 1U);
  EXPECT_EQ(stats.pass.sensor_develop_execute, 1U);
  EXPECT_EQ(stats.pass.drt_execute, 1U);
  EXPECT_EQ(stats.pass.sensor_develop_skip, 0U);
}

TEST_F(CudaResultCacheProductFixture,
       ClearAllIntermediateBuffersReleasesCudaProductSessionGpuAndHostCaches) {
  ASSERT_TRUE(OutputIsFinite(Render()));
  EXPECT_GT(renderer_->SessionResources().texture_pool_used_bytes, 0U);
  EXPECT_GT(renderer_->SessionResources().published_result_count, 0U);
  EXPECT_GT(renderer_->SessionResources().prepared_source_host_bytes, 0U);
  renderer_->ReleaseSessionCaches();
  const auto resources = renderer_->SessionResources();
  EXPECT_EQ(resources.texture_pool_used_bytes, 0U);
  EXPECT_EQ(resources.texture_pool_entry_count, 0U);
  EXPECT_EQ(resources.published_result_count, 0U);
  EXPECT_EQ(resources.prepared_source_host_bytes, 0U);
  EXPECT_EQ(resources.prepared_source_entry_count, 0U);
  EXPECT_TRUE(resources.session_value_ids.empty());
}

TEST_F(CudaResultCacheProductFixture, TexturePoolBudgetNoLongerHardCodedTo64MiBOnProductPath) {
  EXPECT_GT(renderer_->Device().Workspace().Textures().ByteBudget(), 64ull << 20);
}

TEST_F(CudaResultCacheProductFixture,
       ViewportChangeAfterSessionReleaseStillReusesSensorLinearOnTheLivePipeline) {
  ASSERT_TRUE(OutputIsFinite(Render()));
  renderer_->ResetStats();
  RenderRequest request;
  request.view.visible_rect_in_edit_space = {0.1f, 0.1f, 0.8f, 0.8f};
  request.view.viewport_extent            = {24, 16};
  ASSERT_TRUE(OutputIsFinite(Render(request)));
  const auto stats = renderer_->Stats();
  EXPECT_EQ(stats.pass.sensor_develop_execute, 0U);
  EXPECT_EQ(stats.pass.sensor_develop_skip, 1U);
  EXPECT_EQ(stats.pass.geometry_execute, 1U);
}

TEST_F(CudaResultCacheProductFixture, ThreeSequentialImagePinsDoNotRetainPreviousImageGpuTextures) {
  auto document_a = std::make_shared<PipelineDocument>(CreateDefaultPipelineDocument());
  auto document_b = std::make_shared<PipelineDocument>(CreateDefaultPipelineDocument());
  auto document_c = std::make_shared<PipelineDocument>(CreateDefaultPipelineDocument());
  gpu_dag_test::EnsureTestCameraProfile(*document_a);
  gpu_dag_test::EnsureTestCameraProfile(*document_b);
  gpu_dag_test::EnsureTestCameraProfile(*document_c);
  CudaProductRenderer session_a(document_a, MakeUnpacker());
  CudaProductRenderer session_b(document_b, MakeUnpacker());
  CudaProductRenderer session_c(document_c, MakeUnpacker());
  ASSERT_TRUE(OutputIsFinite(RenderHost(session_a, MakeEncodedImage(11), DecodeRes::FULL, {})));
  EXPECT_GT(session_a.SessionResources().texture_pool_used_bytes, 0U);
  session_a.ReleaseSessionCaches();
  ASSERT_TRUE(OutputIsFinite(RenderHost(session_b, MakeEncodedImage(12), DecodeRes::FULL, {})));
  EXPECT_EQ(session_a.SessionResources().texture_pool_used_bytes, 0U);
  EXPECT_GT(session_b.SessionResources().texture_pool_used_bytes, 0U);
  session_b.ReleaseSessionCaches();
  ASSERT_TRUE(OutputIsFinite(RenderHost(session_c, MakeEncodedImage(13), DecodeRes::FULL, {})));
  EXPECT_EQ(session_a.SessionResources().texture_pool_used_bytes, 0U);
  EXPECT_EQ(session_b.SessionResources().texture_pool_used_bytes, 0U);
  EXPECT_GT(session_c.SessionResources().texture_pool_used_bytes, 0U);
}

TEST_F(CudaResultCacheProductFixture, ImageSwitchBackReusesMatchingPreparedSourceAndGpuResults) {
  const auto image_a = MakeEncodedImage(81);
  const auto image_b = MakeEncodedImage(82);
  ASSERT_TRUE(OutputIsFinite(RenderHost(*renderer_, image_a, DecodeRes::FULL, {})));
  ASSERT_TRUE(OutputIsFinite(RenderHost(*renderer_, image_b, DecodeRes::FULL, {})));
  renderer_->ResetStats();
  ASSERT_TRUE(OutputIsFinite(RenderHost(*renderer_, image_a, DecodeRes::FULL, {})));
  const auto stats = renderer_->Stats();
  EXPECT_EQ(stats.libraw_open_unpack_count, 0U);
  EXPECT_EQ(stats.prepared_source_hits, 1U);
  EXPECT_EQ(stats.pass.source_h2d_count, 0U);
  EXPECT_EQ(stats.pass.sensor_develop_execute, 0U);
  EXPECT_EQ(stats.pass.geometry_execute, 0U);
  EXPECT_EQ(stats.pass.camera_color_execute, 0U);
  EXPECT_EQ(stats.pass.primary_grade_execute, 0U);
  EXPECT_EQ(stats.pass.drt_execute, 0U);
  EXPECT_EQ(stats.pass.sensor_develop_skip, 1U);
  EXPECT_EQ(stats.pass.drt_skip, 1U);
}

TEST_F(CudaResultCacheProductFixture, OneShotRenderDoesNotReadWriteOrClearEditorSessionCaches) {
  ASSERT_TRUE(OutputIsFinite(Render()));
  const auto resources_before = renderer_->SessionResources();
  renderer_->ResetStats();

  ASSERT_TRUE(
      OutputIsFinite(RenderHostWithoutSessionCache(*renderer_, image_, DecodeRes::FULL, {})));

  const auto after_one_shot = renderer_->Stats();
  EXPECT_EQ(after_one_shot.prepared_source_hits, 0U);
  EXPECT_EQ(after_one_shot.prepared_source_misses, 0U);
  EXPECT_EQ(after_one_shot.plan_cache_hits, 0U);
  EXPECT_EQ(after_one_shot.plan_cache_misses, 0U);
  EXPECT_EQ(after_one_shot.pass.sensor_develop_execute, 0U);
  EXPECT_EQ(renderer_->SessionResources().published_result_count,
            resources_before.published_result_count);
  EXPECT_EQ(renderer_->SessionResources().prepared_source_entry_count,
            resources_before.prepared_source_entry_count);

  ASSERT_TRUE(OutputIsFinite(Render()));
  const auto after_preview = renderer_->Stats();
  EXPECT_EQ(after_preview.prepared_source_hits, 1U);
  EXPECT_EQ(after_preview.libraw_open_unpack_count, 0U);
  EXPECT_EQ(after_preview.pass.sensor_develop_execute, 0U);
  EXPECT_EQ(after_preview.pass.drt_execute, 0U);
  EXPECT_EQ(after_preview.pass.sensor_develop_skip, 1U);
  EXPECT_EQ(after_preview.pass.drt_skip, 1U);
}

class CudaResultCacheDeviceFixture : public ::testing::Test {
 protected:
  void SetUp() override {
    if (!HasCudaDevice()) {
      GTEST_SKIP() << "No CUDA device available.";
    }
    input_    = RawInputLoader::FromDirectRgb(gpu_dag_test::MakeF32RgbaPlane(16, 12),
                                              gpu_dag_test::FullSensor(16, 12));
    document_ = CreateDefaultPipelineDocument();
    gpu_dag_test::EnsureTestCameraProfile(document_);
  }

  auto Compile(const RenderRequest& request = {}) -> ExecutionPlan {
    return GraphCompiler::Compile(document_, input_.CompileSource(), request);
  }

  PreparedRawInput input_;
  PipelineDocument document_;
  CudaRenderDevice device_;
};

TEST_F(CudaResultCacheDeviceFixture, FailedSubmissionDoesNotPublishResultContentKey) {
  auto       plan       = Compile();
  const auto first_keys = BuildFrameResultContentKeys(plan, input_, document_);
  ASSERT_EQ(device_.Execute(plan, input_, document_), plan.display_output);
  auto develop                   = document_.Develop()->Params().Params();
  develop.highlights_reconstruct = !develop.highlights_reconstruct;
  document_.Develop()->Params().ReplaceParams(develop);
  const auto second_keys = BuildFrameResultContentKeys(plan, input_, document_);
  ASSERT_NE(second_keys.sensor_linear, first_keys.sensor_linear);
  device_.Workspace().Device().FailNextUpload();
  EXPECT_THROW((void)device_.Execute(plan, input_, document_), std::runtime_error);
  device_.WaitIdle();
  auto&      images    = device_.Workspace().Images();
  const auto completed = device_.Workspace().Device().CompletedSubmission();
  EXPECT_TRUE(images.FindValidResult(plan.sensor_linear_output, first_keys.sensor_linear,
                                     first_keys.sensor_extent, TextureFormat::Rgba32f, completed));
  EXPECT_TRUE(images.FindValidResult(plan.display_output, first_keys.drt_display,
                                     first_keys.geometry_extent, TextureFormat::Rgba32f,
                                     completed));
  EXPECT_FALSE(images.FindValidResult(plan.sensor_linear_output, second_keys.sensor_linear,
                                      second_keys.sensor_extent, TextureFormat::Rgba32f,
                                      completed));
  EXPECT_FALSE(images.FindValidResult(plan.display_output, second_keys.drt_display,
                                      second_keys.geometry_extent, TextureFormat::Rgba32f,
                                      completed));
}

TEST_F(CudaResultCacheDeviceFixture,
       CancelledSubmissionKeepsPreviouslyCompletedCacheEntriesUsable) {
  auto       plan = Compile();
  const auto keys = BuildFrameResultContentKeys(plan, input_, document_);
  ASSERT_EQ(device_.Execute(plan, input_, document_), plan.display_output);
  device_.BeginRender();
  (void)device_.Workspace().AcquireImageForWrite(
      plan.display_output,
      {keys.geometry_extent.width, keys.geometry_extent.height, TextureFormat::Rgba32f});
  device_.CancelRender();
  device_.WaitIdle();
  const auto completed = device_.Workspace().Device().CompletedSubmission();
  EXPECT_TRUE(device_.Workspace().Images().FindValidResult(plan.display_output, keys.drt_display,
                                                           keys.geometry_extent,
                                                           TextureFormat::Rgba32f, completed));
  ASSERT_EQ(device_.Execute(plan, input_, document_), plan.display_output);
  EXPECT_EQ(device_.PassStats().drt_skip, 1U);
}

}  // namespace
}  // namespace alcedo
