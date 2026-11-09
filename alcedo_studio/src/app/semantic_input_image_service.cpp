//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/semantic_input_image_service.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <exception>
#include <format>
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <span>
#include <stdexcept>
#include <thread>
#include <utility>

#include "edit/input/raster_input_loader.hpp"
#include "image/image_buffer.hpp"
#include "io/image/embedded_preview_reader.hpp"
#include "io/image/image_loader.hpp"
#include "renderer/pipeline_task.hpp"

namespace alcedo {
namespace {

/// Render workers and batch executors of one service: edited images are a small share of a
/// library, and one executor keeps the GPU memory of a long run small.
constexpr size_t kRenderExecutorCount = 1;

/// Scales @p image down so that its shorter edge is @p short_edge. A smaller image keeps its size:
/// the model resizes it.
auto             ScaleToShortEdge(const cv::Mat& image, uint32_t short_edge) -> cv::Mat {
  const int current = std::min(image.cols, image.rows);
  if (current <= static_cast<int>(short_edge) || current <= 0) {
    return image;
  }
  const double scale  = static_cast<double>(short_edge) / static_cast<double>(current);
  const int    width  = std::max(1, static_cast<int>(std::lround(image.cols * scale)));
  const int    height = std::max(1, static_cast<int>(std::lround(image.rows * scale)));
  cv::Mat      scaled;
  cv::resize(image, scaled, cv::Size(width, height), 0.0, 0.0, cv::INTER_AREA);
  return scaled;
}

/// RGBA8 model input of @p rgb (CV_8UC3 or CV_8UC4, R, G, B order).
auto ToInputImage(const cv::Mat& rgb, uint32_t short_edge, SemanticInputImageSource source)
    -> SemanticInputImage {
  const cv::Mat scaled = ScaleToShortEdge(rgb, short_edge);
  cv::Mat       rgba8;
  if (scaled.channels() == 4) {
    rgba8 = scaled;
  } else {
    cv::cvtColor(scaled, rgba8, cv::COLOR_RGB2RGBA);
  }
  if (!rgba8.isContinuous()) {
    rgba8 = rgba8.clone();
  }
  SemanticInputImage input;
  input.width  = static_cast<uint32_t>(rgba8.cols);
  input.height = static_cast<uint32_t>(rgba8.rows);
  input.source = source;
  input.rgba8.assign(rgba8.data, rgba8.data + rgba8.total() * rgba8.elemSize());
  return input;
}

/// RGB or RGBA 8-bit pixels of a render result (32-bit float or 8-bit, R, G, B order).
auto RenderPixelsToRgb8(ImageBuffer& result) -> cv::Mat {
  if (!result.cpu_data_valid_) {
    if (!result.gpu_data_valid_) {
      throw std::runtime_error("the render produced no pixels");
    }
    result.SyncToCPU();
  }
  const cv::Mat& pixels = result.GetCPUData();
  if (pixels.empty() || (pixels.channels() != 3 && pixels.channels() != 4)) {
    throw std::runtime_error("the render produced pixels with an unsupported layout");
  }
  cv::Mat rgb8;
  if (pixels.depth() == CV_8U) {
    rgb8 = pixels;
  } else if (pixels.depth() == CV_32F) {
    pixels.convertTo(rgb8, CV_MAKETYPE(CV_8U, pixels.channels()), 255.0);
  } else {
    pixels.convertTo(rgb8, CV_MAKETYPE(CV_8U, pixels.channels()));
  }
  return rgb8;
}

/// Longest render edge that gives a shorter edge of @p short_edge for images up to 2:1.
auto RenderMaxEdge(uint32_t short_edge) -> uint32_t { return short_edge * 2; }

/// RAW decode resolution of a render with long edge @p max_edge, by the rule of the thumbnail
/// tiers: 1/8 up to 512 px, 1/4 up to 1024 px, 1/2 above.
auto DecodeResForMaxEdge(uint32_t max_edge) -> DecodeRes {
  if (max_edge <= 512) {
    return DecodeRes::EIGHTH;
  }
  if (max_edge <= 1024) {
    return DecodeRes::QUARTER;
  }
  return DecodeRes::HALF;
}

auto IsCanceled(const std::function<bool()>& canceled) -> bool {
  try {
    return canceled && canceled();
  } catch (...) {
    return true;
  }
}

/// Exactly one answer of one request, from whichever render path ends first.
struct RenderAnswer {
  sl_element_id_t            element_id = 0;
  image_id_t                 image_id   = 0;
  SemanticInputImageCallback callback;
  std::atomic<bool>          sent{false};

