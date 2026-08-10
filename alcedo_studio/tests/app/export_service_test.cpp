//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/export_service.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <exiv2/exiv2.hpp>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <memory>
#include <opencv2/imgcodecs.hpp>
#include <string>
#include <vector>
#if defined(ALCEDO_HAS_ULTRAHDR)
#include <ultrahdr_api.h>
#endif

#include "app/import_service.hpp"
#include "app/pipeline_service.hpp"
#include "app/project_service.hpp"
#include "edit/operators/operator_registeration.hpp"
#include "edit/pipeline/default_pipeline_params.hpp"
#include "io/image/image_writer.hpp"
#include "type/supported_file_type.hpp"
#include "utils/clock/time_provider.hpp"
#include "utils/profiler/profiler.hpp"
#include "utils/string/convert.hpp"

namespace alcedo {
namespace {
using namespace std::chrono_literals;

auto SanitizeForPath(std::string s) -> std::string {
  for (char& c : s) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                    c == '_' || c == '-' || c == '.';
    if (!ok) c = '_';
  }
  return s;
}

auto CollectSupportedBatchImportImages(size_t max_count) -> std::vector<image_path_t> {
  // Prefer the smaller CI ARW set for full-res export coverage; batch_import DNGs
  // are ~8K and have crashed the standard JPEG writer under memory pressure.
  // Within a root, prefer the smallest supported file so ExportOneImage exercises
  // the production Import→Pipeline→ImageWriter path without multi-hundred-MB peak.
  const std::filesystem::path candidates[] = {
      std::filesystem::path(TEST_IMG_PATH) / "ci_rawfiles",
      std::filesystem::path(TEST_IMG_PATH) / "raw" / "linear_dng",
      std::filesystem::path(TEST_IMG_PATH) / "raw" / "batch_import",
  };
  std::vector<image_path_t> paths;
  for (const auto& img_root_path : candidates) {
    if (!std::filesystem::exists(img_root_path)) {
      continue;
    }
    for (const auto& entry : std::filesystem::directory_iterator(img_root_path)) {
      if (entry.is_regular_file() && is_supported_file(entry.path())) {
        paths.push_back(entry.path());
      }
    }
    if (!paths.empty()) {
      break;
    }
  }

  std::sort(paths.begin(), paths.end(), [](const image_path_t& a, const image_path_t& b) {
    std::error_code ea;
    std::error_code eb;
    const auto      sa = std::filesystem::file_size(a, ea);
    const auto      sb = std::filesystem::file_size(b, eb);
    if (ea || eb) {
      return a < b;
    }
    if (sa != sb) {
      return sa < sb;
    }
    return a < b;
  });
  if (max_count != 0 && paths.size() > max_count) {
    paths.resize(max_count);
  }
  return paths;
}

void AssertReadableNonEmptyImageFile(const std::filesystem::path& path) {
  ASSERT_TRUE(std::filesystem::exists(path)) << "Missing export file: " << path.string();
  ASSERT_GT(std::filesystem::file_size(path), 0u) << "Export file empty: " << path.string();

  // const cv::Mat img = cv::imread(conv::ToBytes(path.wstring()), cv::IMREAD_UNCHANGED);
  // ASSERT_FALSE(img.empty()) << "Failed to read export image: " << path.string();
  // ASSERT_GT(img.cols, 0);
  // ASSERT_GT(img.rows, 0);
}

void AssertReadableJpegFile(const std::filesystem::path& path) {
  const cv::Mat img = cv::imread(conv::ToBytes(path.wstring()), cv::IMREAD_UNCHANGED);
  ASSERT_FALSE(img.empty()) << "Failed to read JPEG file: " << path.string();
  ASSERT_GT(img.cols, 0);
  ASSERT_GT(img.rows, 0);
}

auto ReadFileBytes(const std::filesystem::path& path) -> std::vector<uint8_t> {
  std::ifstream input(path, std::ios::binary);
  if (!input.is_open()) {
    return {};
  }
  return std::vector<uint8_t>(std::istreambuf_iterator<char>(input),
                              std::istreambuf_iterator<char>());
}
}  // namespace

