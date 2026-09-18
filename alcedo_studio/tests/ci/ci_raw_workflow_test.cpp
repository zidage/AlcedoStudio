//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <exiv2/exiv2.hpp>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <mutex>
#include <opencv2/core.hpp>
#include <string>
#include <vector>

#include "app/import_service.hpp"
#include "app/project_service.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/operators/operator_registeration.hpp"
#include "edit/pipeline/pipeline_cpu.hpp"
#include "image/image.hpp"
#include "image/image_buffer.hpp"
#include "image/metadata_extractor.hpp"
#include "renderer/pipeline_scheduler.hpp"
#include "sleeve/sleeve_element/sleeve_element.hpp"
#include "type/supported_file_type.hpp"
#include "utils/clock/time_provider.hpp"

namespace alcedo {
namespace {
using namespace std::chrono_literals;

auto CiRawDir() -> std::filesystem::path {
  return std::filesystem::path(TEST_IMG_PATH) / "ci_rawfiles";
}

auto CollectCiRawFiles(size_t max_count = 0) -> std::vector<std::filesystem::path> {
  const auto collect_from_root = [max_count](const std::filesystem::path& dir) {
    std::vector<std::filesystem::path> paths;
    if (!std::filesystem::exists(dir)) {
      return paths;
    }

    for (const auto& entry : std::filesystem::recursive_directory_iterator(dir)) {
      if (entry.is_regular_file() && is_supported_file(entry.path())) {
        paths.push_back(entry.path());
        if (max_count != 0 && paths.size() >= max_count) {
          break;
        }
      }
    }

    std::sort(paths.begin(), paths.end());
    return paths;
  };

  auto paths = collect_from_root(CiRawDir());
  if (!paths.empty()) {
    return paths;
  }
  return collect_from_root(std::filesystem::path("/Users/zidage/Photos"));
}

auto ReadFileToBuffer(const std::filesystem::path& path) -> std::vector<uint8_t> {
  std::ifstream file(path, std::ios::binary);
  if (!file.is_open()) {
    return {};
  }

  file.seekg(0, std::ios::end);
  const std::streamsize size = file.tellg();
  file.seekg(0, std::ios::beg);
  if (size <= 0) {
    return {};
  }

  std::vector<uint8_t> buffer(static_cast<size_t>(size));
  if (!file.read(reinterpret_cast<char*>(buffer.data()), size)) {
    return {};
  }
  return buffer;
}

auto MakeTempPath(const char* suffix) -> std::filesystem::path {
  const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
  return std::filesystem::temp_directory_path() /
         std::filesystem::path(std::string("alcedo_ci_") + std::to_string(tick) + suffix);
}

/** Bind a Default document and install camera/profile data from the RAW file.
 *  Product Apply reads those matrices from the document; a bare executor fails. */
auto BindDefaultDocumentWithImportedCamera(CPUPipelineExecutor& pipeline,
                                            const std::filesystem::path& path) -> bool {
  Image image(1, path, ImageType::DEFAULT);
  MetadataExtractor::ExtractEXIF_ToImage(path, image);
  if (!image.HasRawColorContext()) {
    return false;
  }
  auto document = std::make_shared<PipelineDocument>(CreateDefaultPipelineDocument());
  std::unique_lock lock(pipeline.GetRenderLock());
  pipeline.SetPipelineDocument(document);
  pipeline.InjectRawMetadata(MetadataExtractor::ReadRawColorContextForRender(image));
  return true;
}

void AssertFloatImage(ImageBuffer& buffer) {
  ASSERT_TRUE(buffer.cpu_data_valid_);
  const cv::Mat& cpu = buffer.GetCPUData();
  ASSERT_FALSE(cpu.empty());
  EXPECT_EQ(cpu.depth(), CV_32F);
  EXPECT_TRUE(cpu.channels() == 3 || cpu.channels() == 4);
  EXPECT_GT(cpu.cols, 0);
  EXPECT_GT(cpu.rows, 0);
}

auto RenderBlocking(RenderType render_type, const std::filesystem::path& path)
    -> std::shared_ptr<ImageBuffer> {
  auto raw_bytes = ReadFileToBuffer(path);
  if (raw_bytes.empty()) {
    ADD_FAILURE() << "Failed to read CI RAW fixture " << path.string();
    return nullptr;
  }

  auto pipeline = std::make_shared<CPUPipelineExecutor>();
  if (!BindDefaultDocumentWithImportedCamera(*pipeline, path)) {
    ADD_FAILURE() << "CI RAW fixture has no camera matrices: " << path.string();
    return nullptr;
  }

  PipelineTask task;
  task.pipeline_executor_                 = pipeline;
  task.input_                             = std::make_shared<ImageBuffer>(std::move(raw_bytes));
  task.options_.render_desc_.render_type_ = render_type;
  task.options_.is_blocking_              = true;
  task.options_.is_callback_              = false;
  task.options_.is_seq_callback_          = false;

  auto promise = std::make_shared<std::promise<std::shared_ptr<ImageBuffer>>>();
  auto future  = promise->get_future();
  task.result_ = promise;

  PipelineScheduler scheduler(1);
  scheduler.ScheduleTask(std::move(task));
  if (future.wait_for(90s) != std::future_status::ready) {
    ADD_FAILURE() << "Timed out waiting for pipeline render";
    return nullptr;
  }
  return future.get();
}

class CiRawWorkflowTest : public ::testing::Test {
 protected:
  void SetUp() override {
    TimeProvider::Refresh();
    Exiv2::LogMsg::setLevel(Exiv2::LogMsg::Level::mute);
    RegisterAllOperators();
  }
};

}  // namespace

TEST_F(CiRawWorkflowTest, RawImportPersistsAcrossProjectReload) {
  const auto raw_files = CollectCiRawFiles(2);
  if (raw_files.empty()) {
    GTEST_SKIP() << "CI RAW fixtures missing under " << CiRawDir()
                 << " and /Users/zidage/Photos";
  }

  const auto db_path   = MakeTempPath(".db");
  const auto meta_path = MakeTempPath(".json");

  auto       cleanup   = [&]() {
    std::error_code ec;
    std::filesystem::remove(db_path, ec);
    std::filesystem::remove(meta_path, ec);
  };
  cleanup();

  {
    ProjectService            project(db_path, meta_path);
    auto                      sleeve_service = project.GetSleeveService();
    auto                      image_pool     = project.GetImagePoolService();

    ImportServiceImpl         import_service(sleeve_service, image_pool);

    std::vector<image_path_t> paths;
    paths.reserve(raw_files.size());
    for (const auto& path : raw_files) {
      paths.push_back(path);
    }

    auto                       import_job = std::make_shared<ImportJob>();
    std::promise<ImportResult> import_done;
    auto                       import_future = import_done.get_future();
    import_job->on_finished_                 = [&import_done](const ImportResult& result) {
      import_done.set_value(result);
    };

    import_job = import_service.ImportToFolder(paths, L"", {}, import_job);
    ASSERT_NE(import_job, nullptr);
    ASSERT_EQ(import_future.wait_for(90s), std::future_status::ready);

    const ImportResult result = import_future.get();
    EXPECT_EQ(result.requested_, static_cast<uint32_t>(paths.size()));
    EXPECT_EQ(result.imported_, static_cast<uint32_t>(paths.size()));
    EXPECT_EQ(result.failed_, 0u);

    ASSERT_NE(import_job->import_log_, nullptr);
    const auto snapshot = import_job->import_log_->Snapshot();
    ASSERT_EQ(snapshot.created_.size(), paths.size());
    ASSERT_EQ(snapshot.metadata_ok_.size(), paths.size());
    EXPECT_EQ(snapshot.metadata_failed_.size(), 0u);

    import_service.SyncImports(snapshot, L"");
    project.SaveProject(meta_path);
  }

  {
    ProjectService reopened(db_path, meta_path, ProjectOpenMode::kLoadExisting);
    const auto     entries = reopened.GetSleeveService()->ListFolderEntries("/");
    EXPECT_GE(entries.size(), raw_files.size());
  }

  cleanup();
}

TEST_F(CiRawWorkflowTest, DefaultPipelineRendersCiRawFixture) {
  const auto raw_files = CollectCiRawFiles(1);
  if (raw_files.empty()) {
    GTEST_SKIP() << "CI RAW fixtures missing under " << CiRawDir()
                 << " and /Users/zidage/Photos";
  }

  auto raw_bytes = ReadFileToBuffer(raw_files.front());
  ASSERT_FALSE(raw_bytes.empty());

  CPUPipelineExecutor pipeline;
  ASSERT_TRUE(BindDefaultDocumentWithImportedCamera(pipeline, raw_files.front()))
      << raw_files.front().string();
  pipeline.SetForceCPUOutput(true);

  std::shared_ptr<ImageBuffer> output;
  {
    std::unique_lock lock(pipeline.GetRenderLock());
    output = pipeline.Apply(std::make_shared<ImageBuffer>(std::move(raw_bytes)));
  }
  ASSERT_NE(output, nullptr);
  if (!output->cpu_data_valid_) {
    ASSERT_NO_THROW(output->SyncToCPU());
  }
  AssertFloatImage(*output);
}

TEST_F(CiRawWorkflowTest, SchedulerProducesThumbnailAndFastPreview) {
  const auto raw_files = CollectCiRawFiles(1);
  if (raw_files.empty()) {
    GTEST_SKIP() << "CI RAW fixtures missing under " << CiRawDir()
                 << " and /Users/zidage/Photos";
  }

  // Album / semantic / analysis all schedule RenderType::THUMBNAIL: one-shot DAG,
  // host download, no editor frame sink. Camera matrices come from the bound document.
  auto thumbnail = RenderBlocking(RenderType::THUMBNAIL, raw_files.front());
  ASSERT_NE(thumbnail, nullptr);
  if (!thumbnail->cpu_data_valid_) {
    ASSERT_NO_THROW(thumbnail->SyncToCPU());
  }
  AssertFloatImage(*thumbnail);
  EXPECT_LE(std::max(thumbnail->GetCPUData().cols, thumbnail->GetCPUData().rows), 1024);

  // FAST_PREVIEW is the editor interactive present path (session cache, GPU sink).
  // This CI helper has no viewer sink, so Apply returns an empty host buffer after
  // the DAG runs. Pixel proof for album/semantic is the THUMBNAIL branch above.
  auto fast_preview = RenderBlocking(RenderType::FAST_PREVIEW, raw_files.front());
  ASSERT_NE(fast_preview, nullptr);
}

}  // namespace alcedo