  void                       Send(SemanticInputImageResult result) {
    if (sent.exchange(true)) {
      return;
    }
    result.element_id = element_id;
    result.image_id   = image_id;
    callback(std::move(result));
  }
};

}  // namespace

auto SemanticInputImageService::DefaultWorkerCount() -> size_t {
  const size_t threads = std::max(1U, std::thread::hardware_concurrency());
  return std::clamp<size_t>(threads / 2, 2, 8);
}

SemanticInputImageService::SemanticInputImageService(std::shared_ptr<ImagePoolService> image_pool,
                                                     std::shared_ptr<PipelineMgmtService> pipelines,
                                                     uint32_t input_short_edge, size_t worker_count)
    : image_pool_(std::move(image_pool)),
      pipelines_(std::move(pipelines)),
      input_short_edge_(input_short_edge),
      worker_count_(std::max<size_t>(1, worker_count)),
      render_executors_(kRenderExecutorCount,
                        pipelines_ ? pipelines_->GetAcceleratorBackendPreference()
                                   : AcceleratorBackendPreference::Auto,
                        pipelines_ ? pipelines_->LutResources() : nullptr,
                        pipelines_ ? pipelines_->MaskAssets() : nullptr),
      render_scheduler_(kRenderExecutorCount),
      workers_(worker_count_) {
  if (!image_pool_ || !pipelines_) {
    throw std::invalid_argument(
        "SemanticInputImageService requires the image pool and the pipeline service");
  }
  if (input_short_edge_ == 0) {
    throw std::invalid_argument("SemanticInputImageService requires a model input size");
  }
}

SemanticInputImageService::~SemanticInputImageService() {
  // Workers first: a running preparation may still schedule a render.
  workers_.Shutdown();
  render_scheduler_.Shutdown();
}

auto SemanticInputImageService::ConcurrencyLimit() const -> size_t {
  // Two requests per worker: the next file is queued while a worker decodes.
  return worker_count_ * 2 + kRenderExecutorCount;
}

void SemanticInputImageService::Request(sl_element_id_t element_id, image_id_t image_id,
                                        std::function<bool()>      canceled,
                                        SemanticInputImageCallback callback) {
  workers_.Submit([this, element_id, image_id, canceled = std::move(canceled),
                   callback = std::move(callback)]() mutable {
    try {
      Prepare(element_id, image_id, canceled, callback);
    } catch (const std::exception& e) {
      callback(SemanticInputImageResult{.element_id = element_id,
                                        .image_id   = image_id,
                                        .status     = SemanticInputImageStatus::kError,
                                        .message    = e.what()});
    } catch (...) {
      callback(SemanticInputImageResult{.element_id = element_id,
                                        .image_id   = image_id,
                                        .status     = SemanticInputImageStatus::kError,
                                        .message    = "semantic input preparation failed"});
    }
  });
}

void SemanticInputImageService::Prepare(sl_element_id_t element_id, image_id_t image_id,
                                        const std::function<bool()>& canceled,
                                        SemanticInputImageCallback   callback) {
  const auto canceled_result = [element_id, image_id]() {
    return SemanticInputImageResult{.element_id = element_id,
                                    .image_id   = image_id,
                                    .status     = SemanticInputImageStatus::kCanceled,
                                    .message    = "semantic generation job was canceled"};
  };
  if (IsCanceled(canceled)) {
    callback(canceled_result());
    return;
  }

  auto snapshot = pipelines_->AcquireCommittedSnapshot(element_id);
  auto image    = image_pool_->Read<std::shared_ptr<Image>>(
      image_id, [](const std::shared_ptr<Image>& img) { return img; });
  if (!image) {
    throw std::runtime_error(
        std::format("image {} of element {} is not in the image pool", image_id, element_id));
  }

  // The root state is the import state: the camera preview shows the same photo.
  if (!snapshot->Head().has_value()) {
    if (auto preview = EmbeddedPreviewReader::Read(image->image_path_, input_short_edge_)) {
      if (IsCanceled(canceled)) {
        callback(canceled_result());
        return;
      }
      callback(SemanticInputImageResult{
          .element_id = element_id,
          .image_id   = image_id,
          .status     = SemanticInputImageStatus::kReady,
          .image      = ToInputImage(preview->rgb8, input_short_edge_,
                                     SemanticInputImageSource::kEmbeddedPreview)});
      return;
    }
  }

  ScheduleRender(element_id, image_id, std::move(snapshot), image, canceled, std::move(callback));
}

void SemanticInputImageService::ScheduleRender(
    sl_element_id_t element_id, image_id_t image_id,
    std::shared_ptr<const PipelineGraphSnapshot> snapshot, const std::shared_ptr<Image>& image,
    std::function<bool()> canceled, SemanticInputImageCallback callback) {
  const uint32_t                          max_edge   = RenderMaxEdge(input_short_edge_);
  const DecodeRes                         decode_res = DecodeResForMaxEdge(max_edge);

  std::shared_ptr<const PreparedRawInput> source;
  {
    const auto encoded = ByteBufferLoader::LoadByteBufferFromImage(image);
    if (IsCanceled(canceled)) {
      callback(SemanticInputImageResult{.element_id = element_id,
                                        .image_id   = image_id,
                                        .status     = SemanticInputImageStatus::kCanceled,
                                        .message    = "semantic generation job was canceled"});
      return;
    }
    source = std::make_shared<const PreparedRawInput>(
        LoadEncodedImage(std::as_bytes(std::span<const uint8_t>(encoded)), decode_res));
  }

  auto answer        = std::make_shared<RenderAnswer>();
  answer->element_id = element_id;
  answer->image_id   = image_id;
  answer->callback   = std::move(callback);

  struct ExecutorLease {
    std::optional<size_t> index;
  };
  auto         lease = std::make_shared<ExecutorLease>();

  PipelineTask task;
  task.options_.render_desc_.render_type_ = RenderType::THUMBNAIL;
  task.options_.render_desc_.max_edge_    = max_edge;
  task.options_.render_desc_.decode_res_  = decode_res;
  task.options_.is_blocking_              = false;
  task.options_.is_callback_              = true;
  task.options_.is_seq_callback_          = false;
  task.cancel_requested_                  = canceled;
  task.snapshot_                          = std::move(snapshot);
  task.prepared_input_                    = std::move(source);
  task.prepare_                           = [this, lease](PipelineTask& prepared) -> bool {
    auto [index, executor]      = render_executors_.Take();
    lease->index                = index;
    prepared.pipeline_executor_ = std::move(executor);
    return true;
  };
  task.callback_ = [answer, short_edge = input_short_edge_](ImageBuffer& result) {
    try {
      answer->Send(SemanticInputImageResult{
          .status = SemanticInputImageStatus::kReady,
          .image  = ToInputImage(RenderPixelsToRgb8(result), short_edge,
                                 SemanticInputImageSource::kCommittedRender)});
    } catch (const std::exception& e) {
      answer->Send(SemanticInputImageResult{.status  = SemanticInputImageStatus::kError,
                                            .message = e.what()});
    }
  };
  task.on_complete_ = [this, lease, answer, canceled](bool success, std::string message) {
    if (lease->index.has_value()) {
      render_executors_.Return(*lease->index);
    }
    if (IsCanceled(canceled)) {
      answer->Send(SemanticInputImageResult{.status  = SemanticInputImageStatus::kCanceled,
                                            .message = "semantic generation job was canceled"});
      return;
    }
    answer->Send(SemanticInputImageResult{
        .status = SemanticInputImageStatus::kError,
        .message =
            success || message.empty()
                ? std::format("the render of element {} produced no image", answer->element_id)
                : std::move(message)});
  };
  render_scheduler_.ScheduleTask(std::move(task));
}

}  // namespace alcedo
