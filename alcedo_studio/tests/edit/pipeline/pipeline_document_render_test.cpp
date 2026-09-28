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
#include <optional>
#include <stdexcept>
#include <vector>

#include "edit/graph/develop_color_transform.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/graph/pipeline_graph_commands.hpp"
#include "edit/graph/pipeline_graph_snapshot.hpp"
#include "edit/pipeline/pipeline_executor.hpp"
#include "edit/runtime/cuda/cuda_product_renderer.hpp"
#include "edit/runtime/executor_role.hpp"
#include "edit/runtime/pipeline_apply_request.hpp"
#include "image/dng_color_profile_import.hpp"
#include "image/metadata_extractor.hpp"
#include "support/render_snapshot_source.hpp"

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
    ASSERT_TRUE(imported_.dng_profile_.IsBound());
    ASSERT_TRUE(imported_.color_matrices_valid_);
    input_    = std::make_shared<ImageBuffer>(std::move(bytes));
    document_ = std::make_shared<PipelineDocument>(CreateDefaultPipelineDocument());
    BindImportedCameraProfile(*document_, imported_);
    executor_ = std::make_unique<PipelineExecutor>();
    executor_->SetAcceleratorBackendPreference(AcceleratorBackendPreference::CUDA);
    source_ = std::make_unique<test::RenderSnapshotSource>(document_);
  }

  /** @brief Apply an exposure edit to the working document that every render freezes. */
  void SetExposure(float ev) {
    document_->PrimaryGrade()
        ->FindAdjustmentByType(type_ids::Exposure())
        ->LoadJson({{"exposure_ev", ev}});
  }

  /** @brief Build the request the scheduler sends for an editor (sink) or host render. */
  auto MakeRequest(bool host) -> PipelineApplyRequest {
    PipelineApplyRequest request;
    request.geometry.resolution.max_edge = max_edge_;
    request.geometry.resolution.quality  = host ? RenderQuality::Export : RenderQuality::Preview;
    if (visible_rect_.has_value()) {
      request.geometry.view.visible_rect_in_edit_space = *visible_rect_;
    }
    request.decode_res          = decode_res_;
    request.role                = role_;
    request.require_host_output = host;
    request.sink                = host ? nullptr : &sink_;
    return request;
  }

  /** @brief Execute with the same exclusive access required by the scheduler. */
  auto Render(bool host) -> cv::Mat {
    std::unique_lock lock(executor_->GetRenderLock());
    const auto       snapshot = source_->Freeze();
    const auto       result   = executor_->Apply(*snapshot, input_, MakeRequest(host));
    if (!result) throw std::runtime_error("Missing render result");
    return host ? result->GetCPUData().clone() : sink_.pixels.clone();
  }

  /** @brief Binding the interactive renderer holds after rendering the fixture's source. */
  auto ExpectedBinding() const -> std::optional<RenderBindingKey> {
    return RenderBindingKey{.lineage = source_->Lineage(), .element_id = source_->ElementId()};
  }

  /** @brief Render a separately initialized reference through a standalone Renderer. */
  auto Reference(float ev, RenderQuality quality = RenderQuality::Export) -> cv::Mat {
    auto document = std::make_shared<PipelineDocument>(CreateDefaultPipelineDocument());
    auto develop  = document->Develop()->Params().Params();
    BindDevelopCameraProfile(develop, imported_);
    document->Develop()->Params().ReplaceParams(develop);
    document->PrimaryGrade()
        ->FindAdjustmentByType(type_ids::Exposure())
        ->LoadJson({{"exposure_ev", ev}});
    CudaRenderer  renderer(ExecutorRole::Interactive);
    RenderRequest request;
    request.resolution.max_edge = 256;
    request.resolution.quality  = quality;
    const auto snapshot         = test::FreezeInNewLineage(*document);
    return renderer.Render(*snapshot, input_, DecodeRes::FULL, request, nullptr, {}, true)
        ->GetCPUData()
        .clone();
  }

  DecodeRes                                   decode_res_ = DecodeRes::FULL;
  std::uint32_t                               max_edge_   = 256;
  ExecutorRole                                role_       = ExecutorRole::Interactive;
  std::optional<NormalizedRect>               visible_rect_;
  RawRuntimeColorContext                      imported_;
  std::shared_ptr<ImageBuffer>                input_;
  std::shared_ptr<PipelineDocument>           document_;
  std::unique_ptr<test::RenderSnapshotSource> source_;
  std::unique_ptr<PipelineExecutor>           executor_;
  PixelFrameSink                              sink_;
  cv::Size                                    full_extent_;
};

