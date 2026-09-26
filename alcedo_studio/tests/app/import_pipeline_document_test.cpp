// Copyright 2026 Yurun Zi
// SPDX-License-Identifier: GPL-3.0-only
// Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <future>
#include <memory>
#include <mutex>
#include <string>

#include "app/import_service.hpp"
#include "app/pipeline_service.hpp"
#include "app/project_service.hpp"
#include "edit/graph/develop_color_transform.hpp"
#include "edit/runtime/pipeline_apply_request.hpp"
#include "image/dng_color_profile.hpp"
#include "io/image/image_loader.hpp"

namespace alcedo {
namespace {

/** @brief Exercise import graph creation, persisted camera data and the first background render. */
TEST(ImportPipelineDocumentTest, ImportCreatesRenderableDocumentWithoutStageMirror) {
  const auto root =
      std::filesystem::path(TEST_IMG_PATH).parent_path().parent_path().parent_path().parent_path();
  const auto work = root / "build/tmp/nm1" /
                    ("import-document-" +
                     std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  std::filesystem::create_directories(work);
  const auto raw_path = std::filesystem::path(TEST_IMG_PATH) / "raw/linear_dng/mfzoty.dng";
  ASSERT_TRUE(std::filesystem::exists(raw_path));

  ProjectService    project(work / "project.db", work / "project.json");
  auto              pool = project.GetImagePoolService();
  auto import_pipelines = std::make_shared<PipelineMgmtService>(project.GetStorage());
  ImportServiceImpl importer(project.GetSleeveService(), pool, import_pipelines);
  auto              job        = std::make_shared<ImportJob>();
  auto              completion = std::make_shared<std::promise<ImportResult>>();
  auto              completed  = completion->get_future();
  job->on_finished_ = [completion](const ImportResult& result) { completion->set_value(result); };
  job               = importer.ImportToFolder({raw_path}, L"", {}, job);
  ASSERT_EQ(completed.wait_for(std::chrono::seconds(60)), std::future_status::ready);
  const auto result = completed.get();
  ASSERT_EQ(result.imported_, 1u);
  ASSERT_EQ(result.failed_, 0u);
  const auto imported = job->import_log_->Snapshot();
  ASSERT_EQ(imported.created_.size(), 1u);
  importer.SyncImports(imported, L"");
  const auto element_id = imported.created_.front().element_id_;
  const auto image_id   = imported.created_.front().image_id_;
  const auto image      = pool->Read<std::shared_ptr<Image>>(
      image_id, [](const std::shared_ptr<Image>& value) { return value; });
  ASSERT_NE(image, nullptr);
  ASSERT_TRUE(image->HasRawColorContext());
  const auto& raw = image->GetRawColorContext();
  ASSERT_TRUE(raw.dng_profile_.IsBound());
  ASSERT_TRUE(raw.color_matrices_valid_);

  const auto stored =
      project.GetStorage()->GetElementStore().GetPipelineJsonByElementId(element_id);
  ASSERT_TRUE(stored.has_value());
  // The stored Develop JSON references the DNG profile by fingerprint and holds no table data.
  const auto stored_text = stored->dump();
  EXPECT_EQ(stored_text.find("hue_sat_map"), std::string::npos);
  EXPECT_EQ(stored_text.find("look_table"), std::string::npos);
  EXPECT_NE(stored_text.find(DngColorProfileFingerprintToText(raw.dng_profile_->fingerprint)),
            std::string::npos);
  EXPECT_FALSE(stored->contains("stages"));
  EXPECT_FALSE(stored->contains("legacy_stage_adapter"));
  const auto persisted = PipelineDocument::FromJson(*stored);
  EXPECT_EQ(persisted.Graph().Nodes().size(), 3U);
  EXPECT_EQ(persisted.Graph().Edges().size(), 2U);
  EXPECT_TRUE(persisted.Graph().Validate().empty());
  EXPECT_TRUE(persisted.Graph().ValidateImageBackbone().empty());
  ASSERT_NE(persisted.Develop(), nullptr);
  const auto expected = persisted.Develop()->Params().Params();
  ASSERT_NE(persisted.PrimaryGrade(), nullptr);
  const auto* exposure = persisted.PrimaryGrade()->FindAdjustmentByType(type_ids::Exposure());
  ASSERT_NE(exposure, nullptr);
  EXPECT_FLOAT_EQ(exposure->ToJson().at("exposure_ev").get<float>(), kDefaultPipelineExposureEv);
  const auto* saturation = persisted.PrimaryGrade()->FindAdjustmentByType(type_ids::Saturation());
  ASSERT_NE(saturation, nullptr);
  EXPECT_FLOAT_EQ(saturation->ToJson().at("saturation").get<float>(), kDefaultPipelineSaturation);
  const auto* contrast = persisted.PrimaryGrade()->FindAdjustmentByType(type_ids::Contrast());
  ASSERT_NE(contrast, nullptr);
  EXPECT_FLOAT_EQ(contrast->ToJson().at("contrast").get<float>(), kDefaultPipelineContrast);
  EXPECT_EQ(expected.camera_profile.dng_profile, raw.dng_profile_);
  EXPECT_TRUE(expected.camera_profile.color_matrices_valid);
  // A document read from project data holds an unbound profile reference and does not render.
  EXPECT_FALSE(expected.camera_profile.dng_profile.IsBound());
  EXPECT_EQ(ResolveDevelopColorTransform(expected).error, ColorTransformError::UnboundDngProfile);

  PipelineMgmtService pipelines(project.GetStorage());
  pipelines.SetAcceleratorBackendPreference(AcceleratorBackendPreference::CUDA);
  auto loaded = pipelines.LoadPipeline(element_id);
  ASSERT_NE(loaded, nullptr);
  ASSERT_NE(loaded->document_, nullptr);
  EXPECT_EQ(loaded->document_->Develop()->Params().Params(), expected);
  // LoadPipeline binds the profile from the source file before the document goes live.
  const auto bound = loaded->document_->Develop()->Params().Params();
  ASSERT_TRUE(bound.camera_profile.dng_profile.IsBound());
  EXPECT_EQ(bound.camera_profile.dng_profile->fingerprint, raw.dng_profile_->fingerprint);
  EXPECT_TRUE(ResolveDevelopColorTransform(bound).ok);
  EXPECT_EQ(loaded->pipeline_->GpuDagDocument(), loaded->document_);
  const auto                   before = loaded->document_->ToJson();
  auto                         bytes  = ByteBufferLoader::LoadByteBufferFromImage(image);
  auto                         input  = std::make_shared<ImageBuffer>(std::move(bytes));
  std::shared_ptr<ImageBuffer> output;
  {
    std::unique_lock lock(loaded->pipeline_->GetRenderLock());
    PipelineApplyRequest request;
    request.geometry.resolution.max_edge = 256;
    request.geometry.resolution.quality  = RenderQuality::Export;
    request.decode_res                   = DecodeRes::FULL;
    request.require_host_output          = true;
    output = loaded->pipeline_->Apply(input, request);
  }
  ASSERT_NE(output, nullptr);
  ASSERT_TRUE(output->cpu_data_valid_);
  const auto pixels = output->GetCPUData();
  EXPECT_TRUE(cv::checkRange(pixels));
  EXPECT_GT(cv::mean(pixels)[1], 0.01);
  EXPECT_EQ(loaded->document_->ToJson(), before);
  pipelines.SavePipeline(loaded);
}

/** @brief Import binds the RAW camera profile on the document; no stage value is read. */
TEST(ImportPipelineDocumentTest, ImportBindsCameraProfileOnDocumentOnly) {
  const auto root =
      std::filesystem::path(TEST_IMG_PATH).parent_path().parent_path().parent_path().parent_path();
  const auto work = root / "build/tmp/g10_1" /
                    ("import-camera-profile-" +
                     std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  std::filesystem::create_directories(work);
  const auto raw_path = std::filesystem::path(TEST_IMG_PATH) / "raw/linear_dng/mfzoty.dng";
  ASSERT_TRUE(std::filesystem::exists(raw_path));

  ProjectService    project(work / "project.db", work / "project.json");
  auto              pool      = project.GetImagePoolService();
  auto              pipelines = std::make_shared<PipelineMgmtService>(project.GetStorage());
  ImportServiceImpl importer(project.GetSleeveService(), pool, pipelines);
  auto              job        = std::make_shared<ImportJob>();
  auto              completion = std::make_shared<std::promise<ImportResult>>();
  auto              completed  = completion->get_future();
  job->on_finished_ = [completion](const ImportResult& result) { completion->set_value(result); };
  job               = importer.ImportToFolder({raw_path}, L"", {}, job);
  ASSERT_EQ(completed.wait_for(std::chrono::seconds(60)), std::future_status::ready);
  ASSERT_EQ(completed.get().imported_, 1u);
  const auto imported = job->import_log_->Snapshot();
  ASSERT_EQ(imported.created_.size(), 1u);
  importer.SyncImports(imported, L"");
  const auto image =
      pool->Read<std::shared_ptr<Image>>(imported.created_.front().image_id_,
                                         [](const std::shared_ptr<Image>& value) { return value; });
  ASSERT_NE(image, nullptr);
  ASSERT_TRUE(image->HasRawColorContext());

  // Expected values: the Develop defaults with only the imported RAW context bound.
  DevelopPayload expected;
  BindDevelopCameraProfile(expected, image->GetRawColorContext());

  const auto stored = project.GetStorage()->GetElementStore().GetPipelineJsonByElementId(
      imported.created_.front().element_id_);
  ASSERT_TRUE(stored.has_value());
  const auto document = PipelineDocument::FromJson(*stored);
  ASSERT_NE(document.Develop(), nullptr);
  const auto develop = document.Develop()->Params().Params();
  EXPECT_EQ(develop.camera_profile.color_matrices_valid,
            expected.camera_profile.color_matrices_valid);
  EXPECT_EQ(develop.camera_profile.color_matrix_1, expected.camera_profile.color_matrix_1);
  EXPECT_EQ(develop.camera_profile.color_matrix_2, expected.camera_profile.color_matrix_2);
  EXPECT_EQ(develop.camera_profile.forward_matrix_1, expected.camera_profile.forward_matrix_1);
  EXPECT_EQ(develop.camera_profile.forward_matrix_2, expected.camera_profile.forward_matrix_2);
  EXPECT_EQ(develop.camera_profile.as_shot_neutral, expected.camera_profile.as_shot_neutral);
  EXPECT_EQ(develop.camera_profile.dng_profile, expected.camera_profile.dng_profile);
  EXPECT_TRUE(develop.camera_profile.dng_profile.IsReferenced());
  EXPECT_FLOAT_EQ(develop.as_shot_cct, expected.as_shot_cct);
  EXPECT_FLOAT_EQ(develop.as_shot_tint, expected.as_shot_tint);
  // Import does not write user or lens fields into the document.
  EXPECT_EQ(develop.lens_maker, DevelopPayload{}.lens_maker);
  EXPECT_EQ(develop.lens_model, DevelopPayload{}.lens_model);
  EXPECT_EQ(develop.lens_profile_db_path, DevelopPayload{}.lens_profile_db_path);
  EXPECT_EQ(develop.wb_mode, DevelopPayload{}.wb_mode);
}

}  // namespace
}  // namespace alcedo
