//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// LUT color encodings (lut_color_encoding_plan.md, L4): a LUT with non-default input and output
// encodings is committed through the editor history and stored. The stored history is read back
// (as reopening the project does), and the editor preview, the thumbnail and the export of that
// state show the same image, which differs from the same LUT with the default encodings.

#include <OpenImageIO/imageio.h>
#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "app/editor_working_document.hpp"
#include "app/export_service.hpp"
#include "app/pipeline_history_applier.hpp"
#include "app/pipeline_service.hpp"
#include "app/project_service.hpp"
#include "app/thumbnail_service.hpp"
#include "edit/graph/color_grade_node_model.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/graph/pipeline_graph_snapshot.hpp"
#include "edit/history/commit_graph.hpp"
#include "edit/history/edit_commit.hpp"
#include "edit/history/pipeline_edit_batch.hpp"
#include "edit/operators/models/builtin_type_ids.hpp"
#include "edit/operators/models/lmt_model.hpp"
#include "edit/pipeline/pipeline_executor.hpp"
#include "edit/runtime/executor_role.hpp"
#include "edit/runtime/pipeline_apply_request.hpp"
#include "image/image_buffer.hpp"
#include "io/image/export_recipe.hpp"
#include "json.hpp"
#include "renderer/pipeline_task.hpp"
#include "support/raw_import_pipeline_fixture.hpp"
#include "type/supported_file_type.hpp"
#include "type/type.hpp"
#include "utils/clock/time_provider.hpp"

