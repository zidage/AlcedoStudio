//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <cuda_runtime.h>
#include <gtest/gtest.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <future>
#include <memory>
#include <opencv2/core.hpp>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

#include "../graph/grade_owned_mask_support.hpp"
#include "../graph/test_camera_profile.hpp"
#include "../input/prepared_raw_test_support.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/graph/pipeline_graph_commands.hpp"
#include "edit/input/raw_input_loader.hpp"
#include "edit/runtime/pipeline_apply_request.hpp"
#include "edit/operators/models/scalar_operator_model.hpp"
#include "edit/runtime/cuda/cuda_product_renderer.hpp"
#include "edit/runtime/cuda/cuda_render_device.hpp"
#include "edit/runtime/graph_compiler.hpp"
#include "edit/runtime/renderer.hpp"
#include "edit/runtime/result_content_key.hpp"
#include "edit/runtime/result_persistence.hpp"
#include "edit/runtime/texture_format.hpp"
#include "image/image_buffer.hpp"
#include "json.hpp"
#include "multi_grade_runtime_test_support.hpp"
#include "support/render_snapshot_source.hpp"
#include "ui/edit_viewer/frame_sink.hpp"

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

void ConnectFullCoverageMask(PipelineDocument& document) {
  LinearGradientMaskSource flat;
  flat.start_value         = 1.0f;
  flat.end_value           = 1.0f;
  flat.transition_distance = 1.0f;
  grade_mask_test::AddLinearGradientMask(document, MaskId{"mask.full"}, flat);
}

/// Host render of @p snapshot; the renderer's role decides interactive or batch behavior.
auto RenderHost(CudaProductRenderer& renderer, const PipelineGraphSnapshot& snapshot,
                const std::shared_ptr<ImageBuffer>& input, DecodeRes decode_res,
                const RenderRequest& request) -> std::shared_ptr<ImageBuffer> {
  return renderer.Render(snapshot, input, decode_res, request, nullptr, FrameCompletionSubmission{},
                         true);
}

auto CompareHostRgba(const std::shared_ptr<ImageBuffer>& left,
                     const std::shared_ptr<ImageBuffer>& right, float max_abs)
    -> bool {
  if (!left || !right || !left->cpu_data_valid_ || !right->cpu_data_valid_) {
    return false;
  }
  const auto& a = left->GetCPUData();
  const auto& b = right->GetCPUData();
  if (a.empty() || b.empty() || a.size() != b.size() || a.type() != CV_32FC4 ||
      b.type() != CV_32FC4) {
    return false;
  }
  for (int row = 0; row < a.rows; ++row) {
    const auto* pa = a.ptr<cv::Vec4f>(row);
    const auto* pb = b.ptr<cv::Vec4f>(row);
    for (int col = 0; col < a.cols; ++col) {
      for (int channel = 0; channel < 3; ++channel) {
        if (!std::isfinite(pa[col][channel]) || !std::isfinite(pb[col][channel])) {
          return false;
        }
        if (std::abs(pa[col][channel] - pb[col][channel]) > max_abs) {
          return false;
        }
      }
    }
  }
  return true;
}

class RejectingPresentSink final : public IFrameSink {
 public:
  void EnsureSize(int, int) override {}
  auto MapResourceForWrite(FrameMemoryDomain) -> FrameWriteMapping override { return {}; }
  void UnmapResource() override {}
  void NotifyFrameReady(const FrameCompletionSubmission&) override {}
  auto GetWidth() const -> int override { return 0; }
  auto GetHeight() const -> int override { return 0; }
};

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
    source_   = std::make_unique<test::RenderSnapshotSource>(document_);
    renderer_ = std::make_unique<CudaProductRenderer>(ExecutorRole::Interactive, MakeUnpacker());
    batch_renderer_ = std::make_unique<CudaProductRenderer>(ExecutorRole::Batch, MakeUnpacker());
    image_          = MakeEncodedImage(71);
  }

  auto Render(const RenderRequest& request = {}) -> std::shared_ptr<ImageBuffer> {
    return RenderHost(*renderer_, *source_->Freeze(), image_, DecodeRes::FULL, request);
  }

  /// Batch render of the same snapshot the interactive renderer would receive now.
  auto RenderBatch(const RenderRequest& request = {}) -> std::shared_ptr<ImageBuffer> {
    return RenderHost(*batch_renderer_, *source_->Freeze(), image_, DecodeRes::FULL, request);
  }

  auto RenderRole(FrameRole role, std::uint32_t max_edge) -> std::shared_ptr<ImageBuffer> {
    RenderRequest request;
    request.resolution.max_edge = max_edge;
    FrameCompletionSubmission submission;
    submission.metadata.frame_role = role;
    return renderer_->Render(*source_->Freeze(), image_, DecodeRes::FULL, request, nullptr,
                             submission, true);
  }

  auto GeometryId() const -> GraphValueId {
    return {NodeId{"geometry"}, PortId{"scene_source"}};
  }
  auto SensorId() const -> GraphValueId {
    return {NodeId{"develop"}, PortId{"sensor_linear"}};
  }

  auto Exposure() -> ExposureModel* {
    return dynamic_cast<ExposureModel*>(
        document_->PrimaryGrade()->FindAdjustmentByType(type_ids::Exposure()));
  }

  std::shared_ptr<PipelineDocument>           document_;
  std::unique_ptr<test::RenderSnapshotSource> source_;
  std::unique_ptr<CudaProductRenderer>        renderer_;
  std::unique_ptr<CudaProductRenderer>        batch_renderer_;
  std::shared_ptr<ImageBuffer>                image_;
};

