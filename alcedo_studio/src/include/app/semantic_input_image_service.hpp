//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "app/batch_executor_pool.hpp"
#include "app/image_pool_service.hpp"
#include "app/pipeline_service.hpp"
#include "concurrency/thread_pool.hpp"
#include "renderer/pipeline_scheduler.hpp"
#include "type/type.hpp"

namespace alcedo {

/// Where the pixels of a semantic model input came from.
enum class SemanticInputImageSource : uint8_t {
  /// The preview that the camera stored in the RAW file. Used for a RAW image whose committed
  /// history is its root state.
  kEmbeddedPreview,
  /// A render of the committed pipeline state, for an edited image, a raster image, or a RAW file
  /// without a large enough preview.
  kCommittedRender,
};

enum class SemanticInputImageStatus : uint8_t { kReady, kCanceled, kError };

/// One model input image: RGBA8, upright, and scaled so that its shorter edge equals the model
/// input size (or is smaller only when the source itself is smaller).
struct SemanticInputImage {
  std::vector<uint8_t>     rgba8;
  uint32_t                 width  = 0;
  uint32_t                 height = 0;
  SemanticInputImageSource source = SemanticInputImageSource::kCommittedRender;
};

struct SemanticInputImageResult {
  sl_element_id_t          element_id = 0;
  image_id_t               image_id   = 0;
  SemanticInputImageStatus status     = SemanticInputImageStatus::kError;
  std::string              message;
  SemanticInputImage       image;
};

using SemanticInputImageCallback = std::function<void(SemanticInputImageResult)>;

/**
 * @brief Source of the model input images of semantic label generation.
 */
class ISemanticInputImageSource {
 public:
  virtual ~ISemanticInputImageSource() = default;

  /**
   * @brief Prepare the model input of one image.
   *
   * @p callback runs exactly once, on a worker thread of the source. A request whose @p canceled
   * returns true before its work starts or between its stages ends with kCanceled.
   */
  virtual void               Request(sl_element_id_t element_id, image_id_t image_id,
                                     std::function<bool()> canceled, SemanticInputImageCallback callback) = 0;

  /// Number of outstanding requests that keeps every worker of the source busy.
  [[nodiscard]] virtual auto ConcurrencyLimit() const -> size_t = 0;
};

/**
 * @brief Model input images for semantic label generation, apart from the thumbnail service.
 *
 * For a RAW image whose committed history is its root state (no committed edit), the input is the
 * camera's preview stored in the RAW file (@ref EmbeddedPreviewReader): the file read covers the
 * metadata and the preview, and the sensor data stays packed. Every other image, and a RAW file
 * whose preview is smaller than the model input, gets a render of its committed pipeline state.
 *
 * Owns its workers: a CPU pool for the snapshot lookup, the preview read, and the RAW decode, and
 * one batch executor with one render worker for the renders. The batch executor creates its GPU
 * device at the first render, so a run of unedited RAW images creates no device. It never takes the
 * editor's render lock and shares no cache with the thumbnail service.
 *
 * Thread: Request may be called from any thread. The destructor drops queued work and waits for
 * running work; call it on a thread that is not one of its workers.
 */
class SemanticInputImageService final : public ISemanticInputImageSource {
 public:
  /// CPU workers unless the caller asks for another count: half the hardware threads, 2 to 8.
  [[nodiscard]] static auto DefaultWorkerCount() -> size_t;

  /**
   * @param input_short_edge The model input size in pixels (the image_size of the model); the
   *        shorter edge of every input is scaled to it.
   */
  SemanticInputImageService(std::shared_ptr<ImagePoolService>    image_pool,
                            std::shared_ptr<PipelineMgmtService> pipelines,
                            uint32_t input_short_edge, size_t worker_count = DefaultWorkerCount());
  ~SemanticInputImageService() override;

  SemanticInputImageService(const SemanticInputImageService&)                    = delete;
  auto operator=(const SemanticInputImageService&) -> SemanticInputImageService& = delete;

  void Request(sl_element_id_t element_id, image_id_t image_id, std::function<bool()> canceled,
               SemanticInputImageCallback callback) override;
  [[nodiscard]] auto ConcurrencyLimit() const -> size_t override;

 private:
  void                              Prepare(sl_element_id_t element_id, image_id_t image_id,
                                            const std::function<bool()>& canceled, SemanticInputImageCallback callback);
  void                              ScheduleRender(sl_element_id_t element_id, image_id_t image_id,
                                                   std::shared_ptr<const PipelineGraphSnapshot> snapshot,
                                                   const std::shared_ptr<Image>& image, std::function<bool()> canceled,
                                                   SemanticInputImageCallback callback);

  std::shared_ptr<ImagePoolService> image_pool_;
  std::shared_ptr<PipelineMgmtService> pipelines_;
  uint32_t                             input_short_edge_ = 0;
  size_t                               worker_count_     = 0;
  BatchExecutorPool                    render_executors_;
  PipelineScheduler                    render_scheduler_;
  ThreadPool                           workers_;
};

}  // namespace alcedo