TEST_F(PipelineDocumentRenderTest, EditorAndHostRenderUseDocumentParameters) {
  SetExposure(-1.5f);
  const auto dark_reference = Reference(-1.5f);
  const auto dark_editor    = Render(false);
  const auto dark_host      = Render(true);
  ASSERT_EQ(dark_editor.size(), dark_reference.size());
  ASSERT_EQ(dark_host.type(), dark_reference.type());
  EXPECT_LT(cv::norm(dark_editor, Reference(-1.5f, RenderQuality::Preview), cv::NORM_INF), 2e-5);
  EXPECT_LT(cv::norm(dark_host, dark_reference, cv::NORM_INF), 2e-5);

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
  ASSERT_NE(executor_->DebugCudaRenderer(), nullptr);
  EXPECT_EQ(executor_->DebugCudaRenderer()->Binding(), ExpectedBinding());
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

TEST_F(PipelineDocumentRenderTest,
       HostBatchRendersReuseBatchDeviceAndLeaveInteractiveCacheUntouched) {
  const auto editor   = Render(false);
  auto*      renderer = executor_->DebugCudaRenderer();
  ASSERT_NE(renderer, nullptr);
  renderer->ResetStats();
  const auto session_before = renderer->Resources();
  EXPECT_GT(session_before.published_result_count, 0U);
  EXPECT_EQ(executor_->DebugCudaBatchRenderer(), nullptr);

  role_            = ExecutorRole::Batch;
  const auto host1 = Render(true);
  auto*      batch = executor_->DebugCudaBatchRenderer();
  ASSERT_NE(batch, nullptr);
  ASSERT_NE(batch, renderer);
  const auto batch_id        = batch->DebugDeviceIdentity();
  const auto batch_resources = batch->Resources();
  EXPECT_NE(batch_id, 0U);
  EXPECT_NE(batch_id, renderer->DebugDeviceIdentity());
  EXPECT_FALSE(batch->Binding().has_value());
  EXPECT_EQ(batch_resources.published_result_count, 0U);
  EXPECT_EQ(batch_resources.texture_pool_used_bytes, 0U);
  EXPECT_EQ(batch_resources.texture_pool_entry_count, 0U);
  EXPECT_EQ(batch_resources.prepared_source_entry_count, 0U);
  EXPECT_EQ(renderer->Stats().prepared_source_hits, 0U);
  EXPECT_EQ(renderer->Stats().prepared_source_misses, 0U);
  EXPECT_EQ(renderer->Stats().pass.sensor_develop_execute, 0U);
  EXPECT_EQ(renderer->Resources().published_result_count, session_before.published_result_count);
  EXPECT_EQ(renderer->Resources().prepared_source_entry_count,
            session_before.prepared_source_entry_count);
  EXPECT_EQ(renderer->Binding(), ExpectedBinding());
  EXPECT_LT(cv::norm(host1, Reference(1.5f, RenderQuality::Export), cv::NORM_INF), 2e-5);

  const auto host2 = Render(true);
  EXPECT_EQ(executor_->DebugCudaBatchRenderer(), batch);
  EXPECT_EQ(batch->DebugDeviceIdentity(), batch_id);
  EXPECT_EQ(batch->Resources().published_result_count, 0U);
  EXPECT_EQ(batch->Resources().texture_pool_used_bytes, 0U);
  EXPECT_LT(cv::norm(host1, host2, cv::NORM_INF), 2e-5);

  role_ = ExecutorRole::Interactive;
  renderer->ResetStats();
  const auto editor2 = Render(false);
  EXPECT_EQ(renderer->Stats().prepared_source_hits, 1U);
  EXPECT_EQ(renderer->Stats().libraw_open_unpack_count, 0U);
  EXPECT_EQ(renderer->Stats().pass.sensor_develop_execute, 0U);
  EXPECT_LT(cv::norm(editor, editor2, cv::NORM_INF), 2e-5);
}

TEST_F(PipelineDocumentRenderTest, RenderLeavesPersistentDocumentParametersUnchanged) {
  SetExposure(0.75f);
  const auto  before   = document_->ToJson();
  const auto* exposure = document_->PrimaryGrade()->FindAdjustmentByType(type_ids::Exposure());
  for (const bool host : {false, true, false}) {
    SCOPED_TRACE(host);
    role_             = host ? ExecutorRole::Batch : ExecutorRole::Interactive;
    decode_res_       = host ? DecodeRes::EIGHTH : DecodeRes::FULL;
    max_edge_         = host ? 128 : 256;
    visible_rect_     = NormalizedRect{0.125f, 0.125f, 0.5f, 0.5f};
    const auto pixels = Render(host);
    ASSERT_FALSE(pixels.empty());
    EXPECT_TRUE(cv::checkRange(pixels));
    EXPECT_EQ(document_->ToJson(), before);
    EXPECT_EQ(document_->PrimaryGrade()->FindAdjustmentByType(type_ids::Exposure()), exposure);
  }
  EXPECT_EQ(document_->ToJson(), before);
}

TEST_F(PipelineDocumentRenderTest, DefaultDocumentRendersRealRawAtFullDecodeAndOutputResolution) {
  max_edge_         = 0;
  const auto before = document_->ToJson();
  const auto pixels = Render(true);
  ASSERT_FALSE(pixels.empty());
  EXPECT_EQ(pixels.type(), CV_32FC4);
  EXPECT_EQ(pixels.size(), full_extent_);
  EXPECT_TRUE(cv::checkRange(pixels));
  EXPECT_GT(cv::mean(pixels)[1], 0.01);
  EXPECT_EQ(document_->ToJson(), before);
  const auto stats = executor_->DebugCudaRenderer()->Stats();
  EXPECT_EQ(stats.libraw_open_unpack_count, 1u);
  EXPECT_EQ(stats.pass.sensor_develop_execute, 1u);
  EXPECT_EQ(stats.pass.camera_color_execute, 1u);
  EXPECT_EQ(stats.pass.primary_grade_execute, 1u);
  EXPECT_EQ(stats.pass.drt_execute, 1u);
}

TEST_F(PipelineDocumentRenderTest, SnapshotWithoutDocumentOrLineageIsRejectedBeforeRendering) {
  // A render receives its graph as a snapshot, so a missing document is rejected when the
  // snapshot is formed and never reaches an executor.
  EXPECT_THROW((void)PipelineGraphSnapshot::Preview(nullptr, 1, PipelineLineageId::Next(),
                                                    transaction_chain_hash_t{}),
               std::invalid_argument);
  EXPECT_THROW((void)PipelineGraphSnapshot::Preview(document_->Freeze(), 1, PipelineLineageId{},
                                                    transaction_chain_hash_t{}),
               std::invalid_argument);
  EXPECT_EQ(executor_->DebugCudaRenderer(), nullptr);
  EXPECT_EQ(executor_->DebugCudaBatchRenderer(), nullptr);
  EXPECT_EQ(sink_.ready_count, 0);
  EXPECT_EQ(sink_.host_frame_count, 0);
  EXPECT_TRUE(input_->buffer_valid_);
  EXPECT_FALSE(input_->cpu_data_valid_);
}

TEST_F(PipelineDocumentRenderTest, ExecutorRejectsUnservedRoleWithoutRendering) {
  const auto snapshot = source_->Freeze();
  for (const auto served : {ExecutorRole::Interactive, ExecutorRole::Batch}) {
    SCOPED_TRACE(served == ExecutorRole::Interactive ? "interactive" : "batch");
    PipelineExecutor single_role(served);
    single_role.SetAcceleratorBackendPreference(AcceleratorBackendPreference::CUDA);
    single_role.AttachFrameSink(&sink_);
    role_ = served == ExecutorRole::Interactive ? ExecutorRole::Batch : ExecutorRole::Interactive;
    for (const bool host : {false, true}) {
      std::unique_lock lock(single_role.GetRenderLock());
      EXPECT_THROW((void)single_role.Apply(*snapshot, input_, MakeRequest(host)),
                   std::invalid_argument);
    }
    EXPECT_EQ(single_role.DebugCudaRenderer(), nullptr);
    EXPECT_EQ(single_role.DebugCudaBatchRenderer(), nullptr);
  }
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
  sink_.reject_mapping = false;
  const auto pixels    = Render(false);
  EXPECT_LT(cv::norm(pixels, Reference(0.5f, RenderQuality::Preview), cv::NORM_INF), 2e-5);
  EXPECT_EQ(sink_.ready_count, 1);
}

TEST_F(PipelineDocumentRenderTest, RenderingLoadedDocumentKeepsItsCameraProfileAndEdits) {
  (void)Render(false);
  const auto first_binding = ExpectedBinding();
  // A copy keeps the source DNG profile binding that rendering needs; FromJson alone does not.
  auto loaded = std::make_shared<PipelineDocument>(ClonePipelineDocument(*document_));
  auto payload = loaded->Develop()->Params().Params();
  payload.camera_profile.color_matrix_1[0] += 0.1;
  payload.wb_mode    = "custom";
  payload.custom_cct = 4800;
  loaded->Develop()->Params().ReplaceParams(payload);
  const auto before = loaded->ToJson();
  // A loaded document is a new history load, so it takes a new lineage and a new binding.
  source_->Rebind(loaded);
  const auto pixels = Render(true);
  ASSERT_FALSE(pixels.empty());
  EXPECT_EQ(loaded->ToJson(), before);
  EXPECT_EQ(&source_->Document(), loaded.get());
  EXPECT_EQ(executor_->DebugCudaRenderer()->Binding(), ExpectedBinding());
  EXPECT_NE(executor_->DebugCudaRenderer()->Binding(), first_binding);
}

TEST_F(PipelineDocumentRenderTest, MissingCameraProfileFailsWithoutSubstituteProfile) {
  // The imported RAW color data stays valid; only the authoritative document profile is invalid.
  auto develop                                = document_->Develop()->Params().Params();
  develop.camera_profile.color_matrices_valid = false;
  document_->Develop()->Params().ReplaceParams(develop);
  const auto before = document_->ToJson();
  EXPECT_THROW((void)Render(true), std::runtime_error);
  EXPECT_EQ(document_->ToJson(), before);
  EXPECT_TRUE(input_->buffer_valid_);
  EXPECT_FALSE(input_->cpu_data_valid_);
  EXPECT_EQ(sink_.ready_count, 0);
  EXPECT_GT(executor_->DebugCudaRenderer()->Stats().pass.sensor_develop_execute, 0u);
}

TEST_F(PipelineDocumentRenderTest, CleanTopInsertedMaskGroupLeavesPixelsUnchanged) {
  const auto before = Render(true);
  ASSERT_FALSE(before.empty());

  // Mask Groups top insertion inserts one clean Color Grade directly after
  // Develop; a clean grade is a full-image identity, so pixels cannot move.
  ASSERT_TRUE(
      alcedo::AddCleanColorGrade(*document_, NodeId{"grade.primary"}, NodeId{"grade.top"}).empty());
  const auto after = Render(true);
  ASSERT_EQ(after.size(), before.size());
  EXPECT_LT(cv::norm(before, after, cv::NORM_INF), 2e-5);
  EXPECT_EQ(executor_->DebugCudaRenderer()->Binding(), ExpectedBinding());
}

TEST_F(PipelineDocumentRenderTest, CpuPreferenceFailsWithoutSubstituteBackend) {
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
