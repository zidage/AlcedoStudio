//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

/// @file editor_comparison_large_raw_pair_test.cpp
/// @brief Resource and timing evidence of an editor comparison pair of a large Bayer RAW on the
///        editor's own CUDA executor: cold and warm pair, sensor reuse, output size, and the
///        approved SDR conversion.

#include <gtest/gtest.h>
#include <libraw/libraw.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <iterator>
#include <memory>
#include <opencv2/core.hpp>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "../support/editor_lease_test_support.hpp"
#include "app/editor_comparison_inputs.hpp"
#include "app/editor_comparison_types.hpp"
#include "app/editor_image_render_port.hpp"
#include "app/editor_working_document.hpp"
#include "app/pipeline_service.hpp"
#include "app/project_service.hpp"
#include "edit/graph/develop_color_transform.hpp"
#include "edit/graph/develop_node_model.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/pipeline/pipeline_accelerator.hpp"
#include "edit/pipeline/pipeline_executor.hpp"
#include "edit/runtime/pipeline_apply_request.hpp"
#include "image/image.hpp"
#include "image/image_buffer.hpp"
#include "image/metadata_extractor.hpp"
#include "renderer/pipeline_scheduler.hpp"
#include "ui/alcedo_main/album_backend/comparison_presentation_image.hpp"
#include "ui/alcedo_main/album_backend/editor_session_pipeline_port.hpp"
#include "ui/alcedo_main/album_backend/editor_session_render_scheduler_port.hpp"

#ifdef HAVE_CUDA
#include "edit/runtime/cuda/cuda_product_renderer.hpp"
#endif

namespace alcedo::ui {
namespace {

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

/// One large-RAW frame of a debug build can take minutes.
constexpr auto            kJobTimeout = 600s;
constexpr std::uint64_t   kEpoch      = 90;
constexpr sl_element_id_t kElementId  = 22;
constexpr image_id_t      kImageId    = 11;

auto Milliseconds(Clock::duration duration) -> double {
  return std::chrono::duration<double, std::milli>(duration).count();
}

/// Viewport frame sink that keeps the presented pixels in host memory.
class HostFrameSink final : public IFrameSink {
 public:
  void EnsureSize(int width, int height) override { pixels.create(height, width, CV_32FC4); }
  auto MapResourceForWrite(FrameMemoryDomain /*preferred_domain*/) -> FrameWriteMapping override {
    FrameWriteMapping mapping;
    mapping.data          = pixels.data;
    mapping.row_bytes     = pixels.step;
    mapping.pixel_format  = FramePixelFormat::RGBA32F;
    mapping.memory_domain = FrameMemoryDomain::HostVisible;
    mapping.target_type   = FrameWriteTargetType::LinearBuffer;
    return mapping;
  }
  void UnmapResource() override {}
  void NotifyFrameReady(const FrameCompletionSubmission& /*submission*/) override {}
  [[nodiscard]] auto GetWidth() const -> int override { return pixels.cols; }
  [[nodiscard]] auto GetHeight() const -> int override { return pixels.rows; }