TEST_F(CudaResultCacheProductFixture,
       SecondUnchangedProductRenderSkipsKeyStagesAndDisplay) {
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
  EXPECT_EQ(stats.pass.primary_grade_execute, 1U);
  EXPECT_EQ(stats.pass.primary_grade_skip, 0U);
  EXPECT_EQ(stats.pass.drt_execute, 0U);
  EXPECT_EQ(stats.pass.sensor_develop_skip, 1U);
  EXPECT_EQ(stats.pass.geometry_skip, 1U);
  EXPECT_EQ(stats.pass.camera_color_skip, 1U);
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
       MaskSecondUnchangedRenderSkipsKeyStagesMaskAndDisplay) {
  ConnectFullCoverageMask(*document_);
  ASSERT_TRUE(OutputIsFinite(Render()));
  renderer_->ResetStats();
  ASSERT_TRUE(OutputIsFinite(Render()));
  const auto stats = renderer_->Stats();
  EXPECT_EQ(stats.pass.sensor_develop_execute, 0U);
  EXPECT_EQ(stats.pass.geometry_execute, 0U);
  EXPECT_EQ(stats.pass.camera_color_execute, 0U);
  EXPECT_EQ(stats.pass.mask_execute, 0U);
  EXPECT_EQ(stats.pass.primary_grade_execute, 1U);
  EXPECT_EQ(stats.pass.primary_grade_skip, 0U);
  EXPECT_EQ(stats.pass.drt_execute, 0U);
  EXPECT_EQ(stats.pass.sensor_develop_skip, 1U);
  EXPECT_EQ(stats.pass.geometry_skip, 1U);
  EXPECT_EQ(stats.pass.camera_color_skip, 1U);
  EXPECT_EQ(stats.pass.mask_skip, 1U);
  EXPECT_EQ(stats.pass.drt_skip, 1U);
}

