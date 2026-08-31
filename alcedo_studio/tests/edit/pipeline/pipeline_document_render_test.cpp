// Copyright 2026 Yurun Zi
// SPDX-License-Identifier: GPL-3.0-only
// Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>
#include <libraw/libraw.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <vector>

#include "edit/graph/develop_color_transform.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/operators/operator_registeration.hpp"
#include "edit/pipeline/pipeline_cpu.hpp"
#include "edit/runtime/cuda/cuda_product_renderer.hpp"
#include "image/dng_color_profile_import.hpp"
#include "image/metadata_extractor.hpp"

namespace alcedo {
namespace {

/** @brief Capture actual CUDA presentation pixels in host memory, without a GUI. */
class PixelFrameSink final : public IFrameSink {
 public:
  void EnsureSize(int width, int height) override { pixels.create(height, width, CV_32FC4); }
  auto MapResourceForWrite(FrameMemoryDomain) -> FrameWriteMapping override {
    if (reject_mapping) return {};
    FrameWriteMapping mapping;
    mapping.data          = pixels.data;
    mapping.row_bytes     = pixels.step;
    mapping.pixel_format  = FramePixelFormat::RGBA32F;
    mapping.memory_domain = FrameMemoryDomain::HostVisible;
    mapping.target_type   = FrameWriteTargetType::LinearBuffer;
    return mapping;
  }
  void    UnmapResource() override {}
  void    NotifyFrameReady(const FrameCompletionSubmission&) override { ++ready_count; }
  void    SubmitHostFrame(const ViewerFrame&) override { ++host_frame_count; }
  auto    GetWidth() const -> int override { return pixels.cols; }
  auto    GetHeight() const -> int override { return pixels.rows; }

  cv::Mat pixels;
  int     ready_count      = 0;
  int     host_frame_count = 0;
  bool    reject_mapping   = false;
};

/** @brief One real linear DNG plus an imported Default document, with no application services. */
class PipelineDocumentRenderTest : public ::testing::Test {
 protected:
  void SetUp() override {
    RegisterAllOperators();
    const auto path = std::filesystem::path(TEST_IMG_PATH) / "raw/linear_dng/mfzoty.dng";
    ASSERT_TRUE(std::filesystem::exists(path)) << path.string();
    std::ifstream             stream(path, std::ios::binary);
    std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>(stream),
                                    std::istreambuf_iterator<char>()};
    ASSERT_FALSE(bytes.empty());
    auto raw = std::make_unique<LibRaw>();
    ASSERT_EQ(raw->open_buffer(bytes.data(), bytes.size()), LIBRAW_SUCCESS);
    ASSERT_EQ(raw->unpack(), LIBRAW_SUCCESS);
    full_extent_ = cv::Size(raw->imgdata.sizes.width, raw->imgdata.sizes.height);
    if (raw->imgdata.sizes.flip == 5 || raw->imgdata.sizes.flip == 6) {
      std::swap(full_extent_.width, full_extent_.height);
    }
    MetadataExtractor::PopulateRuntimeContextFromOpenLibRaw(*raw, imported_);
    // Read the actual embedded profile from bytes; do not replace it with an identity profile.
    const auto exif = MetadataExtractor::ExtractEXIFFromBuffer(bytes.data(), bytes.size());
    ASSERT_NE(exif, nullptr);
    imported_.dng_profile_ = ReadDngColorProfile(exif->exifData());
    ASSERT_NE(imported_.dng_profile_, nullptr);
    ASSERT_TRUE(imported_.color_matrices_valid_);
    input_    = std::make_shared<ImageBuffer>(std::move(bytes));
    document_ = std::make_shared<PipelineDocument>(CreateDefaultPipelineDocument());
    executor_ = std::make_unique<CPUPipelineExecutor>();
    executor_->SetAcceleratorBackendPreference(AcceleratorBackendPreference::CUDA);
    executor_->SetPipelineDocument(document_, true);
    executor_->InjectRawMetadata(imported_);
    executor_->SetDecodeRes(DecodeRes::FULL);
    executor_->SetRenderRes(false, 256);
    executor_->SetEnableCache(true);
  }

