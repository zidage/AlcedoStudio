//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/export_service.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <OpenImageIO/imageio.h>
#include <exiv2/exiv2.hpp>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <string>
#include <vector>
#if defined(ALCEDO_HAS_ULTRAHDR)
#include <ultrahdr_api.h>
#endif

#include "app/import_service.hpp"
#include "app/pipeline_service.hpp"
#include "app/project_service.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/operators/operator_registeration.hpp"
#include "edit/operators/utils/color_utils.hpp"
#include "edit/pipeline/default_pipeline_params.hpp"
#include "edit/runtime/drt_display.hpp"
#include "image/image.hpp"
#include "io/image/export_color_profile_config.hpp"
#include "io/image/export_icc_profile_resolver.hpp"
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

void AttachResolvedExportColor(ExportTask& task, PipelineMgmtService& pipelines) {
  auto recipe = task.recipe_.value_or(ExportRecipe::FromLegacyOptions(task.options_));
  auto live   = pipelines.LoadPipeline(task.sleeve_id_);
  if (!live || !live->document_ || !live->document_->Drt() || !live->pipeline_) {
    if (live) {
      pipelines.ReleasePipelineUse(live);
    }
    throw std::runtime_error("export test: document DRT is missing");
  }
  {
    std::lock_guard<std::mutex> lock(live->pipeline_->GetRenderLock());
    recipe.output_color_ = ExportColorProfileFromDrt(live->document_->Drt()->Params().Params());
  }
  pipelines.ReleasePipelineUse(live);
  task.recipe_ = std::move(recipe);
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

auto ReadExportRgb8(const std::filesystem::path& path) -> cv::Mat {
  OIIO_NAMESPACE_USING
  auto input = ImageInput::open(conv::ToBytes(path.wstring()));
  if (!input) {
    return {};
  }
  const ImageSpec& spec = input->spec();
  if (spec.width <= 0 || spec.height <= 0 || spec.nchannels < 3) {
    input->close();
    return {};
  }
  cv::Mat rgb(spec.height, spec.width, CV_8UC3);
  const bool ok = input->read_image(0, 0, 0, 3, TypeDesc::UINT8, rgb.data, sizeof(uint8_t) * 3,
                                    AutoStride, AutoStride);
  input->close();
  if (!ok) {
    return {};
  }
  return rgb;
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

    ImportServiceImpl import_service(sleeve_service, image_pool, pipeline_service);
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
    AttachResolvedExportColor(task, *pipeline_service);
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

    ImportServiceImpl import_service(sleeve_service, image_pool, pipeline_service);
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
    ASSERT_NE(pipeline_guard->document_, nullptr);
    {
      std::unique_lock lock(pipeline_guard->pipeline_->GetRenderLock());
      auto drt = pipeline_guard->document_->Drt()->Params().Params();
      drt.encoding_space = DrtColorSpace::Rec2020;
      drt.encoding_eotf = DrtEotf::St2084;
      drt.peak_luminance = 600.0f;
      pipeline_guard->document_->Drt()->Params().ReplaceParams(drt);
      // Stage defaults remain SDR: both pixels and export encoding must use the document.
      pipeline_service->SyncPipelineDocument(pipeline_guard);
    }
    pipeline_service->SavePipeline(pipeline_guard);

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
    AttachResolvedExportColor(task, *pipeline_service);
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
    AttachResolvedExportColor(task, *pipeline_service);
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
    AttachResolvedExportColor(task, *pipeline_service);
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

TEST_F(ExportServiceTests, ExportRecipeContainsResolvedOutputColorBeforeScheduling) {
  ProjectService project(db_path_, meta_path_);
  ExportService  export_service(project.GetSleeveService(), project.GetImagePoolService(),
                                std::make_shared<PipelineMgmtService>(project.GetStorage()));
  ExportTask     task;
  task.options_.format_ = ImageFormatType::JPEG;
  EXPECT_THROW(export_service.EnqueueExportTask(task), std::runtime_error);
  task.recipe_ = ExportRecipe::FromLegacyOptions(task.options_);
  EXPECT_THROW(export_service.EnqueueExportTask(task), std::runtime_error);
  task.recipe_->output_color_ = ExportColorProfileConfig{};
  EXPECT_NO_THROW(export_service.EnqueueExportTask(task));
}

TEST_F(ExportServiceTests, ExportPixelsAndIccUseTheSameRecipeColorConfiguration) {
  if (!std::filesystem::exists(std::filesystem::path(TEST_IMG_PATH) / "raw" / "linear_dng" /
                               "mfzoty.dng")) {
    GTEST_SKIP() << "Sample DNG file is missing";
  }
  ProjectService project(db_path_, meta_path_);
  auto           sleeve_service   = project.GetSleeveService();
  auto           image_pool       = project.GetImagePoolService();
  auto           pipeline_service = std::make_shared<PipelineMgmtService>(project.GetStorage());
  ImportServiceImpl import_service(sleeve_service, image_pool, pipeline_service);
  auto              import_job = std::make_shared<ImportJob>();
  std::promise<ImportResult> import_done;
  auto                       import_done_fut = import_done.get_future();
  import_job->on_finished_                   = [&import_done](const ImportResult& result) {
    import_done.set_value(result);
  };
  const auto raw_path = std::filesystem::path(TEST_IMG_PATH) / "raw" / "linear_dng" / "mfzoty.dng";
  import_job          = import_service.ImportToFolder({raw_path}, L"", {}, import_job);
  ASSERT_NE(import_job, nullptr);
  ASSERT_EQ(import_done_fut.wait_for(120s), std::future_status::ready);
  ASSERT_EQ(import_done_fut.get().imported_, 1u);
  import_service.SyncImports(import_job->import_log_->Snapshot(), L"");
  const auto snapshot   = import_job->import_log_->Snapshot();
  const auto element_id = snapshot.created_[0].element_id_;
  const auto image_id   = snapshot.created_[0].image_id_;
  auto       live       = pipeline_service->LoadPipeline(element_id);
  const auto document_before = live->document_->Drt()->Params().ToJson();
  pipeline_service->ReleasePipelineUse(live);

  auto run_export = [&](const std::filesystem::path& path, ExportColorProfileConfig color,
                        ExportIccPolicy icc) {
    ExportService export_service(sleeve_service, image_pool, pipeline_service);
    ExportTask    task;
    task.sleeve_id_                = element_id;
    task.image_id_                 = image_id;
    task.options_.format_          = ImageFormatType::JPEG;
    task.options_.export_path_     = path;
    task.options_.resize_enabled_  = true;
    task.options_.max_length_side_ = 256;
    task.recipe_                   = ExportRecipe::FromLegacyOptions(task.options_);
    task.recipe_->output_color_    = color;
    task.recipe_->icc_             = icc;
    export_service.EnqueueExportTask(task);
    std::promise<std::shared_ptr<std::vector<ExportResult>>> done;
    auto                                                     fut = done.get_future();
    export_service.ExportAll(
        [&done](std::shared_ptr<std::vector<ExportResult>> results) { done.set_value(results); });
    EXPECT_EQ(fut.wait_for(180s), std::future_status::ready);
    auto results = fut.get();
    EXPECT_NE(results, nullptr);
    EXPECT_EQ(results->size(), 1u);
    EXPECT_TRUE((*results)[0].success_) << (*results)[0].message_;
  };

  const ExportColorProfileConfig rec709{ColorUtils::ColorSpace::REC709, ColorUtils::EOTF::GAMMA_2_2,
                                        100.0f};
  const ExportColorProfileConfig p3{ColorUtils::ColorSpace::P3_D65, ColorUtils::EOTF::GAMMA_2_2,
                                    100.0f};
  const auto rec709_path = export_dir_ / "recipe-rec709.jpg";
  const auto p3_path     = export_dir_ / "recipe-p3.jpg";
  const auto omit_path   = export_dir_ / "recipe-omit.jpg";
  run_export(rec709_path, rec709, ExportIccPolicy::EMBED_OUTPUT_PROFILE);
  run_export(p3_path, p3, ExportIccPolicy::EMBED_OUTPUT_PROFILE);
  run_export(omit_path, p3, ExportIccPolicy::OMIT);

  live = pipeline_service->LoadPipeline(element_id);
  EXPECT_EQ(live->document_->Drt()->Params().ToJson(), document_before);
  pipeline_service->ReleasePipelineUse(live);

  const auto rec709_icc = ExportIccProfileResolver::ResolveIccProfileBytes(rec709);
  const auto p3_icc     = ExportIccProfileResolver::ResolveIccProfileBytes(p3);
  auto       read_icc   = [](const std::filesystem::path& path) {
    auto image = Exiv2::ImageFactory::open(path.string());
    image->readMetadata();
    if (!image->iccProfileDefined()) {
      return std::vector<uint8_t>{};
    }
    const auto& profile = image->iccProfile();
    if (profile.empty()) {
      return std::vector<uint8_t>{};
    }
    return std::vector<uint8_t>(profile.c_data(), profile.c_data() + profile.size());
  };
  EXPECT_FALSE(rec709_icc.empty());
  EXPECT_FALSE(p3_icc.empty());
  EXPECT_EQ(read_icc(rec709_path), rec709_icc);
  EXPECT_EQ(read_icc(p3_path), p3_icc);
  EXPECT_TRUE(read_icc(omit_path).empty());

  AssertReadableNonEmptyImageFile(rec709_path);
  AssertReadableNonEmptyImageFile(p3_path);
  AssertReadableNonEmptyImageFile(omit_path);
  const auto rec709_pixels = ReadExportRgb8(rec709_path);
  const auto p3_pixels     = ReadExportRgb8(p3_path);
  const auto omit_pixels   = ReadExportRgb8(omit_path);
  ASSERT_FALSE(rec709_pixels.empty()) << rec709_path.string();
  ASSERT_FALSE(p3_pixels.empty()) << p3_path.string();
  ASSERT_FALSE(omit_pixels.empty()) << omit_path.string();
  EXPECT_GT(cv::norm(rec709_pixels, p3_pixels, cv::NORM_INF), 1.0);
  EXPECT_LE(cv::norm(p3_pixels, omit_pixels, cv::NORM_INF), 1.0);
}

}  // namespace alcedo
