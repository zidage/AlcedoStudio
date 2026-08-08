//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include "app/ai_sidecar_runtime_service.hpp"
#include "app/thumbnail_service.hpp"
#include "app/thumbnail_types.hpp"
#include "sidecar_client/dto/semantic_embedding.hpp"
#include "storage/store/semantic/semantic_label_config.hpp"
#include "storage/store/semantic/semantic_store.hpp"
#include "type/type.hpp"

namespace alcedo {

struct SemanticGenerationItem {
  sl_element_id_t element_id = 0;
  image_id_t      image_id   = 0;
};

enum class SemanticGenerationItemStatus : uint8_t {
  kPending = 0,
  kThumbnailReady,
  kEmbeddingRequested,
  kEmbedded,
  kSkipped,
  kCanceled,
  kError,
};

struct SemanticGenerationItemResult {
  SemanticGenerationItem       item{};
  std::string                  request_id;
  SemanticGenerationItemStatus status = SemanticGenerationItemStatus::kPending;
  std::string                  error;
  std::vector<float>           embedding;
  uint32_t                     embedding_dimension = 0;
  bool                         has_label           = false;
  std::string                  label;
  double                       label_score = 0.0;
  std::string                  second_label;
  double                       second_label_score = 0.0;
  double                       label_margin       = 0.0;
  bool                         label_confident    = false;
};

struct SemanticGenerationProgress {
  size_t total               = 0;
  size_t thumbnails_ready    = 0;
  size_t embedding_requested = 0;
  size_t embedded            = 0;
  size_t skipped             = 0;
  size_t failed              = 0;
  size_t canceled            = 0;
};

struct SemanticGenerationPersistenceOptions {
  SemanticStore* storage_controller = nullptr;
  std::string                model_key;
  std::string                prompt_config_hash = kDefaultSemanticPhotographyPromptConfigHash;
  size_t                     label_prototype_batch_size  = 64;
  double                     confidence_score_threshold  = kDefaultSemanticLabelConfidenceThreshold;
  double                     confidence_margin_threshold = kDefaultSemanticLabelMarginThreshold;
  size_t                     top_score_count             = kDefaultSemanticLabelTopScoreCount;
};

struct SemanticGenerationOptions {
  ThumbnailResolution                     thumbnail_resolution = ThumbnailResolution::k256;
  size_t                                  thumbnail_batch_size = 32;
  size_t                                  embedding_batch_size = 64;
  std::chrono::milliseconds               embedding_timeout{30000};
  std::optional<AiSidecarRuntimeModelInfo> expected_model_info;
  std::optional<SemanticGenerationPersistenceOptions> persistence;
  bool                                                force_regenerate = false;
};

struct SemanticImageEmbeddingInput {
  SemanticGenerationItem item{};
  std::string            request_id;
  std::vector<uint8_t>   rgba8_image;
  std::string            format_hint;
};

struct SemanticImageEmbeddingBatchResult {
  SemanticGenerationItem  item{};
  SemanticEmbeddingResult embedding{};
};

using SemanticGenerationProgressCallback = std::function<void(const SemanticGenerationProgress&)>;
using SemanticGenerationFinishedCallback =
    std::function<void(std::vector<SemanticGenerationItemResult>)>;
using SemanticThumbnailRequestCallback = std::function<void(ThumbnailRequestResult)>;
using SemanticImageEmbeddingBatchCallback =
    std::function<void(std::vector<SemanticImageEmbeddingBatchResult>)>;

class ISemanticThumbnailProvider {
 public:
  virtual ~ISemanticThumbnailProvider()                                    = default;

  virtual void RequestThumbnail(const SemanticGenerationItem& item, ThumbnailResolution resolution,
                                SemanticThumbnailRequestCallback callback) = 0;
  virtual void CancelThumbnail(const ThumbnailCacheKey& key)               = 0;
  virtual void ReleaseThumbnail(const ThumbnailCacheKey& key)              = 0;
};

class ThumbnailServiceSemanticThumbnailProvider final : public ISemanticThumbnailProvider {
 public:
  explicit ThumbnailServiceSemanticThumbnailProvider(std::shared_ptr<ThumbnailService> service);

  void RequestThumbnail(const SemanticGenerationItem& item, ThumbnailResolution resolution,
                        SemanticThumbnailRequestCallback callback) override;
  void CancelThumbnail(const ThumbnailCacheKey& key) override;
  void ReleaseThumbnail(const ThumbnailCacheKey& key) override;

 private:
  std::shared_ptr<ThumbnailService> service_;
};

class ISemanticImageEmbeddingClient {
 public:
  virtual ~ISemanticImageEmbeddingClient()                                              = default;

