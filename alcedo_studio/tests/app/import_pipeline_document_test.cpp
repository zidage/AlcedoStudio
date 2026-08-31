// Copyright 2026 Yurun Zi
// SPDX-License-Identifier: GPL-3.0-only
// Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <future>
#include <memory>
#include <mutex>

#include "app/import_service.hpp"
#include "app/pipeline_service.hpp"
#include "app/project_service.hpp"
#include "edit/graph/develop_color_transform.hpp"
#include "edit/operators/operator_registeration.hpp"
#include "io/image/image_loader.hpp"

namespace alcedo {
namespace {

/** @brief Exercise import graph creation, persisted camera data and the first background render. */
TEST(ImportPipelineDocumentTest, ImportCreatesRenderableDocumentWithoutStageMirror) {
  RegisterAllOperators();
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
  ASSERT_NE(raw.dng_profile_, nullptr);
  ASSERT_TRUE(raw.color_matrices_valid_);

  const auto stored =
      project.GetStorage()->GetElementStore().GetPipelineJsonByElementId(element_id);
  ASSERT_TRUE(stored.has_value());
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
  EXPECT_TRUE(DngColorProfilesEqual(expected.camera_profile.dng_profile, raw.dng_profile_));
  EXPECT_TRUE(expected.camera_profile.color_matrices_valid);
  EXPECT_TRUE(ResolveDevelopColorTransform(expected).ok);

  PipelineMgmtService pipelines(project.GetStorage());
  pipelines.SetAcceleratorBackendPreference(AcceleratorBackendPreference::CUDA);
  auto loaded = pipelines.LoadPipeline(element_id);
  ASSERT_NE(loaded, nullptr);
  ASSERT_NE(loaded->document_, nullptr);
  EXPECT_EQ(loaded->document_->Develop()->Params().Params(), expected);
  EXPECT_EQ(loaded->pipeline_->GpuDagDocument(), loaded->document_);
  const auto                   before = loaded->document_->ToJson();
  auto                         bytes  = ByteBufferLoader::LoadByteBufferFromImage(image);
  auto                         input  = std::make_shared<ImageBuffer>(std::move(bytes));
  std::shared_ptr<ImageBuffer> output;
  {
    std::unique_lock lock(loaded->pipeline_->GetRenderLock());
    loaded->pipeline_->SetForceCPUOutput(true);
    loaded->pipeline_->SetDecodeRes(DecodeRes::FULL);
    loaded->pipeline_->SetRenderRes(false, 256);
    output = loaded->pipeline_->Apply(input);
  }
  ASSERT_NE(output, nullptr);
  ASSERT_TRUE(output->cpu_data_valid_);
  const auto pixels = output->GetCPUData();
  EXPECT_TRUE(cv::checkRange(pixels));
  EXPECT_GT(cv::mean(pixels)[1], 0.01);
  EXPECT_EQ(loaded->document_->ToJson(), before);
  pipelines.SavePipeline(loaded);
}

}  // namespace
}  // namespace alcedo