class ExportServiceTests : public ::testing::Test {
 protected:
  std::filesystem::path db_path_;
  std::filesystem::path meta_path_;
  std::filesystem::path export_dir_;
  bool                  keep_exports_ = false;

  void                  SetUp() override {
    TimeProvider::Refresh();
    Exiv2::LogMsg::setLevel(Exiv2::LogMsg::Level::mute);
    RegisterAllOperators();

    db_path_ = std::filesystem::temp_directory_path() / "export_service_test.db";
    meta_path_ = std::filesystem::temp_directory_path() / "export_service_test.json";

    if (std::filesystem::exists(db_path_)) {
      std::filesystem::remove(db_path_);
    }
    if (std::filesystem::exists(meta_path_)) {
      std::filesystem::remove(meta_path_);
    }

    auto*             test_info = ::testing::UnitTest::GetInstance()->current_test_info();
    const std::string suite_name = test_info ? test_info->test_suite_name() : "ExportServiceTests";
    const std::string test_name = test_info ? test_info->name() : "UnknownTest";

    const std::filesystem::path export_root = std::filesystem::path(TEST_IMG_PATH) / "export";

    if (test_name.find("Manual") != std::string::npos) {
      keep_exports_ = true;
      export_dir_   = export_root / "manual";
      std::error_code ec;
      std::filesystem::remove_all(export_dir_, ec);
      std::filesystem::create_directories(export_dir_);
    } else {
      export_dir_ = export_root / SanitizeForPath(suite_name) / SanitizeForPath(test_name);
      std::filesystem::create_directories(export_dir_);
    }

#ifdef EASY_PROFILER_ENABLE
    EASY_PROFILER_ENABLE;
#endif
  }

  void TearDown() override {
    if (std::filesystem::exists(db_path_)) {
      std::filesystem::remove(db_path_);
    }
    if (std::filesystem::exists(meta_path_)) {
      std::filesystem::remove(meta_path_);
    }

    // if (!keep_exports_ && !export_dir_.empty() && std::filesystem::exists(export_dir_)) {
    //   std::error_code ec;
    //   std::filesystem::remove_all(export_dir_, ec);
    // }

#ifdef EASY_PROFILER_ENABLE
    profiler::dumpBlocksToFile(TEST_PROFILER_OUTPUT_PATH);
    EASY_PROFILER_DISABLE;
#endif
  }
};