namespace alcedo {
namespace {
using namespace std::chrono_literals;
using raw_import_test::HostPixels;
using raw_import_test::ImportLinearDng;
using raw_import_test::LinearDngPath;
using raw_import_test::LoadEncodedInput;

/// 8-bit mean absolute difference up to which two render paths show the same state. The paths
/// decode at different resolutions and resample differently; JPEG adds its own error.
constexpr double      kSameStateDifference     = 3.0;
/// 8-bit mean absolute difference above which two images show different LUT encodings.
constexpr double      kDistinctStateDifference = 6.0;

constexpr const char* kInputEncoding           = "sony_slog3_sgamut3cine";
constexpr const char* kOutputEncoding          = "rec709_bt1886";

/// 8-bit RGB of @p pixels: float values are display code values in [0, 1]; four channels are RGBA.
auto                  ToRgb8(const cv::Mat& pixels) -> cv::Mat {
  if (pixels.empty() || (pixels.channels() != 3 && pixels.channels() != 4)) {
    return {};
  }
  cv::Mat eight_bit;
  if (pixels.depth() == CV_32F) {
    pixels.convertTo(eight_bit, CV_MAKETYPE(CV_8U, pixels.channels()), 255.0);
  } else {
    eight_bit = pixels;
  }
  if (eight_bit.channels() == 3) {
    return eight_bit.clone();
  }
  cv::Mat rgb;
  cv::cvtColor(eight_bit, rgb, cv::COLOR_RGBA2RGB);
  return rgb;
}

/// @p pixels resampled to @p size by area averaging.
auto ResizedTo(const cv::Mat& pixels, const cv::Size& size) -> cv::Mat {
  if (pixels.empty() || pixels.size() == size) {
    return pixels;
  }
  cv::Mat resized;
  cv::resize(pixels, resized, size, 0.0, 0.0, cv::INTER_AREA);
  return resized;
}

auto MeanAbsoluteDifference(const cv::Mat& a, const cv::Mat& b) -> double {
  if (a.empty() || b.empty() || a.size() != b.size() || a.type() != b.type()) {
    return std::numeric_limits<double>::infinity();
  }
  cv::Mat a64;
  cv::Mat b64;
  a.convertTo(a64, CV_64F);
  b.convertTo(b64, CV_64F);
  const auto per_channel = cv::mean(cv::abs(a64 - b64));
  double     sum         = 0.0;
  for (int c = 0; c < a.channels(); ++c) {
    sum += per_channel[c];
  }
  return sum / a.channels();
}

/// 2^3 cube that compresses and tints every channel, so a wrong encoding changes the image.
void WriteLookCube(const std::filesystem::path& path) {
  std::ofstream out(path, std::ios::trunc);
  out << "LUT_3D_SIZE 2\n";
  for (int b = 0; b <= 1; ++b) {
    for (int g = 0; g <= 1; ++g) {
      for (int r = 0; r <= 1; ++r) {
        out << 0.08 + 0.80 * r << ' ' << 0.05 + 0.85 * g << ' ' << 0.12 + 0.70 * b << '\n';
      }
    }
  }
}

/// Record @p after as the LMT state of the primary grade: one SetParameter commit on the lease
/// graph, applied to the working document through the `lut` field write, as a settled LUT panel
/// change does.
void CommitLmt(EditorHistoryLease& lease, EditorWorkingDocument& working,
               const nlohmann::json& after) {
  auto&       document = working.Document();
  const auto* lmt_id   = document.PrimaryGrade()->FindAdjustmentIdByType(type_ids::Lmt());
  if (lmt_id == nullptr) {
    throw std::runtime_error("the primary grade has no LMT adjustment");
  }
  PipelineEditBatch  batch;
  SetParameterChange change;
  change.target.owner_kind             = PipelineParameterOwnerKind::ColorGrade;
  change.target.node_id                = NodeId{"grade.primary"};
  change.target.adjustment_instance_id = *lmt_id;
  change.target.field_key              = "lut";
  change.before_value    = document.PrimaryGrade()->FindAdjustmentByType(type_ids::Lmt())->ToJson();
  change.after_value     = after;
  change.before_enabled  = true;
  change.after_enabled   = true;
  batch.operation_kind   = PipelineEditOperationKind::SetParameter;
  batch.presentation_key = "history.operation.set_parameter";
  batch.changes.push_back(std::move(change));
  std::string error;
  if (!ApplyPipelineEditBatch(document, batch, PipelineEditApplyDirection::Forward, &error)) {
    throw std::runtime_error("LMT batch did not apply: " + error);
  }
  auto&      graph  = lease.graph_;
  auto       commit = EditCommit::MakePipelineEdit(graph.GetRootId(),
                                                   graph.GetActiveVersionRef().head_commit_hash, batch);
  const auto head   = commit.GetCommitHash();
  if (!graph.InsertCommit(std::move(commit))) {
    throw std::runtime_error("LMT commit was not inserted");
  }
  graph.MoveWorkingHead(graph.GetActiveVersionId(), head);
}

auto PrimaryLmt(const PipelineDocument& document) -> const LmtModel* {
  return dynamic_cast<const LmtModel*>(
      document.PrimaryGrade()->FindAdjustmentByType(type_ids::Lmt()));
}

/// Editor preview of @p document on the editor's Interactive executor, as the session renders the
/// whole image.
auto RenderEditorPreview(PipelineExecutor& executor, const PipelineDocument& document,
                         sl_element_id_t element_id, const std::shared_ptr<ImageBuffer>& input)
    -> cv::Mat {
  const auto snapshot =
      PipelineGraphSnapshot::Preview(ClonePipelineDocument(document).Freeze(), element_id,
                                     PipelineLineageId::Next(), transaction_chain_hash_t{});
  PipelineTask task;
  task.options_.render_desc_.render_type_ = RenderType::QUALITY_BASE_PREVIEW;
  auto request                            = task.MakeApplyRequest();
  request.require_host_output             = true;
  std::shared_ptr<ImageBuffer> output;
  {
    std::lock_guard<std::mutex> render_lock(executor.GetRenderLock());
    output = executor.Apply(*snapshot, input, request);
  }
  return output ? HostPixels(*output) : cv::Mat{};
}

auto RequestThumbnail(ThumbnailService& service, sl_element_id_t element_id, image_id_t image_id)
    -> cv::Mat {
  std::promise<ThumbnailRequestResult> done;
  auto                                 done_future = done.get_future();
  service.GetThumbnailDetailed(
      element_id, image_id,
      [&done](ThumbnailRequestResult result) { done.set_value(std::move(result)); }, true, nullptr,
      ThumbnailResolution::k256);
  if (done_future.wait_for(120s) != std::future_status::ready) {
    ADD_FAILURE() << "thumbnail request did not finish";
    return {};
  }
  const auto result = done_future.get();
  if (!result.guard || !result.guard->thumbnail_buffer_) {
    ADD_FAILURE() << "thumbnail has no pixels: " << result.message;
    return {};
  }
  return HostPixels(*result.guard->thumbnail_buffer_);
}

auto ExportJpeg(ExportService& service, sl_element_id_t element_id, image_id_t image_id,
                const std::filesystem::path& path) -> cv::Mat {
  ExportTask task;
  task.sleeve_id_                = element_id;
  task.image_id_                 = image_id;
  task.options_.format_          = ImageFormatType::JPEG;
  task.options_.export_path_     = path;
  task.options_.resize_enabled_  = true;
  task.options_.max_length_side_ = 256;
  task.recipe_                   = ExportRecipe::FromLegacyOptions(task.options_);
  service.EnqueueExportTask(task);
  std::promise<std::shared_ptr<std::vector<ExportResult>>> done;
  auto                                                     done_future = done.get_future();
  service.ExportAll([&done](std::shared_ptr<std::vector<ExportResult>> results) {
    done.set_value(std::move(results));
  });
  if (done_future.wait_for(180s) != std::future_status::ready) {
    ADD_FAILURE() << "export did not finish";
    return {};
  }
  const auto results = done_future.get();
  if (!results || results->size() != 1 || !results->front().success_) {
    ADD_FAILURE() << "export failed: "
                  << (results && !results->empty() ? results->front().message_ : "no result");
    return {};
  }
  auto input = OIIO::ImageInput::open(path.string());
  if (!input) {
    return {};
  }
  const OIIO::ImageSpec spec = input->spec();
  cv::Mat               pixels(spec.height, spec.width, CV_MAKETYPE(CV_8U, spec.nchannels));
  const bool ok = input->read_image(0, 0, 0, spec.nchannels, OIIO::TypeDesc::UINT8, pixels.data);
  input->close();
  return ok ? pixels : cv::Mat{};
}

class LutEncodingRenderPathsTest : public ::testing::Test {
 protected:
  void SetUp() override {
    if (!std::filesystem::exists(LinearDngPath())) {
      GTEST_SKIP() << "Sample DNG file is missing: " << LinearDngPath().string();
    }
    TimeProvider::Refresh();
    const auto stamp = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    work_dir_ = std::filesystem::temp_directory_path() / ("lut_encoding_render_paths_" + stamp);
    std::filesystem::create_directories(work_dir_);
  }