  /** @brief Apply an exposure edit to the same persistent Model used by the executor. */
  void SetExposure(float ev) {
    document_->PrimaryGrade()
        ->FindAdjustmentByType(type_ids::Exposure())
        ->LoadJson({{"exposure_ev", ev}});
  }

  /** @brief Execute with the same exclusive access required by the scheduler. */
  auto Render(bool host) -> cv::Mat {
    std::unique_lock lock(executor_->GetRenderLock());
    executor_->SetForceCPUOutput(host);
    executor_->AttachFrameSink(host ? nullptr : &sink_);
    const auto result = executor_->Apply(input_);
    if (!result) throw std::runtime_error("Missing render result");
    return host ? result->GetCPUData().clone() : sink_.pixels.clone();
  }

  /** @brief Render a separately initialized reference through the document-only Renderer API. */
  auto Reference(float ev, RenderQuality quality = RenderQuality::Export) -> cv::Mat {
    auto document = std::make_shared<PipelineDocument>(CreateDefaultPipelineDocument());
    auto develop  = document->Develop()->Params().Params();
    BindDevelopCameraProfile(develop, imported_);
    document->Develop()->Params().ReplaceParams(develop);
    document->PrimaryGrade()
        ->FindAdjustmentByType(type_ids::Exposure())
        ->LoadJson({{"exposure_ev", ev}});
    CudaRenderer  renderer(document);
    RenderRequest request;
    request.resolution.max_edge = 256;
    request.resolution.quality  = quality;
    return renderer.Render(input_, DecodeRes::FULL, request, nullptr, {}, true)
        ->GetCPUData()
        .clone();
  }