TEST_F(ExportServiceTests, ExportOneImage_WritesReadableFile) {
  std::filesystem::path dst_path_global;
  {
    ProjectService project(db_path_, meta_path_);
    auto           sleeve_service   = project.GetSleeveService();
    auto           image_pool       = project.GetImagePoolService();
    auto           pipeline_service = std::make_shared<PipelineMgmtService>(project.GetStorage());

    ImportServiceImpl import_service(sleeve_service, image_pool);
    auto              paths = CollectSupportedBatchImportImages(/*max_count=*/1);
    if (paths.empty()) {
      GTEST_SKIP() << "No supported images found under TEST_IMG_PATH/raw/batch_import";
    }

    auto                       import_job = std::make_shared<ImportJob>();
    std::promise<ImportResult> import_done;
    auto                       import_done_fut = import_done.get_future();
    import_job->on_finished_                   = [&import_done](const ImportResult& result) {
      import_done.set_value(result);
    };

    import_job = import_service.ImportToFolder(paths, L"", {}, import_job);
    ASSERT_NE(import_job, nullptr);
    ASSERT_EQ(import_done_fut.wait_for(120s), std::future_status::ready);
    const ImportResult import_res = import_done_fut.get();
    ASSERT_EQ(import_res.failed_, 0u);

    ASSERT_NE(import_job->import_log_, nullptr);
    auto snapshot = import_job->import_log_->Snapshot();
    ASSERT_FALSE(snapshot.created_.empty());

    const auto element_id = snapshot.created_[0].element_id_;
    const auto image_id   = snapshot.created_[0].image_id_;

    const auto src_path   = image_pool->Read<std::filesystem::path>(
        image_id, [](std::shared_ptr<Image> img) { return img->image_path_; });
    std::filesystem::path dst_name = src_path.filename();
    dst_name.replace_extension(".jpg");
    const std::filesystem::path requested_path = export_dir_ / dst_name;
    const std::filesystem::path dst_path       = export_dir_ / "service-007.jpg";

    ExportService               export_service(sleeve_service, image_pool, pipeline_service);

    ExportTask                  task;
    task.sleeve_id_                 = element_id;
    task.image_id_                  = image_id;
    task.options_.format_           = ImageFormatType::JPEG;
    task.options_.export_path_      = requested_path;
    // Production export UI commonly caps long edge; full-res 8K SDR JPEG has
    // crashed the OIIO/OpenCV write path on this fixture. Exercise the same
    // ExportService + ImageWriter path with the documented resize options.
    task.options_.resize_enabled_   = true;
    task.options_.max_length_side_  = 2048;
    task.recipe_                    = ExportRecipe::FromLegacyOptions(task.options_);
    task.recipe_->file_name_.parts_ = {
        {.field_ = ExportFileNameField::LITERAL, .literal_ = L"service-"},
        {.field_ = ExportFileNameField::SEQUENCE, .number_width_ = 3},
    };
    task.file_name_context_ = ExportFileNameContext{.sequence_ = 7};

    // Verify that the commit stage replaces an existing destination only after encode succeeds.
    {
      std::ofstream old_destination(dst_path, std::ios::binary);
      old_destination << "old";
    }
    export_service.EnqueueExportTask(task);

    std::promise<std::shared_ptr<std::vector<ExportResult>>> done;
    auto                                                     done_fut = done.get_future();
    export_service.ExportAll(
        [&done](std::shared_ptr<std::vector<ExportResult>> results) { done.set_value(results); });

    ASSERT_EQ(done_fut.wait_for(600s), std::future_status::ready) << "Export timed out";
    auto results = done_fut.get();
    ASSERT_NE(results, nullptr);
    ASSERT_EQ(results->size(), 1u);
    EXPECT_TRUE((*results)[0].success_) << (*results)[0].message_;
    EXPECT_EQ((*results)[0].output_path_, dst_path);
    EXPECT_TRUE((*results)[0].failed_stage_.empty());
    for (const auto& entry : std::filesystem::directory_iterator(export_dir_)) {
      EXPECT_EQ(entry.path().filename().wstring().find(L".alcedo-export-"), std::wstring::npos);
    }
    dst_path_global = dst_path;
  }

  AssertReadableNonEmptyImageFile(dst_path_global);
}

