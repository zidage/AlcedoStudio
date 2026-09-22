//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// G10.1: frame geometry for the Geometry panel (DocumentGeometryUse), FAST_PREVIEW ROI pixels for
// a rotated crop without the removed stage rotation check, and cancel propagation without a stage
// write. Every render goes through the GPU DAG product renderer.

#include <cuda_runtime.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <future>
#include <memory>
#include <opencv2/core.hpp>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "../graph/test_camera_profile.hpp"
#include "../input/prepared_raw_test_support.hpp"
#include "edit/geometry/render_request.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/input/raw_input_loader.hpp"
#include "edit/operators/operator_registeration.hpp"
#include "edit/pipeline/pipeline_accelerator.hpp"
#include "edit/pipeline/pipeline_cpu.hpp"
#include "edit/runtime/cuda/cuda_product_renderer.hpp"
#include "image/image_buffer.hpp"
#include "renderer/pipeline_scheduler.hpp"
#include "renderer/pipeline_task.hpp"
#include "ui/edit_viewer/frame_sink.hpp"

namespace alcedo {
namespace {

constexpr std::uint32_t kSourceWidth    = 96;
constexpr std::uint32_t kSourceHeight   = 64;
constexpr float         kPixelTolerance = 1.0f / 1024.0f;

auto                    HasCudaDevice() -> bool {
  int count = 0;
  return ::cudaGetDeviceCount(&count) == cudaSuccess && count > 0;
}

auto MakeEncodedImage(std::uint8_t tag) -> std::shared_ptr<ImageBuffer> {
  std::vector<std::uint8_t> bytes(64, tag);
  bytes[1] = 0x5A;
  return std::make_shared<ImageBuffer>(std::move(bytes));
}

auto MakeUnpacker() -> PreparedSourceCache::UnpackFn {
  return [](std::span<const std::byte>, DecodeRes decode_res) {
    const auto pattern = gpu_dag_test::MakeRggbPattern();
    return RawInputLoader::FromUnpackedCfa(
        gpu_dag_test::MakeU16CfaPlane(kSourceWidth, kSourceHeight, pattern), pattern,
        gpu_dag_test::DefaultLinearization(), gpu_dag_test::FullSensor(kSourceWidth, kSourceHeight),
        decode_res);
  };
}

auto HostPixels(const std::shared_ptr<ImageBuffer>& image) -> cv::Mat {
  if (!image || !image->cpu_data_valid_) {
    return {};
  }
  return image->GetCPUData();
}

/** @brief Largest absolute RGB difference; infinity when sizes, types, or finiteness differ. */
auto MaxRgbDifference(const cv::Mat& a, const cv::Mat& b) -> float {
  if (a.empty() || b.empty() || a.size() != b.size() || a.type() != CV_32FC4 ||
      b.type() != CV_32FC4) {
    return INFINITY;
  }
  float max_difference = 0.0f;
  for (int row = 0; row < a.rows; ++row) {
    const auto* pa = a.ptr<cv::Vec4f>(row);
    const auto* pb = b.ptr<cv::Vec4f>(row);
    for (int col = 0; col < a.cols; ++col) {
      for (int channel = 0; channel < 3; ++channel) {
        if (!std::isfinite(pa[col][channel]) || !std::isfinite(pb[col][channel])) {
          return INFINITY;
        }
        max_difference = std::max(max_difference, std::abs(pa[col][channel] - pb[col][channel]));
      }
    }
  }
  return max_difference;
}

auto BitwiseEqual(const cv::Mat& a, const cv::Mat& b) -> bool {
  if (a.empty() || a.size() != b.size() || a.type() != b.type()) {
    return false;
  }
  for (int row = 0; row < a.rows; ++row) {
    if (std::memcmp(a.ptr(row), b.ptr(row), a.cols * a.elemSize()) != 0) {
      return false;
    }
  }
  return true;
}

class CudaDocumentGeometryRequestFixture : public ::testing::Test {
 protected:
  void SetUp() override {
    if (!HasCudaDevice()) {
      GTEST_SKIP() << "No CUDA device available.";
    }
    document_ = std::make_shared<PipelineDocument>(CreateDefaultPipelineDocument());
    gpu_dag_test::EnsureTestCameraProfile(*document_);
    document_->Geometry().SetCropRect({0.2f, 0.1f, 0.5f, 0.6f});
    document_->Geometry().SetRotationDegrees(7.0f);
    renderer_ = std::make_unique<CudaProductRenderer>(document_, MakeUnpacker());
    image_    = MakeEncodedImage(83);
  }

  auto Render(CudaProductRenderer& renderer, const RenderRequest& request) -> cv::Mat {
    return HostPixels(renderer.Render(image_, DecodeRes::FULL, request, nullptr,
                                      FrameCompletionSubmission{}, true));
  }

  auto Render(const RenderRequest& request = {}) -> cv::Mat { return Render(*renderer_, request); }

  static auto Uncropped() -> RenderRequest {
    RenderRequest request;
    request.document_geometry = DocumentGeometryUse::UncroppedSource;
    return request;
  }