TEST_F(CudaResultCacheProductFixture, ExposureEditWithMaskReusesSensorGeometryCameraAndMask) {
  ConnectFullCoverageMask(*document_);
  ASSERT_TRUE(OutputIsFinite(Render()));
  renderer_->ResetStats();
  auto* exposure = dynamic_cast<ExposureModel*>(
      document_->PrimaryGrade()->FindAdjustmentByType(type_ids::Exposure()));
  ASSERT_NE(exposure, nullptr);
  exposure->SetValue(0.75f);
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

TEST_F(CudaResultCacheProductFixture, DrtEditReexecutesGradesAndRunsDisplayTransform) {
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
  EXPECT_EQ(stats.pass.primary_grade_execute, 1U);
  EXPECT_EQ(stats.pass.primary_grade_skip, 0U);
  EXPECT_EQ(stats.pass.drt_execute, 1U);
  EXPECT_EQ(stats.pass.drt_skip, 0U);
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

TEST_F(CudaResultCacheProductFixture, ImageSwitchBackAfterReleaseBindingMissesAndReexecutes) {
  ASSERT_TRUE(OutputIsFinite(Render()));
  renderer_->ReleaseBinding();
  EXPECT_FALSE(renderer_->Binding().has_value());
  const auto released = renderer_->Resources();
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

TEST_F(CudaResultCacheProductFixture, RebindToNewLineageReleasesPreviousBindingBeforeRendering) {
  ASSERT_TRUE(OutputIsFinite(Render()));
  const auto device_before = renderer_->DebugDeviceIdentity();
  const auto first_binding = renderer_->Binding();
  ASSERT_TRUE(first_binding.has_value());
  EXPECT_EQ(first_binding->lineage, source_->Lineage());

  // Same document content in a new lineage models a reload of the image history.
  auto reloaded = std::make_shared<PipelineDocument>(CreateDefaultPipelineDocument());
  gpu_dag_test::EnsureTestCameraProfile(*reloaded);
  source_->Rebind(reloaded);
  renderer_->ResetStats();
  ASSERT_TRUE(OutputIsFinite(Render()));
  const auto stats = renderer_->Stats();
  EXPECT_EQ(stats.prepared_source_hits, 0U);
  EXPECT_EQ(stats.prepared_source_misses, 1U);
  EXPECT_EQ(stats.plan_compile_count, 1U);
  EXPECT_EQ(stats.pass.sensor_develop_execute, 1U);
  EXPECT_EQ(stats.pass.drt_execute, 1U);
  EXPECT_EQ(stats.pass.sensor_develop_skip, 0U);
  ASSERT_TRUE(renderer_->Binding().has_value());
  EXPECT_EQ(renderer_->Binding()->lineage, source_->Lineage());
  EXPECT_NE(renderer_->Binding()->lineage, first_binding->lineage);
  EXPECT_EQ(renderer_->DebugDeviceIdentity(), device_before);
}

TEST_F(CudaResultCacheProductFixture, ReleaseBindingReleasesCudaProductSessionGpuAndHostCaches) {
  ASSERT_TRUE(OutputIsFinite(Render()));
  EXPECT_GT(renderer_->Resources().texture_pool_used_bytes, 0U);
  EXPECT_GT(renderer_->Resources().published_result_count, 0U);
  EXPECT_GT(renderer_->Resources().prepared_source_host_bytes, 0U);
  renderer_->ReleaseBinding();
  EXPECT_FALSE(renderer_->Binding().has_value());
  const auto resources = renderer_->Resources();
  EXPECT_EQ(resources.texture_pool_used_bytes, 0U);
  EXPECT_EQ(resources.texture_pool_entry_count, 0U);
  EXPECT_EQ(resources.published_result_count, 0U);
  EXPECT_EQ(resources.prepared_source_host_bytes, 0U);
  EXPECT_EQ(resources.prepared_source_entry_count, 0U);
  EXPECT_TRUE(resources.session_value_ids.empty());
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
  const auto          snapshot_a = test::FreezeInNewLineage(*document_a);
  const auto          snapshot_b = test::FreezeInNewLineage(*document_b);
  const auto          snapshot_c = test::FreezeInNewLineage(*document_c);
  CudaProductRenderer session_a(ExecutorRole::Interactive, MakeUnpacker());
  CudaProductRenderer session_b(ExecutorRole::Interactive, MakeUnpacker());
  CudaProductRenderer session_c(ExecutorRole::Interactive, MakeUnpacker());
  ASSERT_TRUE(OutputIsFinite(
      RenderHost(session_a, *snapshot_a, MakeEncodedImage(11), DecodeRes::FULL, {})));
  EXPECT_GT(session_a.Resources().texture_pool_used_bytes, 0U);
  session_a.ReleaseBinding();
  ASSERT_TRUE(OutputIsFinite(
      RenderHost(session_b, *snapshot_b, MakeEncodedImage(12), DecodeRes::FULL, {})));
  EXPECT_EQ(session_a.Resources().texture_pool_used_bytes, 0U);
  EXPECT_GT(session_b.Resources().texture_pool_used_bytes, 0U);
  session_b.ReleaseBinding();
  ASSERT_TRUE(OutputIsFinite(
      RenderHost(session_c, *snapshot_c, MakeEncodedImage(13), DecodeRes::FULL, {})));
  EXPECT_EQ(session_a.Resources().texture_pool_used_bytes, 0U);
  EXPECT_EQ(session_b.Resources().texture_pool_used_bytes, 0U);
  EXPECT_GT(session_c.Resources().texture_pool_used_bytes, 0U);
}

TEST_F(CudaResultCacheProductFixture, ImageSwitchBackReusesMatchingPreparedSourceAndGpuResults) {
  const auto image_a = MakeEncodedImage(81);
  const auto image_b = MakeEncodedImage(82);
  ASSERT_TRUE(
      OutputIsFinite(RenderHost(*renderer_, *source_->Freeze(), image_a, DecodeRes::FULL, {})));
  ASSERT_TRUE(
      OutputIsFinite(RenderHost(*renderer_, *source_->Freeze(), image_b, DecodeRes::FULL, {})));
  renderer_->ResetStats();
  ASSERT_TRUE(
      OutputIsFinite(RenderHost(*renderer_, *source_->Freeze(), image_a, DecodeRes::FULL, {})));
  const auto stats = renderer_->Stats();
  EXPECT_EQ(stats.libraw_open_unpack_count, 0U);
  EXPECT_EQ(stats.prepared_source_hits, 1U);
  EXPECT_EQ(stats.pass.sensor_develop_execute, 1U);
  EXPECT_EQ(stats.pass.drt_execute, 1U);
  EXPECT_EQ(stats.pass.sensor_develop_skip, 0U);
  EXPECT_EQ(stats.pass.drt_skip, 0U);
}

TEST_F(CudaResultCacheProductFixture,
       BatchRenderOfSameSnapshotDoesNotReadWriteOrClearInteractiveRendererCaches) {
  ASSERT_TRUE(OutputIsFinite(Render()));
  const auto resources_before = renderer_->Resources();
  const auto binding_before   = renderer_->Binding();
  renderer_->ResetStats();

  ASSERT_TRUE(OutputIsFinite(RenderBatch()));

  const auto after_batch = renderer_->Stats();
  EXPECT_EQ(after_batch.prepared_source_hits, 0U);
  EXPECT_EQ(after_batch.prepared_source_misses, 0U);
  EXPECT_EQ(after_batch.plan_cache_hits, 0U);
  EXPECT_EQ(after_batch.plan_cache_misses, 0U);
  EXPECT_EQ(after_batch.pass.sensor_develop_execute, 0U);
  EXPECT_EQ(renderer_->Resources().published_result_count, resources_before.published_result_count);
  EXPECT_EQ(renderer_->Resources().prepared_source_entry_count,
            resources_before.prepared_source_entry_count);
  EXPECT_EQ(renderer_->Binding(), binding_before);
  const auto batch_stats = batch_renderer_->Stats();
  EXPECT_EQ(batch_stats.prepared_source_hits, 0U);
  EXPECT_EQ(batch_stats.plan_cache_hits, 0U);
  EXPECT_GE(batch_stats.pass.sensor_develop_execute, 1U);
  EXPECT_EQ(batch_renderer_->Resources().published_result_count, 0U);
  EXPECT_EQ(batch_renderer_->Resources().texture_pool_used_bytes, 0U);
  EXPECT_FALSE(batch_renderer_->Binding().has_value());

  ASSERT_TRUE(OutputIsFinite(Render()));
  const auto after_preview = renderer_->Stats();
  EXPECT_EQ(after_preview.prepared_source_hits, 1U);
  EXPECT_EQ(after_preview.libraw_open_unpack_count, 0U);
  EXPECT_EQ(after_preview.pass.sensor_develop_execute, 0U);
  EXPECT_EQ(after_preview.pass.drt_execute, 0U);
  EXPECT_EQ(after_preview.pass.sensor_develop_skip, 1U);
  EXPECT_EQ(after_preview.pass.drt_skip, 1U);
}

TEST_F(CudaResultCacheProductFixture, BackgroundMultiGradeRenderPreservesEditorCache) {
  multi_grade_test::AddCleanGradesBeforeDrt(*document_, {"grade.b", "grade.c"});
  ASSERT_TRUE(OutputIsFinite(Render()));
  const auto resources_before = renderer_->Resources();
  EXPECT_GT(resources_before.published_result_count, 0U);
  renderer_->ResetStats();

  ASSERT_TRUE(OutputIsFinite(RenderBatch()));

  EXPECT_EQ(renderer_->Resources().published_result_count, resources_before.published_result_count);
  EXPECT_EQ(renderer_->Resources().prepared_source_entry_count,
            resources_before.prepared_source_entry_count);
  EXPECT_EQ(batch_renderer_->Resources().published_result_count, 0U);
  EXPECT_EQ(batch_renderer_->Resources().texture_pool_used_bytes, 0U);

  ASSERT_TRUE(OutputIsFinite(Render()));
  EXPECT_EQ(renderer_->Stats().pass.sensor_develop_skip, 1U);
  EXPECT_EQ(renderer_->Stats().pass.drt_skip, 1U);
}

TEST_F(CudaResultCacheProductFixture, CudaRendererPreservesCurrentPlanAndResultCacheKeys) {
  static_assert(std::is_same_v<CudaRenderer, Renderer<CudaBackend>>);
  ASSERT_TRUE(OutputIsFinite(Render()));
  EXPECT_EQ(renderer_->PlanCache().BackendCapabilityVersion(), kCudaDagBackendCapabilityVersion);

  auto&      encoded       = image_->GetBuffer();
  const auto encoded_bytes = std::span<const std::byte>{
      reinterpret_cast<const std::byte*>(encoded.data()), encoded.size()};
  auto       source_lease = renderer_->SourceCache().AcquireEncoded(encoded_bytes, DecodeRes::FULL);
  const auto& prepared    = source_lease.Get();
  const auto expected_plan = GraphCompiler::CompileStatic(
      *document_, prepared.CompileSource(), kCudaDagBackendCapabilityVersion);
  auto plan = renderer_->PlanCache().GetOrCompile(*document_, prepared.CompileSource());
  GraphCompiler::BindFrameGeometry(plan, *document_, {});
  EXPECT_EQ(plan.static_key.backend_capability_version, kCudaDagBackendCapabilityVersion);
  EXPECT_EQ(plan.static_key, expected_plan.static_key);
  EXPECT_EQ(plan.sensor_linear_output.producer.Value(), "develop");
  EXPECT_EQ(plan.sensor_linear_output.output_port.Value(), "sensor_linear");
  EXPECT_EQ(plan.geometry_output.producer.Value(), "geometry");
  EXPECT_EQ(plan.geometry_output.output_port.Value(), "scene_source");
  EXPECT_EQ(plan.develop_output.producer.Value(), "develop");
  EXPECT_EQ(plan.develop_output.output_port.Value(), "image");
  EXPECT_EQ(plan.passes.size(), expected_plan.passes.size());
  for (std::size_t i = 0; i < expected_plan.passes.size(); ++i) {
    EXPECT_EQ(plan.passes[i].kind, expected_plan.passes[i].kind);
  }

  const auto keys = BuildFrameResultContentKeys(plan, prepared, *document_);
  renderer_->Device().WaitIdle();
  const auto completed = renderer_->Device().Workspace().Device().CompletedSubmission();
  auto&      images    = renderer_->Device().Workspace().Images();
  auto&      invalidation = renderer_->Device().Workspace().ResultInvalidation();
  ASSERT_NE(plan.FirstGrade(), nullptr);
  const auto expect_current = [&](const GraphValueId& id, ImageExtent extent) {
    EXPECT_NE(images.PublishedRevision(id), 0U);
    EXPECT_EQ(images.PublishedRevision(id), invalidation.RequiredRevision(id));
    EXPECT_TRUE(images.FindValidResult(id, invalidation.RequiredRevision(id),
                                       invalidation.MakeImageRepresentation(id, extent,
                                                                            TextureFormat::Rgba32f),
                                       completed));
  };
  expect_current(plan.sensor_linear_output, keys.sensor_extent);
  expect_current(plan.geometry_output, keys.geometry_extent);
  expect_current(plan.develop_output, keys.geometry_extent);
  EXPECT_EQ(images.Find(plan.FirstGrade()->scene_output), nullptr);
  expect_current(plan.display_output, keys.geometry_extent);
}

TEST_F(CudaResultCacheProductFixture, RepeatedBatchRendersReuseDeviceAndReleaseWorkspace) {
  ASSERT_TRUE(OutputIsFinite(Render()));
  const auto session_before = renderer_->Resources();
  EXPECT_GT(session_before.published_result_count, 0U);
  renderer_->ResetStats();
  EXPECT_EQ(batch_renderer_->DebugDeviceIdentity(), 0U);

  ASSERT_TRUE(OutputIsFinite(RenderBatch()));
  const auto first_id = batch_renderer_->DebugDeviceIdentity();
  EXPECT_NE(first_id, 0U);
  EXPECT_NE(first_id, renderer_->DebugDeviceIdentity());
  EXPECT_EQ(batch_renderer_->Resources().published_result_count, 0U);
  EXPECT_EQ(batch_renderer_->Resources().texture_pool_used_bytes, 0U);
  EXPECT_EQ(batch_renderer_->Resources().texture_pool_entry_count, 0U);
  EXPECT_EQ(renderer_->Resources().published_result_count, session_before.published_result_count);
  EXPECT_EQ(renderer_->Resources().prepared_source_entry_count,
            session_before.prepared_source_entry_count);
  EXPECT_EQ(renderer_->Stats().prepared_source_hits, 0U);
  EXPECT_EQ(renderer_->Stats().prepared_source_misses, 0U);
  EXPECT_EQ(renderer_->Stats().pass.sensor_develop_execute, 0U);

  ASSERT_TRUE(OutputIsFinite(RenderBatch()));
  EXPECT_EQ(batch_renderer_->DebugDeviceIdentity(), first_id);
  EXPECT_EQ(batch_renderer_->Resources().published_result_count, 0U);
  EXPECT_EQ(batch_renderer_->Resources().texture_pool_used_bytes, 0U);
  EXPECT_EQ(batch_renderer_->Resources().texture_pool_entry_count, 0U);
  EXPECT_EQ(renderer_->Resources().published_result_count, session_before.published_result_count);

  ASSERT_TRUE(OutputIsFinite(Render()));
  EXPECT_EQ(renderer_->Stats().prepared_source_hits, 1U);
  EXPECT_EQ(renderer_->Stats().libraw_open_unpack_count, 0U);
  EXPECT_EQ(renderer_->Stats().pass.sensor_develop_execute, 0U);
  EXPECT_EQ(renderer_->Stats().pass.drt_skip, 1U);
}

// Plan §3.3: a batch task is one binding. A batch render that fails inside Execute releases its
// result textures, transients, and parameter slots before it throws, not at the next successful
// batch render, and keeps its device. The test fails each upload of the render in turn (#288).
TEST_F(CudaResultCacheProductFixture, BatchRenderFailureDuringExecuteReleasesEveryResultResource) {
  ASSERT_TRUE(OutputIsFinite(RenderBatch()));
  const auto device = batch_renderer_->DebugDeviceIdentity();
  ASSERT_NE(device, 0U);
  auto&         backend         = batch_renderer_->Device().Workspace().Device();
  std::uint32_t failed_renders  = 0;
  bool          render_finished = false;
  // The render finishes once the armed failure lies past the last upload of the render.
  for (std::uint32_t uploads_to_pass = 0; uploads_to_pass < 256 && !render_finished;
       ++uploads_to_pass) {
    SCOPED_TRACE(uploads_to_pass);
    backend.FailUploadAfter(uploads_to_pass);
    try {
      render_finished = OutputIsFinite(RenderBatch());
      ASSERT_TRUE(render_finished);
    } catch (const std::runtime_error& ex) {
      ++failed_renders;
      EXPECT_NE(std::string(ex.what()).find("injected failure"), std::string::npos) << ex.what();
      const auto resources = batch_renderer_->Resources();
      EXPECT_EQ(resources.published_result_count, 0U);
      EXPECT_TRUE(resources.session_value_ids.empty());
      EXPECT_EQ(resources.texture_pool_used_bytes, 0U);
      EXPECT_EQ(resources.texture_pool_entry_count, 0U);
      EXPECT_EQ(resources.transient_used_bytes, 0U);
      EXPECT_EQ(resources.transient_capacity_bytes, 0U);
      EXPECT_EQ(resources.transient_slab_count, 0U);
      EXPECT_EQ(resources.parameter_slot_count, 0U);
      EXPECT_EQ(resources.parameter_capacity_bytes, 0U);
      EXPECT_EQ(batch_renderer_->Device().Workspace().Images().UnpublishedCount(), 0U);
      EXPECT_FALSE(batch_renderer_->Device().Workspace().IsRendering());
      EXPECT_EQ(batch_renderer_->DebugDeviceIdentity(), device);
    }
  }
  EXPECT_TRUE(render_finished);
  EXPECT_GT(failed_renders, 1U);
}

TEST_F(CudaResultCacheProductFixture, ParallelBatchRendersCompleteAndReleaseWorkspaces) {
  constexpr int kWorkers = 2;
  struct Worker {
    std::unique_ptr<CudaProductRenderer>         renderer;
    std::shared_ptr<const PipelineGraphSnapshot> snapshot;
    std::shared_ptr<ImageBuffer>                 image;
    bool                                         finite          = false;
    std::uintptr_t                               device_identity = 0;
    std::size_t                                  published       = 1;
    std::size_t                                  pool_bytes      = 1;
    std::size_t                                  pool_entries    = 1;
    std::uint64_t                                source_hits     = 1;
    std::uint64_t                                source_misses   = 1;
    std::string                                  error;
  };

  std::vector<Worker> workers(static_cast<std::size_t>(kWorkers));
  for (int i = 0; i < kWorkers; ++i) {
    auto document = std::make_shared<PipelineDocument>(CreateDefaultPipelineDocument());
    gpu_dag_test::EnsureTestCameraProfile(*document);
    workers[static_cast<std::size_t>(i)].renderer =
        std::make_unique<CudaProductRenderer>(ExecutorRole::Batch, MakeUnpacker());
    workers[static_cast<std::size_t>(i)].snapshot = test::FreezeInNewLineage(*document);
    workers[static_cast<std::size_t>(i)].image =
        MakeEncodedImage(static_cast<std::uint8_t>(90 + i));
  }

  std::promise<void> start;
  const auto         go = start.get_future().share();
  std::atomic<int>   ready{0};
  std::vector<std::thread> threads;
  threads.reserve(static_cast<std::size_t>(kWorkers));
  for (int i = 0; i < kWorkers; ++i) {
    threads.emplace_back([&, i] {
      auto& worker = workers[static_cast<std::size_t>(i)];
      ready.fetch_add(1, std::memory_order_relaxed);
      go.wait();
      try {
        const auto output =
            RenderHost(*worker.renderer, *worker.snapshot, worker.image, DecodeRes::FULL, {});
        worker.finite          = OutputIsFinite(output);
        worker.device_identity = worker.renderer->DebugDeviceIdentity();
        const auto batch       = worker.renderer->Resources();
        worker.published       = batch.published_result_count;
        worker.pool_bytes      = batch.texture_pool_used_bytes;
        worker.pool_entries    = batch.texture_pool_entry_count;
        worker.source_hits     = worker.renderer->Stats().prepared_source_hits;
        worker.source_misses   = worker.renderer->Stats().prepared_source_misses;
      } catch (const std::exception& ex) {
        worker.error = ex.what();
      } catch (...) {
        worker.error = "unknown parallel batch render failure";
      }
    });
  }

  while (ready.load(std::memory_order_relaxed) < kWorkers) {
    std::this_thread::yield();
  }
  start.set_value();
  for (auto& thread : threads) {
    thread.join();
  }

  for (int i = 0; i < kWorkers; ++i) {
    SCOPED_TRACE(i);
    const auto& worker = workers[static_cast<std::size_t>(i)];
    EXPECT_TRUE(worker.error.empty()) << worker.error;
    EXPECT_TRUE(worker.finite);
    EXPECT_NE(worker.device_identity, 0U);
    EXPECT_EQ(worker.published, 0U);
    EXPECT_EQ(worker.pool_bytes, 0U);
    EXPECT_EQ(worker.pool_entries, 0U);
    EXPECT_EQ(worker.source_hits, 0U);
    EXPECT_EQ(worker.source_misses, 0U);
  }
  EXPECT_NE(workers[0].device_identity, workers[1].device_identity);
}

TEST_F(CudaResultCacheProductFixture, BatchRendererWorkspaceCannotPublishIntoInteractiveCache) {
  ASSERT_TRUE(OutputIsFinite(Render()));
  const auto session_before = renderer_->Resources();
  EXPECT_GT(session_before.published_result_count, 0U);
  renderer_->ResetStats();

  ASSERT_TRUE(OutputIsFinite(RenderBatch()));
  EXPECT_EQ(batch_renderer_->Resources().published_result_count, 0U);
  EXPECT_TRUE(batch_renderer_->Resources().session_value_ids.empty());
  EXPECT_EQ(renderer_->Resources().published_result_count, session_before.published_result_count);
  EXPECT_EQ(renderer_->Resources().prepared_source_entry_count,
            session_before.prepared_source_entry_count);
  EXPECT_EQ(renderer_->Stats().prepared_source_hits, 0U);
  EXPECT_EQ(renderer_->Stats().plan_cache_hits, 0U);
  EXPECT_EQ(renderer_->Stats().pass.sensor_develop_execute, 0U);

  ASSERT_TRUE(OutputIsFinite(Render()));
  EXPECT_EQ(renderer_->Stats().prepared_source_hits, 1U);
  EXPECT_EQ(renderer_->Stats().pass.sensor_develop_execute, 0U);
  EXPECT_EQ(renderer_->Stats().pass.drt_skip, 1U);
}

TEST_F(CudaResultCacheProductFixture, RendererFailureDoesNotPublishUnfinishedRevisions) {
  ASSERT_TRUE(OutputIsFinite(Render()));
  auto&      encoded       = image_->GetBuffer();
  const auto encoded_bytes = std::span<const std::byte>{
      reinterpret_cast<const std::byte*>(encoded.data()), encoded.size()};
  auto        source_lease = renderer_->SourceCache().AcquireEncoded(encoded_bytes, DecodeRes::FULL);
  const auto& prepared     = source_lease.Get();
  auto        first_plan   = renderer_->PlanCache().GetOrCompile(*document_, prepared.CompileSource());
  GraphCompiler::BindFrameGeometry(first_plan, *document_, {});
  const auto published_before = renderer_->Resources().published_result_count;
  auto&      images           = renderer_->Device().Workspace().Images();
  const auto published_sensor = images.PublishedRevision(first_plan.sensor_linear_output);
  const auto published_display = images.PublishedRevision(first_plan.display_output);
  const auto sensor_repr      = images.PublishedRepresentation(first_plan.sensor_linear_output);
  const auto display_repr     = images.PublishedRepresentation(first_plan.display_output);

  auto develop                   = document_->Develop()->Params().Params();
  develop.highlights_reconstruct = !develop.highlights_reconstruct;
  document_->Develop()->Params().ReplaceParams(develop);
  renderer_->Device().Workspace().Device().FailNextUpload();
  EXPECT_THROW(Render(), std::runtime_error);

  renderer_->Device().WaitIdle();
  const auto completed = renderer_->Device().Workspace().Device().CompletedSubmission();
  auto&      invalidation = renderer_->Device().Workspace().ResultInvalidation();
  EXPECT_EQ(renderer_->Resources().published_result_count, published_before);
  EXPECT_TRUE(images.FindValidResult(first_plan.sensor_linear_output, published_sensor, sensor_repr,
                                     completed));
  EXPECT_TRUE(images.FindValidResult(first_plan.display_output, published_display, display_repr,
                                     completed));
  EXPECT_NE(invalidation.RequiredRevision(first_plan.sensor_linear_output), published_sensor);
  EXPECT_NE(invalidation.RequiredRevision(first_plan.display_output), published_display);
  EXPECT_EQ(images.UnpublishedCount(), 0U);
}

TEST_F(CudaResultCacheProductFixture,
       DownstreamEditsPreserveValidDevelopAndInteractiveResize) {
  ASSERT_TRUE(OutputIsFinite(RenderRole(FrameRole::InteractivePrimary, 16)));
  auto& images = renderer_->Device().Workspace().Images();
  const auto sensor_handle = images.Find(SensorId())->Handle();
  const auto sensor_rev    = images.PublishedRevision(SensorId());
  const auto geometry_rev  = images.PublishedRevision(GeometryId());
  renderer_->ResetStats();
  auto* exposure = Exposure();
  ASSERT_NE(exposure, nullptr);
  exposure->SetValue(0.35f);
  const auto after_first = RenderRole(FrameRole::InteractivePrimary, 16);
  ASSERT_TRUE(OutputIsFinite(after_first));
  exposure->SetValue(0.55f);
  const auto after_second = RenderRole(FrameRole::InteractivePrimary, 16);
  ASSERT_TRUE(OutputIsFinite(after_second));
  const auto stats = renderer_->Stats();
  EXPECT_EQ(stats.pass.sensor_develop_execute, 0U);
  EXPECT_EQ(stats.pass.geometry_execute, 0U);
  EXPECT_EQ(images.Find(SensorId())->Handle(), sensor_handle);
  EXPECT_EQ(images.PublishedRevision(SensorId()), sensor_rev);
  EXPECT_EQ(images.PublishedRevision(GeometryId()), geometry_rev);
  const auto fresh = RenderBatch([] {
    RenderRequest request;
    request.resolution.max_edge = 16;
    return request;
  }());
  ASSERT_TRUE(OutputIsFinite(fresh));
  EXPECT_TRUE(CompareHostRgba(after_second, fresh, 1.0e-4f));
}

TEST_F(CudaResultCacheProductFixture, QualityBaseBypassesEveryResultCacheAfterSensorDevelop) {
  ConnectFullCoverageMask(*document_);
  auto* shadows = dynamic_cast<ShadowsModel*>(
      document_->PrimaryGrade()->FindAdjustmentByType(type_ids::Shadows()));
  ASSERT_NE(shadows, nullptr);
  shadows->SetValue(40.0f);
  ASSERT_TRUE(OutputIsFinite(RenderRole(FrameRole::InteractivePrimary, 16)));
  auto& images = renderer_->Device().Workspace().Images();
  const auto published_before = images.CurrentValueIds();
  const auto geometry_handle  = images.Find(GeometryId())->Handle();
  const auto geometry_rev     = images.PublishedRevision(GeometryId());
  const auto lookups_before   = images.LookupCount();
  const auto publishes_before = images.PersistentPublishCount();
  const auto values_before    = renderer_->Device().Workspace().Values().Size();
  renderer_->ResetStats();
  ASSERT_TRUE(OutputIsFinite(RenderRole(FrameRole::QualityBase, 32)));
  const auto stats = renderer_->Stats();
  EXPECT_EQ(stats.pass.sensor_develop_execute, 0U);
  EXPECT_EQ(stats.pass.sensor_develop_skip, 1U);
  EXPECT_EQ(stats.pass.geometry_skip, 0U);
  EXPECT_GE(stats.pass.geometry_execute, 1U);
  EXPECT_EQ(stats.pass.camera_color_skip, 0U);
  EXPECT_EQ(stats.pass.primary_grade_skip, 0U);
  EXPECT_EQ(stats.pass.drt_skip, 0U);
  EXPECT_EQ(stats.pass.mask_skip, 0U);
  EXPECT_GT(stats.pass.result_policy_bypass, 0U);
  EXPECT_EQ(stats.pass.persistent_result_lookups, 1U);
  EXPECT_EQ(images.LookupCount() - lookups_before, 1U);
  EXPECT_EQ(images.PersistentPublishCount(), publishes_before);
  EXPECT_EQ(images.UnpublishedCount(), 0U);
  EXPECT_EQ(images.Find(GeometryId())->Handle(), geometry_handle);
  EXPECT_EQ(images.PublishedRevision(GeometryId()), geometry_rev);
  EXPECT_EQ(images.PublishedRepresentation(GeometryId()).extent.width, 16U);
  EXPECT_EQ(renderer_->Device().Workspace().Values().Size(), values_before);
  auto ids = images.CurrentValueIds();
  ASSERT_EQ(ids.size(), published_before.size());
}

TEST_F(CudaResultCacheProductFixture,
       InteractiveAfterQualityReusesKeyStagesAndReexecutesUnpublishedGrade) {
  ASSERT_TRUE(OutputIsFinite(RenderRole(FrameRole::InteractivePrimary, 16)));
  auto& images = renderer_->Device().Workspace().Images();
  const auto geometry_handle = images.Find(GeometryId())->Handle();
  const auto geometry_rev    = images.PublishedRevision(GeometryId());
  const auto geometry_repr   = images.PublishedRepresentation(GeometryId());
  const auto sensor_handle   = images.Find(SensorId())->Handle();
  ASSERT_TRUE(OutputIsFinite(RenderRole(FrameRole::QualityBase, 32)));
  EXPECT_EQ(images.Find(GeometryId())->Handle(), geometry_handle);
  EXPECT_EQ(images.PublishedRevision(GeometryId()), geometry_rev);
  EXPECT_EQ(images.PublishedRepresentation(GeometryId()).extent, geometry_repr.extent);
  EXPECT_EQ(images.Find(SensorId())->Handle(), sensor_handle);
  renderer_->ResetStats();
  ASSERT_TRUE(OutputIsFinite(RenderRole(FrameRole::InteractivePrimary, 16)));
  const auto stats = renderer_->Stats();
  EXPECT_EQ(stats.pass.sensor_develop_execute, 0U);
  EXPECT_EQ(stats.pass.geometry_execute, 0U);
  EXPECT_EQ(stats.pass.camera_color_execute, 0U);
  EXPECT_EQ(stats.pass.primary_grade_execute, 1U);
  EXPECT_EQ(stats.pass.drt_execute, 0U);
  EXPECT_EQ(images.Find(GraphValueId{NodeId{"grade.primary"}, PortId{"image"}}), nullptr);
  EXPECT_EQ(images.PublishedRepresentation(GeometryId()).extent.width, 16U);
}

TEST_F(CudaResultCacheProductFixture,
       QualityBaseMutationInvalidatesOnlyDependentInteractiveResults) {
  ASSERT_TRUE(OutputIsFinite(RenderRole(FrameRole::InteractivePrimary, 16)));
  auto* exposure = Exposure();
  ASSERT_NE(exposure, nullptr);
  exposure->SetValue(0.45f);
  renderer_->ResetStats();
  ASSERT_TRUE(OutputIsFinite(RenderRole(FrameRole::QualityBase, 32)));
  EXPECT_EQ(renderer_->Stats().pass.sensor_develop_execute, 0U);
  renderer_->ResetStats();
  ASSERT_TRUE(OutputIsFinite(RenderRole(FrameRole::InteractivePrimary, 16)));
  auto stats = renderer_->Stats();
  EXPECT_EQ(stats.pass.sensor_develop_execute, 0U);
  EXPECT_EQ(stats.pass.geometry_execute, 0U);
  EXPECT_GE(stats.pass.primary_grade_execute, 1U);

  document_->Geometry().SetCropRect({0.1f, 0.1f, 0.8f, 0.8f});
  renderer_->ResetStats();
  ASSERT_TRUE(OutputIsFinite(RenderRole(FrameRole::QualityBase, 32)));
  EXPECT_EQ(renderer_->Stats().pass.sensor_develop_execute, 0U);
  renderer_->ResetStats();
  ASSERT_TRUE(OutputIsFinite(RenderRole(FrameRole::InteractivePrimary, 16)));
  stats = renderer_->Stats();
  EXPECT_EQ(stats.pass.sensor_develop_execute, 0U);
  EXPECT_GE(stats.pass.geometry_execute, 1U);

  auto develop                   = document_->Develop()->Params().Params();
  develop.highlights_reconstruct = !develop.highlights_reconstruct;
  document_->Develop()->Params().ReplaceParams(develop);
  renderer_->ResetStats();
  ASSERT_TRUE(OutputIsFinite(RenderRole(FrameRole::QualityBase, 32)));
  EXPECT_GE(renderer_->Stats().pass.sensor_develop_execute, 1U);
  renderer_->ResetStats();
  ASSERT_TRUE(OutputIsFinite(RenderRole(FrameRole::InteractivePrimary, 16)));
  stats = renderer_->Stats();
  EXPECT_EQ(stats.pass.sensor_develop_execute, 0U);
  EXPECT_GE(stats.pass.geometry_execute, 1U);
}

TEST_F(CudaResultCacheProductFixture,
       RepeatedQualityBaseRendersReleaseDownstreamStorageAfterPresentation) {
  ASSERT_TRUE(OutputIsFinite(RenderRole(FrameRole::InteractivePrimary, 16)));
  const auto published = renderer_->Device().Workspace().Images().PublishedCount();
  const auto values    = renderer_->Device().Workspace().Values().Size();
  std::size_t used_after_first_cycle = 0;
  std::size_t entries_after_first    = 0;
  for (int round = 0; round < 4; ++round) {
    auto* exposure = Exposure();
    ASSERT_NE(exposure, nullptr);
    exposure->SetValue(0.1f * static_cast<float>(round + 1));
    ASSERT_TRUE(OutputIsFinite(RenderRole(FrameRole::QualityBase, 32)));
    EXPECT_EQ(renderer_->Device().Workspace().Images().PublishedCount(), published);
    EXPECT_EQ(renderer_->Device().Workspace().Images().UnpublishedCount(), 0U);
    EXPECT_EQ(renderer_->Device().Workspace().Values().Size(), values);
    ASSERT_TRUE(OutputIsFinite(RenderRole(FrameRole::InteractivePrimary, 16)));
    const auto used    = renderer_->Device().Workspace().Textures().UsedBytes();
    const auto entries = renderer_->Device().Workspace().Textures().EntryCount();
    if (round == 0) {
      used_after_first_cycle = used;
      entries_after_first    = entries;
    } else {
      EXPECT_LE(used, used_after_first_cycle);
      EXPECT_LE(entries, entries_after_first);
    }
  }
}

TEST_F(CudaResultCacheProductFixture, QualityBaseFailurePreservesUnchangedInteractiveResults) {
  ASSERT_TRUE(OutputIsFinite(RenderRole(FrameRole::InteractivePrimary, 16)));
  auto& images = renderer_->Device().Workspace().Images();
  const auto geometry_handle = images.Find(GeometryId())->Handle();
  const auto geometry_rev    = images.PublishedRevision(GeometryId());
  const auto sensor_rev      = images.PublishedRevision(SensorId());
  const auto completed_geo =
      renderer_->Device().Workspace().ResultInvalidation().CompletedRevision(GeometryId());
  RejectingPresentSink sink;
  RenderRequest request;
  request.resolution.max_edge = 32;
  FrameCompletionSubmission submission;
  submission.metadata.frame_role = FrameRole::QualityBase;
  EXPECT_THROW((void)renderer_->Render(*source_->Freeze(), image_, DecodeRes::FULL, request, &sink,
                                       submission, false),
               std::runtime_error);
  EXPECT_EQ(images.Find(GeometryId())->Handle(), geometry_handle);
  EXPECT_EQ(images.PublishedRevision(GeometryId()), geometry_rev);
  EXPECT_EQ(images.PublishedRevision(SensorId()), sensor_rev);
  EXPECT_EQ(renderer_->Device().Workspace().ResultInvalidation().CompletedRevision(GeometryId()),
            completed_geo);
  EXPECT_EQ(images.UnpublishedCount(), 0U);
}

TEST_F(CudaResultCacheProductFixture, QualityBasePixelsMatchFreshExecutionWithinDeclaredTolerance) {
  // Same 32x32 fixture, QualityBase long-edge 32. Compare bypass-cache pixels
  // against a batch execution of the same request. Absolute tolerance 1e-4
  // on RGB in the renderer host RGBA32F download (ACES display encoding).
  ConnectFullCoverageMask(*document_);
  auto* shadows = dynamic_cast<ShadowsModel*>(
      document_->PrimaryGrade()->FindAdjustmentByType(type_ids::Shadows()));
  ASSERT_NE(shadows, nullptr);
  shadows->SetValue(35.0f);
  document_->Geometry().SetCropRect({0.05f, 0.05f, 0.9f, 0.9f});
  auto* exposure = Exposure();
  ASSERT_NE(exposure, nullptr);
  exposure->SetValue(0.4f);
  ASSERT_TRUE(OutputIsFinite(RenderRole(FrameRole::InteractivePrimary, 16)));
  const auto quality = RenderRole(FrameRole::QualityBase, 32);
  ASSERT_TRUE(OutputIsFinite(quality));
  RenderRequest fresh_request;
  fresh_request.resolution.max_edge = 32;
  const auto fresh = RenderBatch(fresh_request);
  ASSERT_TRUE(OutputIsFinite(fresh));
  EXPECT_EQ(quality->GetCPUData().cols, fresh->GetCPUData().cols);
  EXPECT_EQ(quality->GetCPUData().rows, fresh->GetCPUData().rows);
  EXPECT_TRUE(CompareHostRgba(quality, fresh, 1.0e-4f));
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

TEST_F(CudaResultCacheDeviceFixture, FailedSubmissionDoesNotPublishResultRevision) {
  auto plan = Compile();
  ASSERT_EQ(device_.Execute(plan, input_, document_), plan.display_output);
  auto& images            = device_.Workspace().Images();
  const auto published_sensor  = images.PublishedRevision(plan.sensor_linear_output);
  const auto published_display = images.PublishedRevision(plan.display_output);
  const auto sensor_repr       = images.PublishedRepresentation(plan.sensor_linear_output);
  const auto display_repr      = images.PublishedRepresentation(plan.display_output);
  auto develop                   = document_.Develop()->Params().Params();
  develop.highlights_reconstruct = !develop.highlights_reconstruct;
  document_.Develop()->Params().ReplaceParams(develop);
  device_.Workspace().Device().FailNextUpload();
  EXPECT_THROW((void)device_.Execute(plan, input_, document_), std::runtime_error);
  device_.WaitIdle();
  const auto completed    = device_.Workspace().Device().CompletedSubmission();
  auto&      invalidation = device_.Workspace().ResultInvalidation();
  EXPECT_TRUE(images.FindValidResult(plan.sensor_linear_output, published_sensor, sensor_repr,
                                     completed));
  EXPECT_TRUE(images.FindValidResult(plan.display_output, published_display, display_repr,
                                     completed));
  EXPECT_NE(invalidation.RequiredRevision(plan.sensor_linear_output), published_sensor);
  EXPECT_NE(invalidation.RequiredRevision(plan.display_output), published_display);
}

TEST_F(CudaResultCacheDeviceFixture,
       CancelledSubmissionKeepsPreviouslyCompletedCacheEntriesUsable) {
  auto plan = Compile();
  ASSERT_EQ(device_.Execute(plan, input_, document_), plan.display_output);
  auto&      images   = device_.Workspace().Images();
  const auto revision = images.PublishedRevision(plan.display_output);
  const auto repr     = images.PublishedRepresentation(plan.display_output);
  device_.BeginRender();
  (void)device_.Workspace().AcquireImageForWrite(
      plan.display_output, {repr.extent.width, repr.extent.height, TextureFormat::Rgba32f});
  device_.CancelRender();
  device_.WaitIdle();
  const auto completed = device_.Workspace().Device().CompletedSubmission();
  EXPECT_TRUE(images.FindValidResult(plan.display_output, revision, repr, completed));
  ASSERT_EQ(device_.Execute(plan, input_, document_), plan.display_output);
  EXPECT_EQ(device_.PassStats().drt_skip, 1U);
}

}  // namespace
}  // namespace alcedo