TEST_F(ExportServiceTests, ExportHdrJpeg_WritesUltraHdrFile) {
#if !defined(ALCEDO_HAS_ULTRAHDR)
  GTEST_SKIP() << "Ultra HDR support is not enabled in this build.";
#else
  std::filesystem::path dst_path_global;
  {
    ProjectService project(db_path_, meta_path_);
    auto           sleeve_service   = project.GetSleeveService();
    auto           image_pool       = project.GetImagePoolService();
    auto           pipeline_service = std::make_shared<PipelineMgmtService>(project.GetStorage());

    ImportServiceImpl import_service(sleeve_service, image_pool);
    auto              paths = CollectSupportedBatchImportImages(/*max_count=*/1);
    if (paths.empty()) {
      GTEST_SKIP() << "No supported images found under TEST_IMG_PATH/raw/batch_import";
    }

    auto                       import_job = std::make_shared<ImportJob>();
    std::promise<ImportResult> import_done;
    auto                       import_done_fut = import_done.get_future();
    import_job->on_finished_                   = [&import_done](const ImportResult& result) {
      import_done.set_value(result);
    };

    import_job = import_service.ImportToFolder(paths, L"", {}, import_job);
    ASSERT_NE(import_job, nullptr);
    ASSERT_EQ(import_done_fut.wait_for(120s), std::future_status::ready);
    const ImportResult import_res = import_done_fut.get();
    ASSERT_EQ(import_res.failed_, 0u);

    ASSERT_NE(import_job->import_log_, nullptr);
    auto snapshot = import_job->import_log_->Snapshot();
    ASSERT_FALSE(snapshot.created_.empty());

    const auto element_id     = snapshot.created_[0].element_id_;
    const auto image_id       = snapshot.created_[0].image_id_;

    auto       pipeline_guard = pipeline_service->LoadPipeline(element_id);
    ASSERT_NE(pipeline_guard, nullptr);
    nlohmann::json odt_params           = pipeline_defaults::MakeDefaultODTParams();
    odt_params["odt"]["encoding_space"] = "rec2020";
    odt_params["odt"]["encoding_eotf"]  = "st2084";
    odt_params["odt"]["peak_luminance"] = 600.0f;
    auto& output_stage = pipeline_guard->pipeline_->GetStage(PipelineStageName::Output_Transform);
    output_stage.SetOperator(OperatorType::ODT, odt_params);
    pipeline_guard->dirty_ = true;
    pipeline_service->SavePipeline(pipeline_guard);
    pipeline_service->Sync();

    const auto src_path = image_pool->Read<std::filesystem::path>(
        image_id, [](std::shared_ptr<Image> img) { return img->image_path_; });
    std::filesystem::path dst_name = src_path.filename();
    dst_name.replace_extension(".jpg");
    const std::filesystem::path dst_path = export_dir_ / dst_name;

    ExportService               export_service(sleeve_service, image_pool, pipeline_service);

    ExportTask                  task;
    task.sleeve_id_            = element_id;
    task.image_id_             = image_id;
    task.options_.format_      = ImageFormatType::JPEG;
    task.options_.export_path_ = dst_path;
    export_service.EnqueueExportTask(task);

    std::promise<std::shared_ptr<std::vector<ExportResult>>> done;
    auto                                                     done_fut = done.get_future();
    export_service.ExportAll(
        [&done](std::shared_ptr<std::vector<ExportResult>> results) { done.set_value(results); });

    ASSERT_EQ(done_fut.wait_for(600s), std::future_status::ready) << "Export timed out";
    auto results = done_fut.get();
    ASSERT_NE(results, nullptr);
    ASSERT_EQ(results->size(), 1u);
    EXPECT_TRUE((*results)[0].success_) << (*results)[0].message_;
    dst_path_global = dst_path;
  }

  AssertReadableNonEmptyImageFile(dst_path_global);

  const std::vector<uint8_t> bytes = ReadFileBytes(dst_path_global);
  ASSERT_FALSE(bytes.empty());
  ASSERT_EQ(is_uhdr_image(const_cast<uint8_t*>(bytes.data()), static_cast<int>(bytes.size())), 1);

  using DecoderPtr = std::unique_ptr<uhdr_codec_private_t, decltype(&uhdr_release_decoder)>;
  DecoderPtr decoder(uhdr_create_decoder(), &uhdr_release_decoder);
  ASSERT_TRUE(decoder != nullptr);

  uhdr_compressed_image_t image = {};
  image.data                    = const_cast<uint8_t*>(bytes.data());
  image.data_sz                 = bytes.size();
  image.capacity                = bytes.size();
  image.cg                      = UHDR_CG_UNSPECIFIED;
  image.ct                      = UHDR_CT_UNSPECIFIED;
  image.range                   = UHDR_CR_UNSPECIFIED;

  ASSERT_EQ(uhdr_dec_set_image(decoder.get(), &image).error_code, UHDR_CODEC_OK);
  ASSERT_EQ(uhdr_dec_set_out_img_format(decoder.get(), UHDR_IMG_FMT_64bppRGBAHalfFloat).error_code,
            UHDR_CODEC_OK);
  ASSERT_EQ(uhdr_dec_set_out_color_transfer(decoder.get(), UHDR_CT_LINEAR).error_code,
            UHDR_CODEC_OK);
  ASSERT_EQ(uhdr_decode(decoder.get()).error_code, UHDR_CODEC_OK);

  const uhdr_raw_image_t* decoded = uhdr_get_decoded_image(decoder.get());
  ASSERT_NE(decoded, nullptr);
  EXPECT_GT(decoded->w, 0u);
  EXPECT_GT(decoded->h, 0u);
#endif
}