  cv::Mat            pixels;
};

/**
 * @brief The editor render port of one large Bayer RAW on CUDA, with an in-memory history whose
 *        root has the imported camera profile bound.
 *
 * Skipped when CUDA is not compiled, no CUDA device exists, or the sample RAW is missing.
 */
class EditorComparisonLargeRawPairTest : public ::testing::Test {
 protected:
  void SetUp() override {
#ifndef HAVE_CUDA
    GTEST_SKIP() << "CUDA backend is not compiled.";
#else
    raw_path_ = std::filesystem::path(TEST_IMG_PATH) / "raw" / "camera" / "sony" / "a7rv" /
                "DSC00064.ARW";
    if (!std::filesystem::exists(raw_path_)) {
      GTEST_SKIP() << "Large RAW sample is missing: " << raw_path_.string();
    }
    try {
      (void)ResolveAcceleratorBackend(AcceleratorBackendPreference::CUDA);
    } catch (const std::exception& ex) {
      GTEST_SKIP() << "No CUDA device available: " << ex.what();
    }
    const auto stamp = std::to_string(Clock::now().time_since_epoch().count());
    db_path_         = std::filesystem::temp_directory_path() / ("comparison_raw_" + stamp + ".db");
    meta_path_ = std::filesystem::temp_directory_path() / ("comparison_raw_" + stamp + ".json");
    project_   = std::make_unique<ProjectService>(db_path_, meta_path_);
    service_   = std::make_shared<PipelineMgmtService>(project_->GetStorage(),
                                                       AcceleratorBackendPreference::CUDA);

    std::ifstream             stream(raw_path_, std::ios::binary);
    std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>(stream),
                                    std::istreambuf_iterator<char>()};
    ASSERT_FALSE(bytes.empty());
    auto raw = std::make_unique<LibRaw>();
    ASSERT_EQ(raw->open_buffer(bytes.data(), bytes.size()), LIBRAW_SUCCESS);
    source_width_  = raw->imgdata.sizes.raw_width;
    source_height_ = raw->imgdata.sizes.raw_height;
    RawRuntimeColorContext imported;
    MetadataExtractor::PopulateRuntimeContextFromOpenLibRaw(*raw, imported);
    ASSERT_TRUE(imported.color_matrices_valid_);
    auto root = CreateDefaultPipelineDocument();
    BindImportedCameraProfile(root, imported);
    input_      = std::make_shared<ImageBuffer>(std::move(bytes));

    auto shared = std::make_shared<PipelineDocument>(std::move(root));
    pipeline_   = std::make_shared<EditorSessionPipelinePort>();
    pipeline_->SetServices(EditorSessionPipelineMappers{
        [service = service_] { return service; }, [shared](sl_element_id_t id) {
          return test::MakeInMemoryEditorLease(id, ClonePipelineDocument(*shared));
        }});
    std::string error;
    lease_ = pipeline_->AcquireLease(kElementId, &error);
    ASSERT_TRUE(lease_.has_value()) << error;

    pipeline_scheduler_ = std::make_shared<PipelineScheduler>(1);
    port_               = std::make_shared<EditorSessionRenderSchedulerPort>(pipeline_scheduler_);
    port_->SetPipelinePort(pipeline_);
    port_->SetSinkResolver([this] { return static_cast<IFrameSink*>(&sink_); });
    EditorRenderSessionContext context;
    context.epoch                = kEpoch;
    context.element_id           = kElementId;
    context.image_id             = kImageId;
    context.presentation_sink_id = 7;
    context.image                = std::make_shared<Image>(kImageId);
    context.image->image_path_   = raw_path_;
    context.input                = input_;
    port_->InstallSessionContext(std::move(context));
#endif
  }

  void TearDown() override {
    port_.reset();
    pipeline_scheduler_.reset();
    lease_.reset();
    pipeline_.reset();
    service_.reset();
    project_.reset();
    std::error_code ec;
    if (!db_path_.empty()) {
      std::filesystem::remove(db_path_, ec);
      std::filesystem::remove(meta_path_, ec);
    }
  }

#ifdef HAVE_CUDA
  auto Working() -> EditorWorkingDocument& { return *lease_->document_; }
  auto Renderer() -> CudaRenderer* {
    const auto executor = port_->interactive_executor();
    return executor ? executor->DebugCudaRenderer() : nullptr;
  }

  /// Root and Current inputs from the production builder; Current has a different white balance.
  auto BuildRootAndCurrent() -> EditorComparisonInputPair {
    std::string error;
    auto pair = BuildEditorComparisonInputs(*lease_->graph_, *lease_->root_,
                                            pipeline_->CurrentPreview(kElementId), kElementId,
                                            EditorComparisonSource::Root(),
                                            EditorComparisonSource::Current(), &error);
    EXPECT_TRUE(pair.has_value()) << error;
    return pair.value_or(EditorComparisonInputPair{});
  }