  virtual auto GetModelInfo(AiSidecarRuntimeModelInfo* info, std::string* error) -> bool = 0;
  virtual auto EmbedText(const std::string& request_id, const std::string& text,
                         std::chrono::milliseconds timeout) -> SemanticEmbeddingResult  = 0;
  virtual auto EmbedTextBatch(const std::vector<SemanticTextEmbeddingRequest>& requests,
                              std::chrono::milliseconds                        timeout)
      -> std::vector<SemanticEmbeddingResult>;
  virtual void EmbedImageBatch(std::vector<SemanticImageEmbeddingInput> inputs,
                               std::chrono::milliseconds                timeout,
                               SemanticImageEmbeddingBatchCallback      callback) = 0;
};

class AiSidecarRuntimeImageEmbeddingClient final : public ISemanticImageEmbeddingClient {
 public:
  explicit AiSidecarRuntimeImageEmbeddingClient(std::shared_ptr<AiSidecarRuntimeService> runtime);

  auto GetModelInfo(AiSidecarRuntimeModelInfo* info, std::string* error) -> bool override;
  auto EmbedText(const std::string& request_id, const std::string& text,
                 std::chrono::milliseconds timeout) -> SemanticEmbeddingResult override;
  auto EmbedTextBatch(const std::vector<SemanticTextEmbeddingRequest>& requests,
                      std::chrono::milliseconds                        timeout)
      -> std::vector<SemanticEmbeddingResult> override;
  void EmbedImageBatch(std::vector<SemanticImageEmbeddingInput> inputs,
                       std::chrono::milliseconds                timeout,
                       SemanticImageEmbeddingBatchCallback      callback) override;

 private:
  std::shared_ptr<AiSidecarRuntimeService> runtime_;
};

class MockSemanticImageEmbeddingClient final : public ISemanticImageEmbeddingClient {
 public:
  explicit MockSemanticImageEmbeddingClient(
      std::chrono::milliseconds response_delay      = std::chrono::milliseconds(0),
      uint32_t                  embedding_dimension = 2);

  auto GetModelInfo(AiSidecarRuntimeModelInfo* info, std::string* error) -> bool override;
  auto EmbedText(const std::string& request_id, const std::string& text,
                 std::chrono::milliseconds timeout) -> SemanticEmbeddingResult override;
  void EmbedImageBatch(std::vector<SemanticImageEmbeddingInput> inputs,
                       std::chrono::milliseconds                timeout,
                       SemanticImageEmbeddingBatchCallback      callback) override;

  void FailRequestIds(std::unordered_set<std::string> request_ids);

 private:
  std::chrono::milliseconds       response_delay_;
  uint32_t                        embedding_dimension_;
  std::mutex                      lock_;
  std::unordered_set<std::string> fail_request_ids_;
};

class SemanticGenerationJob final {
 public:
  SemanticGenerationJob() = default;
  ~SemanticGenerationJob();

  SemanticGenerationJob(const SemanticGenerationJob&)            = delete;
  SemanticGenerationJob& operator=(const SemanticGenerationJob&) = delete;

  void                   Cancel();
  auto                   IsCanceled() const -> bool;
  void                   Wait();
  auto                   SnapshotProgress() const -> SemanticGenerationProgress;
  auto                   Results() const -> std::vector<SemanticGenerationItemResult>;

 private:
  friend class SemanticGenerationService;

  void UpdateProgress(const std::function<void(SemanticGenerationProgress&)>& updater);
  void AppendResult(SemanticGenerationItemResult result);
  void SetWorkerThread(std::thread worker);
  void Finish();

  mutable std::mutex                        lock_;
  std::condition_variable                   finished_cv_;
  SemanticGenerationProgress                progress_{};
  std::vector<SemanticGenerationItemResult> results_;
  std::atomic<bool>                         canceled_{false};
  std::thread                               worker_;
  bool                                      finished_ = false;
};

class SemanticGenerationService final {
 public:
  SemanticGenerationService(std::shared_ptr<ISemanticThumbnailProvider>    thumbnail_provider,
                            std::shared_ptr<ISemanticImageEmbeddingClient> embedding_client);

  static auto EnsureLabelPrototypes(const SemanticGenerationPersistenceOptions& persistence,
                                    const std::shared_ptr<ISemanticImageEmbeddingClient>& client,
                                    std::chrono::milliseconds timeout, std::string* error) -> bool;

  auto        StartGeneration(std::vector<SemanticGenerationItem> items,
                              SemanticGenerationOptions           options     = {},
                              SemanticGenerationProgressCallback  on_progress = {},
                              SemanticGenerationFinishedCallback  on_finished = {})
      -> std::shared_ptr<SemanticGenerationJob>;

 private:
  static void RunJob(const std::shared_ptr<SemanticGenerationJob>&  job,
                     const std::vector<SemanticGenerationItem>&     items,
                     SemanticGenerationOptions                      options,
                     SemanticGenerationProgressCallback             on_progress,
                     SemanticGenerationFinishedCallback             on_finished,
                     std::shared_ptr<ISemanticThumbnailProvider>    thumbnail_provider,
                     std::shared_ptr<ISemanticImageEmbeddingClient> embedding_client);

  std::shared_ptr<ISemanticThumbnailProvider>    thumbnail_provider_;
  std::shared_ptr<ISemanticImageEmbeddingClient> embedding_client_;
};

auto ToString(SemanticGenerationItemStatus status) -> const char*;

}  // namespace alcedo