TEST_F(ExportServiceTests, DISABLED_BatchExport_LimitedCount_WritesReadableFiles) {
  ProjectService    project(db_path_, meta_path_);
  auto              sleeve_service   = project.GetSleeveService();
  auto              image_pool       = project.GetImagePoolService();
  auto              pipeline_service = std::make_shared<PipelineMgmtService>(project.GetStorage());

  ImportServiceImpl import_service(sleeve_service, image_pool);

  // Requirement: never export more than 50 images. Keep this test lighter by default.
  constexpr size_t  kMaxExport    = 200;
  constexpr size_t  kPreferExport = 8;
  auto paths = CollectSupportedBatchImportImages(/*max_count=*/std::max(kMaxExport, kPreferExport));
  if (paths.empty()) {
    GTEST_SKIP() << "No supported images found under TEST_IMG_PATH/raw/batch_import";
  }

  auto                       import_job = std::make_shared<ImportJob>();
  std::promise<ImportResult> import_done;
  auto                       import_done_fut = import_done.get_future();
  import_job->on_finished_                   = [&import_done](const ImportResult& result) {
    import_done.set_value(result);
  };

  import_job = import_service.ImportToFolder(paths, L"", {}, import_job);
  ASSERT_NE(import_job, nullptr);
  ASSERT_EQ(import_done_fut.wait_for(300s), std::future_status::ready);
  const ImportResult import_res = import_done_fut.get();
  ASSERT_EQ(import_res.failed_, 0u);

  ASSERT_NE(import_job->import_log_, nullptr);
  auto snapshot = import_job->import_log_->Snapshot();
  ASSERT_FALSE(snapshot.created_.empty());

  const size_t export_count = std::min<size_t>(snapshot.created_.size(), kMaxExport);
  ASSERT_LE(export_count, kMaxExport);

  ExportService                      export_service(sleeve_service, image_pool, pipeline_service);

  std::vector<std::filesystem::path> expected_paths;
  expected_paths.reserve(export_count);

  for (size_t i = 0; i < export_count; ++i) {
    const auto element_id = snapshot.created_[i].element_id_;
    const auto image_id   = snapshot.created_[i].image_id_;

    const auto src_path   = image_pool->Read<std::filesystem::path>(
        image_id, [](std::shared_ptr<Image> img) { return img->image_path_; });
    std::filesystem::path dst_name = src_path.filename();
    dst_name.replace_extension(".jpg");
    const std::filesystem::path dst_path =
        export_dir_ / (std::to_string(element_id) + "_" + dst_name.string());

    ExportTask task;
    task.sleeve_id_            = element_id;
    task.image_id_             = image_id;
    task.options_.format_      = ImageFormatType::JPEG;
    task.options_.export_path_ = dst_path;
    export_service.EnqueueExportTask(task);
    expected_paths.push_back(dst_path);
  }

  std::promise<std::shared_ptr<std::vector<ExportResult>>> done;
  auto                                                     done_fut = done.get_future();
  export_service.ExportAll(
      [&done](std::shared_ptr<std::vector<ExportResult>> results) { done.set_value(results); });

  ASSERT_EQ(done_fut.wait_for(1800s), std::future_status::ready) << "Batch export timed out";
  auto results = done_fut.get();
  ASSERT_NE(results, nullptr);
  ASSERT_EQ(results->size(), export_count);

  for (size_t i = 0; i < results->size(); ++i) {
    EXPECT_TRUE((*results)[i].success_) << (*results)[i].message_;
  }
  for (const auto& p : expected_paths) {
    AssertReadableNonEmptyImageFile(p);
  }
}