  /// Render the pair as one image job and return its result and wall time.
  auto RenderPair(const EditorComparisonInputPair& inputs, Clock::duration* elapsed)
      -> std::optional<EditorImageRenderResult> {
    EditorImageRenderRequest request;
    request.element_id            = kElementId;
    request.image_id              = kImageId;
    request.image_load_request_id = ImageLoadRequestId{kEpoch};
    request.snapshots             = {inputs.a.snapshot, inputs.b.snapshot};
    auto        done              = std::make_shared<std::promise<EditorImageRenderResult>>();
    auto        result            = done->get_future();
    std::string error;
    const auto  start  = Clock::now();
    const auto  job_id = port_->ScheduleImages(
        std::move(request),
        [done](EditorImageRenderResult images) { done->set_value(std::move(images)); }, &error);
    EXPECT_NE(job_id, 0u) << error;
    if (job_id == 0 || result.wait_for(kJobTimeout) != std::future_status::ready) {
      ADD_FAILURE() << "The pair did not complete";
      return std::nullopt;
    }
    *elapsed = Clock::now() - start;
    return result.get();
  }

  /// Render one viewport frame of the current document.
  void RenderFrame(std::uint64_t request_id) {
    EditorRenderRequest request;
    request.request_id                   = request_id;
    request.intent.element_id            = kElementId;
    request.intent.image_id              = kImageId;
    request.intent.image_load_request_id = ImageLoadRequestId{kEpoch};
    request.intent.reason                = EditorRenderReason::InitialFrame;
    request.intent.quality               = EditorRenderQuality::Interactive;
    request.intent.frame_role            = FrameRole::InteractivePrimary;
    request.intent.requested_width       = 1280;
    request.intent.requested_height      = 853;
    request.intent.presentation_sink_id  = 7;
    auto       done   = std::make_shared<std::promise<bool>>();
    auto       result = done->get_future();
    const auto job_id = port_->Schedule(
        request, [done](bool success, std::string /*message*/) { done->set_value(success); });
    ASSERT_NE(job_id, 0u);
    ASSERT_EQ(result.wait_for(kJobTimeout), std::future_status::ready);
    ASSERT_TRUE(result.get());
  }
#endif

