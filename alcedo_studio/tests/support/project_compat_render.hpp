//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// One render of a committed image history, hashed over its host pixels. The compatibility
// fixture under tests/resources/compat records this hash from the build that wrote the
// project, and ProjectCompatibilityTest compares it with the current build
// (raster_image_input_plan.md, section 7.4). Keep the render request unchanged: the recorded
// hash depends on it.

#pragma once

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <mutex>
#include <opencv2/core/mat.hpp>

#include "app/image_pool_service.hpp"
#include "app/pipeline_service.hpp"
#include "edit/pipeline/pipeline_executor.hpp"
#include "edit/runtime/pipeline_apply_request.hpp"
#include "image/image.hpp"
#include "image/image_buffer.hpp"
#include "io/image/image_loader.hpp"

namespace alcedo::test {

/// FNV-1a over the bytes of every row of @p mat.
inline auto HashMatRows(const cv::Mat& mat) -> uint64_t {
  uint64_t     hash      = 14695981039346656037ull;
  const size_t row_bytes = static_cast<size_t>(mat.cols) * mat.elemSize();
  for (int row = 0; row < mat.rows; ++row) {
    const auto* bytes = mat.ptr<uint8_t>(row);
    for (size_t i = 0; i < row_bytes; ++i) {
      hash ^= static_cast<uint64_t>(bytes[i]);
      hash *= 1099511628211ull;
    }
  }
  return hash;
}

/// Render the committed history of @p element_id at 512 px on the batch executor and hash the
/// host pixels. Returns 0 after a test failure.
inline auto RenderCommittedElementHash(PipelineMgmtService& pipelines, ImagePoolService& pool,
                                       sl_element_id_t element_id, image_id_t image_id)
    -> uint64_t {
  auto image = pool.Read<std::shared_ptr<Image>>(
      image_id, [](const std::shared_ptr<Image>& img) { return img; });
  if (image == nullptr) {
    ADD_FAILURE() << "Image " << image_id << " is not in the pool";
    return 0;
  }
  const auto snapshot = pipelines.AcquireCommittedSnapshot(element_id);
  if (snapshot == nullptr) {
    ADD_FAILURE() << "Element " << element_id << " has no committed snapshot";
    return 0;
  }
  PipelineApplyRequest request;
  request.geometry.resolution.max_edge = 512;
  request.geometry.resolution.quality  = RenderQuality::Export;
  request.decode_res                   = DecodeRes::QUARTER;
  request.role                         = ExecutorRole::Batch;
  request.require_host_output          = true;

  auto bytes                           = ByteBufferLoader::LoadFromImage(image);
  if (bytes == nullptr) {
    ADD_FAILURE() << "Source bytes of image " << image_id << " cannot be read";
    return 0;
  }
  auto                         input    = std::make_shared<ImageBuffer>(std::move(*bytes));
  auto                         executor = std::make_shared<PipelineExecutor>(ExecutorRole::Batch);
  std::shared_ptr<ImageBuffer> result;
  {
    std::unique_lock<std::mutex> render_lock(executor->GetRenderLock());
    result = executor->Apply(*snapshot, input, request);
  }
  if (result == nullptr) {
    ADD_FAILURE() << "The render of element " << element_id << " returned no image";
    return 0;
  }
  if (!result->cpu_data_valid_ && result->gpu_data_valid_) {
    result->SyncToCPU();
  }
  const auto& mat = result->GetCPUData();
  if (mat.empty()) {
    ADD_FAILURE() << "The render of element " << element_id << " has no host pixels";
    return 0;
  }
  return HashMatRows(mat);
}

}  // namespace alcedo::test