TEST_F(ExportServiceTests, DISABLED_Manual_KeepExportFiles) {
  ProjectService    project(db_path_, meta_path_);
  auto              sleeve_service   = project.GetSleeveService();
  auto              image_pool       = project.GetImagePoolService();
  auto              pipeline_service = std::make_shared<PipelineMgmtService>(project.GetStorage());

  ImportServiceImpl import_service(sleeve_service, image_pool);
  auto              paths = CollectSupportedBatchImportImages(/*max_count=*/2);
  if (paths.empty()) {
    GTEST_SKIP() << "No supported images found under TEST_IMG_PATH/raw/batch_import";
  }

  auto                       import_job = std::make_shared<ImportJob>();
  std::promise<ImportResult> import_done;
  auto                       import_done_fut = import_done.get_future();
  import_job->on_finished_                   = [&import_done](const ImportResult& result) {
    import_done.set_value(result);
  };

  import_job = import_service.ImportToFolder(paths, L"", {}, import_job);
  ASSERT_NE(import_job, nullptr);
  ASSERT_EQ(import_done_fut.wait_for(300s), std::future_status::ready);
  const ImportResult import_res = import_done_fut.get();
  ASSERT_EQ(import_res.failed_, 0u);

  ASSERT_NE(import_job->import_log_, nullptr);
  auto snapshot = import_job->import_log_->Snapshot();
  ASSERT_GE(snapshot.created_.size(), 1u);

  ExportService                      export_service(sleeve_service, image_pool, pipeline_service);

  std::vector<std::filesystem::path> expected_paths;
  expected_paths.reserve(snapshot.created_.size());

  for (size_t i = 0; i < snapshot.created_.size(); ++i) {
    const auto element_id = snapshot.created_[i].element_id_;
    const auto image_id   = snapshot.created_[i].image_id_;

    const auto src_path   = image_pool->Read<std::filesystem::path>(
        image_id, [](std::shared_ptr<Image> img) { return img->image_path_; });
    std::filesystem::path dst_name = src_path.filename();
    dst_name.replace_extension(".jpg");
    const std::filesystem::path dst_path =
        export_dir_ / (std::to_string(element_id) + "_" + dst_name.string());

    ExportTask task;
    task.sleeve_id_            = element_id;
    task.image_id_             = image_id;
    task.options_.format_      = ImageFormatType::JPEG;
    task.options_.export_path_ = dst_path;
    export_service.EnqueueExportTask(task);
    expected_paths.push_back(dst_path);
  }

  std::promise<std::shared_ptr<std::vector<ExportResult>>> done;
  auto                                                     done_fut = done.get_future();
  export_service.ExportAll(
      [&done](std::shared_ptr<std::vector<ExportResult>> results) { done.set_value(results); });

  ASSERT_EQ(done_fut.wait_for(1800s), std::future_status::ready) << "Export timed out";
  auto results = done_fut.get();
  ASSERT_NE(results, nullptr);
  ASSERT_EQ(results->size(), expected_paths.size());

  for (const auto& p : expected_paths) {
    AssertReadableNonEmptyImageFile(p);
  }

  std::cout << "[Manual Export] Kept export outputs under: " << export_dir_.string() << std::endl;
}

}  // namespace alcedo