  std::filesystem::path                             raw_path_;
  std::filesystem::path                             db_path_;
  std::filesystem::path                             meta_path_;
  std::unique_ptr<ProjectService>                   project_;
  std::shared_ptr<PipelineMgmtService>              service_;
  std::shared_ptr<ImageBuffer>                      input_;
  std::shared_ptr<EditorSessionPipelinePort>        pipeline_;
  std::optional<EditorImageLease>                   lease_;
  std::shared_ptr<PipelineScheduler>                pipeline_scheduler_;
  std::shared_ptr<EditorSessionRenderSchedulerPort> port_;
  HostFrameSink                                     sink_;
  std::uint32_t                                     source_width_  = 0;
  std::uint32_t                                     source_height_ = 0;
};

TEST_F(EditorComparisonLargeRawPairTest, LargeRawPairReusesTheSensorResultAndRecordsTiming) {
#ifdef HAVE_CUDA
  auto develop          = Working().Document().Develop()->Params().Params();
  develop.use_camera_wb = false;
  develop.wb_mode       = "custom";
  develop.custom_cct    = 4300.0f;
  develop.custom_tint   = 8.0f;
  Working().Document().Develop()->Params().ReplaceParams(develop);
  (void)Working().PublishPreview();

  // Cold: the pair is the first work on the current binding.
  Clock::duration cold_elapsed{};
  const auto      cold = RenderPair(BuildRootAndCurrent(), &cold_elapsed);
  ASSERT_TRUE(cold.has_value());
  ASSERT_EQ(cold->status, EditorImageRenderStatus::Completed) << cold->message;
  auto* renderer = Renderer();
  ASSERT_NE(renderer, nullptr);
  const auto cold_stats = renderer->Stats();
  EXPECT_LE(cold_stats.libraw_open_unpack_count, 1u);
  EXPECT_LE(cold_stats.pass.sensor_develop_execute, 1u) << "at most one sensor computation";

  // Warm: a viewport frame, then the pair again.
  RenderFrame(901);
  ASSERT_FALSE(HasFatalFailure());
  const auto resources_before = renderer->Resources();
  renderer->ResetStats();
  Clock::duration warm_elapsed{};
  const auto      warm = RenderPair(BuildRootAndCurrent(), &warm_elapsed);
  ASSERT_TRUE(warm.has_value());
  ASSERT_EQ(warm->status, EditorImageRenderStatus::Completed) << warm->message;
  ASSERT_EQ(warm->images.size(), 2u);
  const auto warm_stats = renderer->Stats();
  const auto resources  = renderer->Resources();
  EXPECT_EQ(warm_stats.libraw_open_unpack_count, 0u);
  EXPECT_EQ(warm_stats.prepared_source_misses, 0u);
  EXPECT_EQ(warm_stats.pass.sensor_develop_execute, 0u);
  EXPECT_EQ(resources.published_result_count, resources_before.published_result_count);
  EXPECT_EQ(resources.prepared_source_entry_count, resources_before.prepared_source_entry_count);

  // The approved SDR conversion, as the GUI does it.
  const auto& a_geometry = warm->images[0].geometry;
  EXPECT_LE(std::max(a_geometry.render_extent.width, a_geometry.render_extent.height),
            kQualityBaseMaxLongEdge);
  const auto convert_start = Clock::now();
  const auto a             = ConvertRenderedImageForComparison(warm->images[0]);
  const auto b             = ConvertRenderedImageForComparison(warm->images[1]);
  const auto convert_ms    = Milliseconds(Clock::now() - convert_start);
  EXPECT_EQ(a.image.width(), static_cast<int>(a_geometry.render_extent.width));
  const auto sdr_bytes = static_cast<std::size_t>(a.image.sizeInBytes() + b.image.sizeInBytes());

  std::cout << "[comparison large RAW] source " << raw_path_.filename().string() << " "
            << source_width_ << "x" << source_height_ << ", CUDA, Debug build\n"
            << "[comparison large RAW] output " << a_geometry.render_extent.width << "x"
            << a_geometry.render_extent.height << ", reference "
            << a_geometry.full_reference_extent.width << "x"
            << a_geometry.full_reference_extent.height
            << "\n"
            << "[comparison large RAW] cold pair " << Milliseconds(cold_elapsed)
            << " ms (unpack " << cold_stats.libraw_open_unpack_count << ", sensor develop "
            << cold_stats.pass.sensor_develop_execute << ")\n"
            << "[comparison large RAW] warm pair " << Milliseconds(warm_elapsed)
            << " ms (unpack " << warm_stats.libraw_open_unpack_count << ", sensor develop "
            << warm_stats.pass.sensor_develop_execute << ", sensor skip "
            << warm_stats.pass.sensor_develop_skip << ")\n"
            << "[comparison large RAW] SDR conversion of both images " << convert_ms << " ms, "
            << sdr_bytes << " bytes of RGBA8\n"
            << "[comparison large RAW] prepared sources " << resources.prepared_source_entry_count
            << " (" << resources.prepared_source_host_bytes << " host bytes), published results "
            << resources.published_result_count << ", transient used "
            << resources.transient_used_bytes << " of " << resources.transient_capacity_bytes
            << " bytes\n";
  RecordProperty("cold_pair_ms", std::to_string(Milliseconds(cold_elapsed)));
  RecordProperty("warm_pair_ms", std::to_string(Milliseconds(warm_elapsed)));
  RecordProperty("sdr_conversion_ms", std::to_string(convert_ms));
#endif
}

}  // namespace
}  // namespace alcedo::ui