  RawRuntimeColorContext               imported_;
  std::shared_ptr<ImageBuffer>         input_;
  std::shared_ptr<PipelineDocument>    document_;
  std::unique_ptr<CPUPipelineExecutor> executor_;
  PixelFrameSink                       sink_;
  cv::Size                             full_extent_;
};

TEST_F(PipelineDocumentRenderTest, EditorAndHostRenderUseDocumentParameters) {
  // Stage values deliberately disagree, including a stage change between editor renders.
  executor_->GetStage(PipelineStageName::Basic_Adjustment)
      .SetOperator(OperatorType::EXPOSURE, {{"exposure", 9.0f}});
  SetExposure(-1.5f);
  const auto dark_reference = Reference(-1.5f);
  const auto dark_editor    = Render(false);
  const auto dark_host      = Render(true);
  ASSERT_EQ(dark_editor.size(), dark_reference.size());
  ASSERT_EQ(dark_host.type(), dark_reference.type());
  EXPECT_LT(cv::norm(dark_editor, Reference(-1.5f, RenderQuality::Preview), cv::NORM_INF), 2e-5);
  EXPECT_LT(cv::norm(dark_host, dark_reference, cv::NORM_INF), 2e-5);

  executor_->GetStage(PipelineStageName::Basic_Adjustment)
      .SetOperator(OperatorType::EXPOSURE, {{"exposure", -9.0f}});
  SetExposure(1.5f);
  const auto bright_reference = Reference(1.5f);
  const auto bright_editor    = Render(false);
  const auto bright_host      = Render(true);
  EXPECT_LT(cv::norm(bright_editor, Reference(1.5f, RenderQuality::Preview), cv::NORM_INF), 2e-5);
  EXPECT_LT(cv::norm(bright_host, bright_reference, cv::NORM_INF), 2e-5);
  EXPECT_GT(
      cv::norm(bright_reference, dark_reference, cv::NORM_L1) / (3.0 * dark_reference.total()),
      0.02);
  EXPECT_GT(cv::mean(bright_host)[1], cv::mean(dark_host)[1] + 0.02);
  EXPECT_EQ(sink_.ready_count, 2);
  EXPECT_EQ(sink_.host_frame_count, 0);
  EXPECT_EQ(executor_->GpuDagDocument().get(), document_.get());
}

TEST_F(PipelineDocumentRenderTest, ConsecutiveDocumentEditsReusePreparedSourceAndGeometry) {
  const auto before = Render(false);
  const auto first  = executor_->DebugCudaRenderer()->Stats();
  SetExposure(-1.5f);
  const auto after  = Render(false);
  const auto second = executor_->DebugCudaRenderer()->Stats();
  EXPECT_EQ(second.libraw_open_unpack_count, first.libraw_open_unpack_count);
  EXPECT_EQ(second.pass.sensor_develop_execute, first.pass.sensor_develop_execute);
  EXPECT_EQ(second.pass.geometry_execute, first.pass.geometry_execute);
  EXPECT_GT(second.pass.geometry_skip, first.pass.geometry_skip);
  EXPECT_GT(second.pass.primary_grade_execute, first.pass.primary_grade_execute);
  EXPECT_GT(cv::norm(before, after, cv::NORM_INF), 0.02);
}

TEST_F(PipelineDocumentRenderTest, HostBypassRendersReuseOneShotDeviceAndLeaveSessionCacheUntouched) {
  const auto editor = Render(false);
  auto*      renderer = executor_->DebugCudaRenderer();
  ASSERT_NE(renderer, nullptr);
  renderer->ResetStats();
  const auto session_before = renderer->SessionResources();
  EXPECT_GT(session_before.published_result_count, 0U);
  EXPECT_EQ(renderer->DebugOneShotDeviceIdentity(), 0U);

  executor_->SetEnableCache(false);
  const auto host1        = Render(true);
  const auto one_shot_id  = renderer->DebugOneShotDeviceIdentity();
  const auto one_shot     = renderer->OneShotResources();
  EXPECT_NE(one_shot_id, 0U);
  EXPECT_EQ(renderer->OneShotPublishedResultCount(), 0U);
  EXPECT_EQ(one_shot.published_result_count, 0U);
  EXPECT_EQ(one_shot.texture_pool_used_bytes, 0U);
  EXPECT_EQ(one_shot.texture_pool_entry_count, 0U);
  EXPECT_EQ(renderer->Stats().prepared_source_hits, 0U);
  EXPECT_EQ(renderer->Stats().prepared_source_misses, 0U);
  EXPECT_EQ(renderer->Stats().pass.sensor_develop_execute, 0U);
  EXPECT_EQ(renderer->SessionResources().published_result_count,
            session_before.published_result_count);
  EXPECT_EQ(renderer->SessionResources().prepared_source_entry_count,
            session_before.prepared_source_entry_count);
  EXPECT_LT(cv::norm(host1, Reference(1.5f, RenderQuality::Export), cv::NORM_INF), 2e-5);

  const auto host2 = Render(true);
  EXPECT_EQ(renderer->DebugOneShotDeviceIdentity(), one_shot_id);
  EXPECT_EQ(renderer->OneShotPublishedResultCount(), 0U);
  EXPECT_EQ(renderer->OneShotResources().texture_pool_used_bytes, 0U);
  EXPECT_LT(cv::norm(host1, host2, cv::NORM_INF), 2e-5);

  executor_->SetEnableCache(true);
  renderer->ResetStats();
  const auto editor2 = Render(false);
  EXPECT_EQ(renderer->Stats().prepared_source_hits, 1U);
  EXPECT_EQ(renderer->Stats().libraw_open_unpack_count, 0U);
  EXPECT_EQ(renderer->Stats().pass.sensor_develop_execute, 0U);
  EXPECT_LT(cv::norm(editor, editor2, cv::NORM_INF), 2e-5);
}

TEST_F(PipelineDocumentRenderTest, RenderLeavesPersistentDocumentParametersUnchanged) {
  SetExposure(0.75f);
  const auto  before         = document_->ToJson();
  const auto  stages_before  = executor_->ExportPipelineParams();
  const auto  request_before = executor_->CaptureOneShotRenderParams();
  const auto* exposure = document_->PrimaryGrade()->FindAdjustmentByType(type_ids::Exposure());
  for (const bool host : {false, true, false}) {
    SCOPED_TRACE(host);
    executor_->SetEnableCache(!host);
    executor_->SetDecodeRes(host ? DecodeRes::EIGHTH : DecodeRes::FULL);
    executor_->SetRenderRes(false, host ? 128 : 256);
    executor_->SetResizeDownsampleAlgorithm(ResizeDownsampleAlgorithm::Bilinear);
    ViewportRenderRegion viewport;
    viewport.reference_width_  = 1024;
    viewport.reference_height_ = 1024;
    viewport.x_                = 128;
    viewport.y_                = 128;
    viewport.scale_x_          = 0.5f;
    viewport.scale_y_          = 0.5f;
    executor_->SetRenderRegion(128, 128, 0.5f, 0.5f, 1024, 1024);
    executor_->SetRenderRequestViewport(viewport);
    const auto pixels = Render(host);
    ASSERT_FALSE(pixels.empty());
    EXPECT_TRUE(cv::checkRange(pixels));
    EXPECT_EQ(document_->ToJson(), before);
    EXPECT_EQ(executor_->ExportPipelineParams(), stages_before);
    EXPECT_EQ(document_->PrimaryGrade()->FindAdjustmentByType(type_ids::Exposure()), exposure);
  }
  executor_->RestoreOneShotRenderParams(request_before);
  EXPECT_EQ(document_->ToJson(), before);
  EXPECT_EQ(executor_->ExportPipelineParams(), stages_before);
}

TEST_F(PipelineDocumentRenderTest, DefaultDocumentRendersRealRawAtFullDecodeAndOutputResolution) {
  executor_->SetRenderRes(true);
  const auto before = document_->ToJson();
  const auto pixels = Render(true);
  ASSERT_FALSE(pixels.empty());
  EXPECT_EQ(pixels.type(), CV_32FC4);
  EXPECT_EQ(pixels.size(), full_extent_);
  EXPECT_TRUE(cv::checkRange(pixels));
  EXPECT_GT(cv::mean(pixels)[1], 0.01);
  EXPECT_EQ(executor_->CaptureOneShotRenderParams().decode_res_, DecodeRes::FULL);
  EXPECT_EQ(document_->ToJson(), before);
  const auto stats = executor_->DebugCudaRenderer()->Stats();
  EXPECT_EQ(stats.libraw_open_unpack_count, 1u);
  EXPECT_EQ(stats.pass.sensor_develop_execute, 1u);
  EXPECT_EQ(stats.pass.camera_color_execute, 1u);
  EXPECT_EQ(stats.pass.primary_grade_execute, 1u);
  EXPECT_EQ(stats.pass.drt_execute, 1u);
}

TEST_F(PipelineDocumentRenderTest, MissingDocumentFailsWithoutExecutingStages) {
  CPUPipelineExecutor unbound;
  unbound.SetAcceleratorBackendPreference(AcceleratorBackendPreference::CUDA);
  unbound.SetExecutionStages(&sink_);
  const auto before = unbound.ExportPipelineParams();
  for (const bool host : {false, true}) {
    unbound.SetForceCPUOutput(host);
    try {
      (void)unbound.Apply(input_);
      FAIL() << "Missing document must fail";
    } catch (const std::runtime_error& error) {
      EXPECT_NE(std::string(error.what()).find("bound PipelineDocument"), std::string::npos);
    }
  }
  EXPECT_EQ(unbound.DebugCudaRenderer(), nullptr);
  EXPECT_EQ(unbound.ExportPipelineParams(), before);
  EXPECT_EQ(sink_.ready_count, 0);
  EXPECT_EQ(sink_.host_frame_count, 0);
  EXPECT_TRUE(input_->buffer_valid_);
  EXPECT_FALSE(input_->cpu_data_valid_);
}

TEST_F(PipelineDocumentRenderTest, FailedGpuPresentationPropagatesErrorWithoutSubstituteOutput) {
  SetExposure(0.5f);
  const auto before    = document_->ToJson();
  sink_.reject_mapping = true;
  try {
    (void)Render(false);
    FAIL() << "Rejected CUDA presentation must fail";
  } catch (const std::runtime_error& error) {
    EXPECT_NE(std::string(error.what()).find("rejected the write mapping"), std::string::npos);
  }
  EXPECT_EQ(sink_.ready_count, 0);
  EXPECT_EQ(sink_.host_frame_count, 0);
  EXPECT_EQ(document_->ToJson(), before);
  EXPECT_EQ(executor_->CaptureOneShotRenderParams().decode_res_, DecodeRes::FULL);
  sink_.reject_mapping = false;
  const auto pixels    = Render(false);
  EXPECT_LT(cv::norm(pixels, Reference(0.5f, RenderQuality::Preview), cv::NORM_INF), 2e-5);
  EXPECT_EQ(sink_.ready_count, 1);
}

TEST_F(PipelineDocumentRenderTest, RebindingExecutorDoesNotOverwriteLoadedCameraProfileOrEdits) {
  auto loaded = std::make_shared<PipelineDocument>(PipelineDocument::FromJson(document_->ToJson()));
  auto payload = loaded->Develop()->Params().Params();
  payload.camera_profile.color_matrix_1[0] += 0.1;
  payload.wb_mode    = "custom";
  payload.custom_cct = 4800;
  loaded->Develop()->Params().ReplaceParams(payload);
  const auto before = loaded->ToJson();
  executor_->SetPipelineDocument(loaded);
  EXPECT_EQ(loaded->ToJson(), before);
  EXPECT_EQ(executor_->GpuDagDocument(), loaded);
  EXPECT_THROW(executor_->SetPipelineDocument(nullptr), std::invalid_argument);
  EXPECT_EQ(executor_->GpuDagDocument(), loaded);
}

TEST_F(PipelineDocumentRenderTest, MissingCameraProfileFailsWithoutReadingStageMetadata) {
  // The import-stage metadata remains valid; only the authoritative document is invalid.
  auto develop                                = document_->Develop()->Params().Params();
  develop.camera_profile.color_matrices_valid = false;
  document_->Develop()->Params().ReplaceParams(develop);
  const auto before = document_->ToJson();
  EXPECT_THROW((void)Render(true), std::runtime_error);
  EXPECT_EQ(document_->ToJson(), before);
  EXPECT_EQ(executor_->CaptureOneShotRenderParams().decode_res_, DecodeRes::FULL);
  EXPECT_TRUE(input_->buffer_valid_);
  EXPECT_FALSE(input_->cpu_data_valid_);
  EXPECT_EQ(sink_.ready_count, 0);
  EXPECT_GT(executor_->DebugCudaRenderer()->Stats().pass.sensor_develop_execute, 0u);
}

TEST_F(PipelineDocumentRenderTest, CpuPreferenceFailsInsteadOfExecutingLegacyStages) {
  executor_->SetAcceleratorBackendPreference(AcceleratorBackendPreference::CPU);
  const auto before = document_->ToJson();
  try {
    (void)Render(true);
    FAIL() << "Product rendering requires a supported GPU backend";
  } catch (const std::runtime_error& error) {
    EXPECT_NE(std::string(error.what()).find("supported GPU backend"), std::string::npos);
  }
  EXPECT_EQ(executor_->DebugCudaRenderer(), nullptr);
  EXPECT_EQ(document_->ToJson(), before);
  EXPECT_TRUE(input_->buffer_valid_);
  EXPECT_FALSE(input_->cpu_data_valid_);
}

}  // namespace
}  // namespace alcedo
