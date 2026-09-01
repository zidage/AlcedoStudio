//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>
#include <opencv2/core.hpp>

#include "../graph/test_camera_profile.hpp"
#include "../input/prepared_raw_test_support.hpp"
#include "edit/graph/color_grade_node_model.hpp"
#include "edit/graph/drt_node_model.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/graph/pipeline_graph_commands.hpp"
#include "edit/input/raw_input_loader.hpp"
#include "edit/operators/models/adjustment_catalog.hpp"
#include "edit/operators/models/builtin_type_ids.hpp"
#include "edit/operators/models/scalar_operator_model.hpp"
#include "edit/operators/models/sharpen_model.hpp"
#include "edit/runtime/renderer.hpp"
#include "image/image_buffer.hpp"
#include "io/image/export_color_profile_config.hpp"
#include "json.hpp"
#include "multi_grade_runtime_test_support.hpp"

namespace alcedo::nm2_qualification {

inline constexpr float kEditorMatchTol = 2.0e-5f;
inline constexpr float kExportDiffMin  = 1.0e-3f;

/**
 * @brief Synthetic encoded payload for product-renderer unpack stubs.
 *
 * The unpacker ignores bytes; the tag only distinguishes ImageBuffer identity.
 */
inline auto MakeEncodedImage(std::uint8_t tag) -> std::shared_ptr<ImageBuffer> {
  std::vector<std::uint8_t> bytes(64, tag);
  bytes[0] = tag;
  bytes[1] = 0x5A;
  return std::make_shared<ImageBuffer>(std::move(bytes));
}

inline auto MakeUnpacker() -> PreparedSourceCache::UnpackFn {
  return [](std::span<const std::byte>, DecodeRes decode_res) {
    const auto pattern = gpu_dag_test::MakeRggbPattern();
    return RawInputLoader::FromUnpackedCfa(gpu_dag_test::MakeU16CfaPlane(32, 32, pattern), pattern,
                                           gpu_dag_test::DefaultLinearization(),
                                           gpu_dag_test::FullSensor(32, 32), decode_res);
  };
}

/** @brief Encoding that differs from the default Rec.709 / Gamma 2.2 / 100 nits DRT. */
inline auto DistinctExportColor() -> ExportColorProfileConfig {
  return {ColorUtils::ColorSpace::REC2020, ColorUtils::EOTF::ST2084, 1000.0f};
}

inline void ConfigureThreeGradeLook(PipelineDocument& document) {
  gpu_dag_test::EnsureTestCameraProfile(document);
  multi_grade_test::ResetGradeLookToIdentity(*document.PrimaryGrade());
  multi_grade_test::AddCleanGradesBeforeDrt(document, {"grade.b", "grade.c"});
  multi_grade_test::GradeAdjustment<ExposureModel>(document, NodeId{"grade.primary"},
                                                  type_ids::Exposure())
      .SetValue(0.75f);
  multi_grade_test::GradeAdjustment<ContrastModel>(document, NodeId{"grade.b"}, type_ids::Contrast())
      .SetValue(40.0f);
  multi_grade_test::GradeAdjustment<ExposureModel>(document, NodeId{"grade.c"}, type_ids::Exposure())
      .SetValue(0.5f);
  auto* clarity = dynamic_cast<ClarityModel*>(
      document.Drt()->FindAdjustmentByType(type_ids::Clarity()));
  auto* sharpen = dynamic_cast<SharpenModel*>(
      document.Drt()->FindAdjustmentByType(type_ids::Sharpen()));
  if (clarity == nullptr || sharpen == nullptr) {
    throw std::runtime_error("nm2_qualification: DRT/Post is missing Clarity or Sharpen");
  }
  clarity->SetValue(25.0f);
  sharpen->SetAmount(12.0f);
}

inline auto MakeThreeGradeDocument() -> std::shared_ptr<PipelineDocument> {
  auto document = std::make_shared<PipelineDocument>(CreateDefaultPipelineDocument());
  ConfigureThreeGradeLook(*document);
  return document;
}

inline void ExpectOwnersAndEdges(const PipelineDocument& document) {
  EXPECT_EQ(document.Graph().ImageBackboneNodeIds(),
            (std::vector<NodeId>{NodeId{"develop"}, NodeId{"grade.primary"}, NodeId{"grade.b"},
                                 NodeId{"grade.c"}, NodeId{"drt"}}));
  const auto* primary = document.PrimaryGrade();
  const auto* grade_b =
      dynamic_cast<const ColorGradeNodeModel*>(document.Graph().FindNode(NodeId{"grade.b"}));
  const auto* grade_c =
      dynamic_cast<const ColorGradeNodeModel*>(document.Graph().FindNode(NodeId{"grade.c"}));
  ASSERT_NE(primary, nullptr);
  ASSERT_NE(grade_b, nullptr);
  ASSERT_NE(grade_c, nullptr);
  ASSERT_NE(document.Drt(), nullptr);
  EXPECT_EQ(primary->FindAdjustmentByType(type_ids::Clarity()), nullptr);
  EXPECT_EQ(grade_b->FindAdjustmentByType(type_ids::Clarity()), nullptr);
  EXPECT_NE(document.Drt()->FindAdjustmentByType(type_ids::Clarity()), nullptr);
  EXPECT_NE(document.Drt()->FindAdjustmentByType(type_ids::Sharpen()), nullptr);
  const auto* exposure = dynamic_cast<const ExposureModel*>(
      primary->FindAdjustmentByType(type_ids::Exposure()));
  const auto* contrast = dynamic_cast<const ContrastModel*>(
      grade_b->FindAdjustmentByType(type_ids::Contrast()));
  const auto* clarity = dynamic_cast<const ClarityModel*>(
      document.Drt()->FindAdjustmentByType(type_ids::Clarity()));
  ASSERT_NE(exposure, nullptr);
  ASSERT_NE(contrast, nullptr);
  ASSERT_NE(clarity, nullptr);
  EXPECT_FLOAT_EQ(exposure->Value(), 0.75f);
  EXPECT_FLOAT_EQ(contrast->Value(), 40.0f);
  EXPECT_FLOAT_EQ(clarity->Value(), 25.0f);
}

inline auto InjectClarityOnFirstGrade(nlohmann::json json) -> nlohmann::json {
  auto default_clarity = BuiltinAdjustmentCatalog::Instance().CreateDefault(type_ids::Clarity());
  for (auto& node : json.at("nodes")) {
    if (node.at("id") != "grade.primary") {
      continue;
    }
    node["adjustments"].push_back({{"id", "grade.primary.clarity"},
                                   {"type", std::string{type_ids::Clarity().Text()}},
                                   {"params", default_clarity->ToJson()}});
    return json;
  }
  throw std::runtime_error("nm2_qualification: grade.primary missing from document JSON");
}

inline auto HostIsFinite(const std::shared_ptr<ImageBuffer>& image) -> bool {
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

inline auto HostMaxAbsError(ImageBuffer& lhs, ImageBuffer& rhs) -> float {
  const auto& a = lhs.GetCPUData();
  const auto& b = rhs.GetCPUData();
  if (a.size() != b.size() || a.type() != b.type()) {
    return INFINITY;
  }
  return static_cast<float>(cv::norm(a, b, cv::NORM_INF));
}

template <class Renderer>
auto RenderSession(Renderer& renderer, const std::shared_ptr<ImageBuffer>& image)
    -> std::shared_ptr<ImageBuffer> {
  return renderer.Render(image, DecodeRes::FULL, RenderRequest{}, nullptr,
                         FrameCompletionSubmission{}, true);
}

template <class Renderer>
auto RenderOneShot(Renderer& renderer, const std::shared_ptr<ImageBuffer>& image,
                   const std::optional<ExportColorProfileConfig>& output_color = {})
    -> std::shared_ptr<ImageBuffer> {
  return renderer.Render(image, DecodeRes::FULL, RenderRequest{}, nullptr,
                         FrameCompletionSubmission{}, true, RenderCachePolicy::BypassSessionCache,
                         output_color);
}

template <class Renderer>
void PrintResourceSnapshot(std::string_view label, Renderer& renderer, std::uint64_t grade_execute,
                           double ms) {
  renderer.Device().WaitIdle();
  const auto session = renderer.SessionResources();
  const auto& params = renderer.Device().Workspace().Parameters();
  const auto completed = renderer.Device().Workspace().Device().CompletedSubmission();
  std::cout << "NM2.5 " << label << " grade_execute=" << grade_execute
            << " session_pool_bytes=" << session.texture_pool_used_bytes
            << " published=" << session.published_result_count
            << " param_slots=" << params.SlotCount() << " param_bytes=" << params.used_bytes()
            << " completed=" << completed << " ms=" << ms << '\n';
  if constexpr (requires(const Renderer& r) {
                  r.Device().Workspace().Device().QueryDeviceMemory();
                }) {
    const auto gpu = renderer.Device().Workspace().Device().QueryDeviceMemory();
    if (gpu.valid) {
      const auto used =
          gpu.total_bytes > gpu.free_bytes ? gpu.total_bytes - gpu.free_bytes : 0;
      std::cout << "NM2.5 " << label << " device_used_bytes=" << used
                << " (not required to equal pool bytes)\n";
    }
  }
}

/**
 * @brief Export overlay must change pixels without writing DRT/Post on the live document.
 */
template <class Renderer>
void ExportRecipeDoesNotChangeNextEditorRender(Renderer& renderer, PipelineDocument& document,
                                               const std::shared_ptr<ImageBuffer>& image) {
  const auto document_before = document.ToJson();
  const auto editor          = RenderSession(renderer, image);
  ASSERT_TRUE(HostIsFinite(editor));
  const auto session_before = renderer.SessionResources();
  EXPECT_GT(session_before.published_result_count, 0U);
  renderer.ResetStats();

  const auto exported = RenderOneShot(renderer, image, DistinctExportColor());
  ASSERT_TRUE(HostIsFinite(exported));
  EXPECT_EQ(document.ToJson(), document_before);
  EXPECT_GT(HostMaxAbsError(*editor, *exported), kExportDiffMin);
  EXPECT_EQ(renderer.OneShotPublishedResultCount(), 0U);
  EXPECT_EQ(renderer.OneShotResources().texture_pool_used_bytes, 0U);
  EXPECT_EQ(renderer.SessionResources().published_result_count,
            session_before.published_result_count);
  EXPECT_EQ(renderer.Stats().prepared_source_hits, 0U);

  renderer.ResetStats();
  const auto editor_again = RenderSession(renderer, image);
  ASSERT_TRUE(HostIsFinite(editor_again));
  EXPECT_EQ(document.ToJson(), document_before);
  EXPECT_LT(HostMaxAbsError(*editor, *editor_again), kEditorMatchTol);
  EXPECT_EQ(renderer.Stats().pass.sensor_develop_skip, 1U);
  EXPECT_EQ(renderer.Stats().pass.drt_skip, 1U);
}

/**
 * @brief JSON reopen keeps owners and edges; loaded document renders the same pixels.
 */
template <class Renderer, class MakeRenderer>
void MultiGradeDocumentRoundTripPreservesOwnersAndEdges(
    Renderer& renderer, PipelineDocument& document, const std::shared_ptr<ImageBuffer>& image,
    MakeRenderer make_renderer) {
  ExpectOwnersAndEdges(document);
  const auto original = RenderSession(renderer, image);
  ASSERT_TRUE(HostIsFinite(original));

  const auto json     = document.ToJson();
  EXPECT_FALSE(json.contains("stages"));
  auto loaded = std::make_shared<PipelineDocument>(PipelineDocument::FromJson(json));
  ExpectOwnersAndEdges(*loaded);

  auto loaded_renderer = make_renderer(loaded);
  const auto restored  = RenderSession(*loaded_renderer, image);
  ASSERT_TRUE(HostIsFinite(restored));
  EXPECT_LT(HostMaxAbsError(*original, *restored), kEditorMatchTol);

  EXPECT_THROW((void)PipelineDocument::FromJson(InjectClarityOnFirstGrade(json)),
               std::runtime_error);
  ExpectOwnersAndEdges(*loaded);
}

/**
 * @brief Thumbnail/export ExactRelease must not drop editor session results.
 */
template <class Renderer>
void BackgroundMultiGradeRenderPreservesEditorCache(Renderer& renderer,
                                                    const std::shared_ptr<ImageBuffer>& image) {
  ASSERT_TRUE(HostIsFinite(RenderSession(renderer, image)));
  const auto resources_before = renderer.SessionResources();
  EXPECT_GT(resources_before.published_result_count, 0U);
  renderer.ResetStats();

  ASSERT_TRUE(HostIsFinite(RenderOneShot(renderer, image)));
  EXPECT_EQ(renderer.OneShotPublishedResultCount(), 0U);
  EXPECT_EQ(renderer.OneShotResources().texture_pool_used_bytes, 0U);
  EXPECT_EQ(renderer.OneShotResources().transient_slab_count, 0U);
  EXPECT_EQ(renderer.SessionResources().published_result_count,
            resources_before.published_result_count);
  EXPECT_EQ(renderer.SessionResources().prepared_source_entry_count,
            resources_before.prepared_source_entry_count);

  renderer.ResetStats();
  ASSERT_TRUE(HostIsFinite(RenderSession(renderer, image)));
  EXPECT_EQ(renderer.Stats().pass.sensor_develop_skip, 1U);
  EXPECT_EQ(renderer.Stats().pass.drt_skip, 1U);
}

/**
 * @brief Measure 1/2/3 Grades, wait for GPU completion, then reclaim after removal.
 *
 * Pool used-bytes are the logical retain set. Device QueryDeviceMemory is recorded
 * and must not be treated as equal to those bytes.
 */
template <class Renderer>
void MultiGradeResourceBytesAfterGpuCompletion(Renderer& renderer, PipelineDocument& document,
                                               const std::shared_ptr<ImageBuffer>& image) {
  const auto time_render = [&](std::string_view label) {
    renderer.ResetStats();
    const auto start = std::chrono::steady_clock::now();
    ASSERT_TRUE(HostIsFinite(RenderSession(renderer, image)));
    const auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() -
                                                              start)
                        .count();
    renderer.Device().WaitIdle();
    EXPECT_FALSE(renderer.Device().Workspace().Device().HasInFlightSubmission());
    EXPECT_GE(renderer.Device().Workspace().Device().CompletedSubmission(),
              renderer.Device().CommandContext().SubmissionId());
    PrintResourceSnapshot(label, renderer, renderer.Stats().pass.primary_grade_execute, ms);
  };

  time_render("grades=1");
  const auto one_published = renderer.SessionResources().published_result_count;
  const auto one_pool      = renderer.SessionResources().texture_pool_used_bytes;
  const auto one_slots     = renderer.Device().Workspace().Parameters().SlotCount();
  EXPECT_EQ(renderer.Stats().pass.primary_grade_execute, 1U);
  EXPECT_GT(one_published, 0U);
  EXPECT_GT(one_pool, 0U);

  multi_grade_test::AddCleanGradesBeforeDrt(document, {"grade.b"});
  time_render("grades=2");
  EXPECT_EQ(renderer.Stats().pass.primary_grade_execute, 1U);
  EXPECT_GE(renderer.Stats().pass.primary_grade_skip, 1U);
  EXPECT_GE(renderer.SessionResources().published_result_count, one_published);
  EXPECT_GE(renderer.SessionResources().texture_pool_used_bytes, one_pool);

  multi_grade_test::AddCleanGradesBeforeDrt(document, {"grade.c"});
  multi_grade_test::GradeAdjustment<ContrastModel>(document, NodeId{"grade.b"},
                                                  type_ids::Contrast())
      .SetValue(40.0f);
  time_render("grades=3");
  EXPECT_EQ(renderer.Stats().pass.primary_grade_execute, 2U);
  EXPECT_GE(renderer.Stats().pass.primary_grade_skip, 1U);
  const auto three_slots = renderer.Device().Workspace().Parameters().SlotCount();
  EXPECT_GT(three_slots, one_slots);

  renderer.ResetStats();
  multi_grade_test::GradeAdjustment<ContrastModel>(document, NodeId{"grade.b"},
                                                  type_ids::Contrast())
      .SetValue(80.0f);
  ASSERT_TRUE(HostIsFinite(RenderSession(renderer, image)));
  EXPECT_GE(renderer.Stats().pass.primary_grade_skip, 1U);
  EXPECT_GE(renderer.Stats().pass.primary_grade_execute, 1U);

  renderer.ResetStats();
  ASSERT_TRUE(HostIsFinite(RenderSession(renderer, image)));
  EXPECT_EQ(renderer.Stats().pass.primary_grade_execute, 0U);
  EXPECT_EQ(renderer.Stats().pass.drt_skip, 1U);

  const auto session_before_oneshot = renderer.SessionResources();
  ASSERT_TRUE(HostIsFinite(RenderOneShot(renderer, image)));
  renderer.Device().WaitIdle();
  EXPECT_EQ(renderer.OneShotResources().texture_pool_used_bytes, 0U);
  EXPECT_EQ(renderer.OneShotResources().published_result_count, 0U);
  EXPECT_EQ(renderer.SessionResources().published_result_count,
            session_before_oneshot.published_result_count);

  ASSERT_TRUE(RemoveColorGradeAndBridge(document, NodeId{"grade.c"}).empty());
  ASSERT_TRUE(RemoveColorGradeAndBridge(document, NodeId{"grade.b"}).empty());
  time_render("grades=1_after_removal");
  EXPECT_GE(renderer.Stats().pass.primary_grade_skip, 1U);
  EXPECT_EQ(renderer.Stats().pass.primary_grade_execute, 0U);
  EXPECT_LT(renderer.Device().Workspace().Parameters().SlotCount(), three_slots);

  renderer.ReleaseSessionCaches();
  renderer.Device().WaitIdle();
  const auto released = renderer.SessionResources();
  EXPECT_EQ(released.published_result_count, 0U);
  EXPECT_EQ(released.texture_pool_used_bytes, 0U);
  EXPECT_EQ(released.texture_pool_entry_count, 0U);
  EXPECT_TRUE(released.session_value_ids.empty());
}

}  // namespace alcedo::nm2_qualification