  void TearDown() override {
    std::error_code ec;
    std::filesystem::remove_all(work_dir_, ec);
  }

  std::filesystem::path work_dir_;
};

TEST_F(LutEncodingRenderPathsTest, StoredNonDefaultEncodingRendersAlikeInEditorThumbnailAndExport) {
  const auto cube = work_dir_ / "look.cube";
  WriteLookCube(cube);
  const auto      db_path    = work_dir_ / "project.db";
  const auto      meta_path  = work_dir_ / "project.json";

  sl_element_id_t element_id = 0;
  image_id_t      image_id   = 0;
  {
    ProjectService project(db_path, meta_path);
    auto           pipelines = std::make_shared<PipelineMgmtService>(project.GetStorage());
    const auto     ids       = ImportLinearDng(project, pipelines);
    ASSERT_NE(ids.first, 0u);
    element_id         = ids.first;
    image_id           = ids.second;

    auto       lease   = pipelines->AcquireEditorLease(element_id);
    auto       working = std::make_unique<EditorWorkingDocument>(element_id, lease.document_);
    const auto before  = lease.graph_.GetImageEditState();
    CommitLmt(lease, *working,
              {{"cube_path", cube.string()},
               {"input_encoding", kInputEncoding},
               {"output_encoding", kOutputEncoding}});
    std::string error;
    ASSERT_TRUE(pipelines->PersistEditorHistory(lease.graph_, before, working->Document(), &error))
        << error;
    pipelines->ReleaseEditorLease(element_id);
    project.SaveProject(meta_path);
  }

  // Reopen the project: every path reads the stored history.
  ProjectService project(db_path, meta_path);
  auto           pipelines = std::make_shared<PipelineMgmtService>(project.GetStorage());
  const auto     lease     = pipelines->AcquireEditorLease(element_id);
  ASSERT_NE(lease.document_, nullptr);
  const auto* stored_lmt = PrimaryLmt(*lease.document_);
  ASSERT_NE(stored_lmt, nullptr);
  EXPECT_EQ(stored_lmt->InputEncoding(), kInputEncoding);
  EXPECT_EQ(stored_lmt->OutputEncoding(), kOutputEncoding);
  EXPECT_EQ(stored_lmt->CubePath(), cube.string());

  const auto input = LoadEncodedInput(*project.GetImagePoolService(), image_id);
  ASSERT_NE(input, nullptr);
  PipelineExecutor editor(ExecutorRole::Interactive, pipelines->LutResources());
  const auto       editor_encoded =
      ToRgb8(RenderEditorPreview(editor, *lease.document_, element_id, input));
  auto default_document = ClonePipelineDocument(*lease.document_);
  dynamic_cast<LmtModel&>(*default_document.PrimaryGrade()->FindAdjustmentByType(type_ids::Lmt()))
      .SetEncodings("acescc", "acescc");
  const auto editor_default =
      ToRgb8(RenderEditorPreview(editor, default_document, element_id, input));
  pipelines->ReleaseEditorLease(element_id);
  ASSERT_FALSE(editor_encoded.empty());
  ASSERT_FALSE(editor_default.empty());

  ThumbnailService thumbnails(project.GetSleeveService(), project.GetImagePoolService(), pipelines);
  const auto       thumbnail = ToRgb8(RequestThumbnail(thumbnails, element_id, image_id));
  thumbnails.ReleaseThumbnail(element_id);
  ASSERT_FALSE(thumbnail.empty());
  ExportService export_service(project.GetSleeveService(), project.GetImagePoolService(),
                               pipelines);
  const auto    exported =
      ToRgb8(ExportJpeg(export_service, element_id, image_id, work_dir_ / "export.jpg"));
  ASSERT_FALSE(exported.empty());

  const cv::Size size                = thumbnail.size();
  const auto     editor_at_size      = ResizedTo(editor_encoded, size);
  const auto     default_at_size     = ResizedTo(editor_default, size);
  const auto     export_at_size      = ResizedTo(exported, size);
  const double   thumbnail_vs_editor = MeanAbsoluteDifference(thumbnail, editor_at_size);
  const double   export_vs_editor    = MeanAbsoluteDifference(export_at_size, editor_at_size);
  const double   encodings_effect    = MeanAbsoluteDifference(default_at_size, editor_at_size);
  std::cout << "[L4 pixels] thumbnail vs editor " << thumbnail_vs_editor << ", export vs editor "
            << export_vs_editor << ", default vs non-default encodings in the editor "
            << encodings_effect << " (8-bit mean absolute difference at " << size.width << "x"
            << size.height << ")\n";
  EXPECT_GT(encodings_effect, kDistinctStateDifference)
      << "the encodings must change the rendered image";
  EXPECT_LE(thumbnail_vs_editor, kSameStateDifference);
  EXPECT_LE(export_vs_editor, kSameStateDifference);
}

}  // namespace
}  // namespace alcedo