  std::shared_ptr<PipelineDocument>    document_;
  std::unique_ptr<CudaProductRenderer> renderer_;
  std::shared_ptr<ImageBuffer>         image_;
};

TEST_F(CudaDocumentGeometryRequestFixture, RotatedCropFastPreviewRoiMatchesFullFramePixels) {
  const auto full = Render();
  ASSERT_FALSE(full.empty());
  ASSERT_GE(full.cols, 8);
  ASSERT_GE(full.rows, 8);

  // Pixel-aligned quarter-area region in edit space, rendered at 1:1 like a FAST_PREVIEW ROI.
  const int     x = full.cols / 4;
  const int     y = full.rows / 4;
  const int     w = full.cols / 2;
  const int     h = full.rows / 2;
  RenderRequest roi;
  roi.view.visible_rect_in_edit_space = {static_cast<float>(x) / static_cast<float>(full.cols),
                                         static_cast<float>(y) / static_cast<float>(full.rows),
                                         static_cast<float>(w) / static_cast<float>(full.cols),
                                         static_cast<float>(h) / static_cast<float>(full.rows)};
  roi.view.viewport_extent = {static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h)};

  const auto roi_pixels    = Render(roi);
  ASSERT_EQ(roi_pixels.cols, w);
  ASSERT_EQ(roi_pixels.rows, h);
  EXPECT_LE(MaxRgbDifference(roi_pixels, full(cv::Rect(x, y, w, h))), kPixelTolerance);
}

TEST_F(CudaDocumentGeometryRequestFixture,
       GeometryPanelFrameShowsUncroppedSourceAndReusesSensorDevelop) {
  const auto document_before = document_->ToJson();
  const auto cropped         = Render();
  ASSERT_FALSE(cropped.empty());

  renderer_->ResetStats();
  const auto uncropped = Render(Uncropped());
  ASSERT_FALSE(uncropped.empty());
  const auto stats = renderer_->Stats();
  EXPECT_EQ(stats.pass.sensor_develop_execute, 0U);
  EXPECT_EQ(stats.pass.sensor_develop_skip, 1U);
  EXPECT_EQ(stats.pass.source_h2d_count, 0U);
  EXPECT_EQ(stats.pass.geometry_execute, 1U);

  // Reference: the same source rendered from a document with identity geometry. Its extent is the
  // developed source extent, so equal size means the frame has the uncropped source aspect ratio.
  auto identity = std::make_shared<PipelineDocument>(CreateDefaultPipelineDocument());
  gpu_dag_test::EnsureTestCameraProfile(*identity);
  CudaProductRenderer identity_renderer(identity, MakeUnpacker());
  const auto          expected = Render(identity_renderer, RenderRequest{});
  ASSERT_FALSE(expected.empty());
  EXPECT_EQ(uncropped.size(), expected.size());
  EXPECT_FALSE(uncropped.size() == cropped.size());
  EXPECT_LE(MaxRgbDifference(uncropped, expected), kPixelTolerance);

  EXPECT_EQ(document_->ToJson(), document_before);
}

TEST_F(CudaDocumentGeometryRequestFixture, ClosingGeometryPanelRendersDocumentCropAgain) {
  const auto first_cropped = Render();
  ASSERT_FALSE(first_cropped.empty());
  ASSERT_FALSE(Render(Uncropped()).empty());

  const auto after_close = Render();
  EXPECT_EQ(after_close.size(), first_cropped.size());
  EXPECT_TRUE(BitwiseEqual(after_close, first_cropped));
}

TEST(GpuDagCudaDrtProduct, CancelRequestReachesRendererWithoutStageWrite) {
  if (!HasCudaDevice()) {
    GTEST_SKIP() << "No CUDA device available.";
  }
  RegisterAllOperators();
  auto exec = std::make_shared<CPUPipelineExecutor>();
  exec->SetAcceleratorBackendPreference(AcceleratorBackendPreference::CUDA);
  auto document = std::make_shared<PipelineDocument>(CreateDefaultPipelineDocument());
  gpu_dag_test::EnsureTestCameraProfile(*document);
  exec->SetPipelineDocument(document);

  bool cancel_calls_seen = false;
  auto cancel            = [&cancel_calls_seen]() {
    cancel_calls_seen = true;
    return true;
  };

  PipelineTask task;
  task.input_                             = MakeEncodedImage(91);
  task.pipeline_executor_                 = exec;
  task.options_.render_desc_.render_type_ = RenderType::QUALITY_BASE_PREVIEW;
  task.cancel_requested_                  = cancel;

  // The request carries the callback that the renderer path receives.
  const auto request                      = task.MakeApplyRequest();
  ASSERT_TRUE(static_cast<bool>(request.cancel_requested));
  EXPECT_TRUE(request.cancel_requested());
  cancel_calls_seen = false;

  auto result       = std::make_shared<std::promise<std::pair<bool, std::string>>>();
  auto completed    = result->get_future();
  task.on_complete_ = [result](bool success, std::string message) {
    result->set_value({success, std::move(message)});
  };
  PipelineScheduler scheduler(1);
  scheduler.ScheduleTask(std::move(task));

  ASSERT_EQ(completed.wait_for(std::chrono::seconds(30)), std::future_status::ready);
  const auto [success, message] = completed.get();
  EXPECT_FALSE(success);
  EXPECT_TRUE(message.empty()) << message;
  EXPECT_TRUE(cancel_calls_seen);
  // Cancelled before Apply: no product renderer was created and no frame was rendered.
  EXPECT_EQ(exec->DebugCudaRenderer(), nullptr);
}

}  // namespace
}  // namespace alcedo
