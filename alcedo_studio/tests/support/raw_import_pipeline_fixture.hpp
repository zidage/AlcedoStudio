//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <chrono>
#include <filesystem>
#include <future>
#include <memory>
#include <mutex>
#include <opencv2/core.hpp>
#include <utility>

#include "app/image_pool_service.hpp"
#include "app/import_service.hpp"
#include "app/pipeline_service.hpp"
#include "app/project_service.hpp"
#include "edit/graph/develop_color_transform.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/pipeline/pipeline_executor.hpp"
#include "image/image.hpp"
#include "image/image_buffer.hpp"
#include "io/image/image_loader.hpp"
#include "type/type.hpp"

/**
 * @file
 * @brief Import a sample RAW file into a test project and read pixels from pipeline output.
 *
 * Shared by the app-level pipeline tests that need a real imported RAW element (element id,
 * image id, stored root, and encoded bytes) rather than a synthetic document.
 */
namespace alcedo::raw_import_test {

/// Linear DNG sample used by the pipeline ownership tests.
inline auto LinearDngPath() -> std::filesystem::path {
  return std::filesystem::path(TEST_IMG_PATH) / "raw" / "linear_dng" / "mfzoty.dng";
}

/**
 * @brief Import one RAW file into the root folder and return its element and image ids.
 * @return `{0, 0}` when the file is missing or import does not finish within 60 s.
 */
inline auto ImportRawFile(ProjectService& project, std::shared_ptr<PipelineMgmtService> pipelines,
                          const std::filesystem::path& raw_path)
    -> std::pair<sl_element_id_t, image_id_t> {
  using namespace std::chrono_literals;
  if (!std::filesystem::exists(raw_path) || !pipelines) {
    return {0, 0};
  }
  auto              fs_service = project.GetSleeveService();
  auto              img_pool   = project.GetImagePoolService();
  ImportServiceImpl import_service(fs_service, img_pool, pipelines);
  auto              import_job = std::make_shared<ImportJob>();
  std::promise<ImportResult> imported;
  auto                       imported_future = imported.get_future();
  import_job->on_finished_                   = [&imported](const ImportResult& result) {
    imported.set_value(result);
  };
  import_job = import_service.ImportToFolder({raw_path}, L"", {}, import_job);
  if (!import_job) {
    return {0, 0};
  }
  if (imported_future.wait_for(60s) != std::future_status::ready) {
    return {0, 0};
  }
  if (imported_future.get().imported_ != 1u || !import_job->import_log_) {
    return {0, 0};
  }
  const auto snapshot = import_job->import_log_->Snapshot();
  if (snapshot.created_.size() != 1u) {
    return {0, 0};
  }
  import_service.SyncImports(snapshot, L"");
  project.GetSleeveService()->Sync();
  project.GetImagePoolService()->SyncWithStorage();
  return {snapshot.created_.front().element_id_, snapshot.created_.front().image_id_};
}

inline auto ImportLinearDng(ProjectService& project, std::shared_ptr<PipelineMgmtService> pipelines)
    -> std::pair<sl_element_id_t, image_id_t> {
  return ImportRawFile(project, std::move(pipelines), LinearDngPath());
}

/// Encoded source bytes of an imported image, as the render path consumes them.
inline auto LoadEncodedInput(ImagePoolService& pool, image_id_t image_id)
    -> std::shared_ptr<ImageBuffer> {
  auto img = pool.Read<std::shared_ptr<Image>>(
      image_id, [](const std::shared_ptr<Image>& image) { return image; });
  if (!img) {
    return nullptr;
  }
  return std::make_shared<ImageBuffer>(ByteBufferLoader::LoadByteBufferFromImage(img));
}

/// Copy of the host pixels of a render result; downloads from the GPU first when needed.
inline auto HostPixels(ImageBuffer& buffer) -> cv::Mat {
  if (!buffer.cpu_data_valid_ && buffer.gpu_data_valid_) {
    buffer.SyncToCPU();
  }
  return buffer.GetCPUData().clone();
}

/// Bind the imported RAW color context of @p image_id onto the live document of @p live.
inline void BindImportedRawColor(const std::shared_ptr<PipelineGuard>& live,
                                 ImagePoolService& pool, image_id_t image_id) {
  if (!live || !live->pipeline_) {
    return;
  }
  auto img = pool.Read<std::shared_ptr<Image>>(
      image_id, [](const std::shared_ptr<Image>& image) { return image; });
  if (!img || !img->HasRawColorContext()) {
    return;
  }
  std::lock_guard<std::mutex> render_lock(live->pipeline_->GetRenderLock());
  BindImportedCameraProfile(*live->document_, img->GetRawColorContext());
}

}  // namespace alcedo::raw_import_test
