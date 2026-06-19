//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/semantic_generation_service.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <deque>
#include <exception>
#include <future>
#include <numeric>
#include <opencv2/imgproc.hpp>
#include <optional>
#include <QStringList>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <utility>

#include "utils/diagnostics/app_logging.hpp"

namespace alcedo {
namespace {

auto MakeRequestId(const SemanticGenerationItem& item) -> std::string {
  return "semantic-image-" + std::to_string(item.element_id) + "-" + std::to_string(item.image_id);
}

auto SummarizeItemIds(const std::vector<SemanticGenerationItem>& items,
                      size_t                                    max_items = 12) -> QString {
  QStringList parts;
  const size_t count = std::min(items.size(), max_items);
  for (size_t i = 0; i < count; ++i) {
    parts << QStringLiteral("%1:%2")
                 .arg(static_cast<qulonglong>(items[i].element_id))
                 .arg(static_cast<qulonglong>(items[i].image_id));
  }
  if (items.size() > max_items) {
    parts << QStringLiteral("...");
  }
  return parts.join(QLatin1Char(','));
}

auto SummarizeInputIds(const std::vector<SemanticImageEmbeddingInput>& inputs,
                       size_t max_items = 12) -> QString {
  QStringList parts;
  const size_t count = std::min(inputs.size(), max_items);
  for (size_t i = 0; i < count; ++i) {
    parts << QStringLiteral("%1:%2/%3")
                 .arg(static_cast<qulonglong>(inputs[i].item.element_id))
                 .arg(static_cast<qulonglong>(inputs[i].item.image_id))
                 .arg(QString::fromStdString(inputs[i].request_id));
  }
  if (inputs.size() > max_items) {
    parts << QStringLiteral("...");
  }
  return parts.join(QLatin1Char(','));
}

void DispatchProgress(const std::shared_ptr<SemanticGenerationJob>& job,
                      const SemanticGenerationProgressCallback&     callback) {
  if (callback) {
    callback(job->SnapshotProgress());
  }
}

auto MaterializeThumbnailRgba8(const ThumbnailGuard& guard, std::vector<uint8_t>* rgba8_image,
                               std::string* format_hint, std::string* error) -> bool {
  if (!guard.thumbnail_buffer_) {
    if (error) {
      *error = "thumbnail guard does not contain an image buffer";
    }
    return false;
  }

  auto* buffer = guard.thumbnail_buffer_.get();
  try {
    if (!buffer->cpu_data_valid_ && buffer->gpu_data_valid_) {
      buffer->SyncToCPU();
    }
  } catch (const std::exception& e) {
    if (error) {
      *error = std::string("failed to sync thumbnail to CPU: ") + e.what();
    }
    return false;
  } catch (...) {
    if (error) {
      *error = "failed to sync thumbnail to CPU";
    }
    return false;
  }

  if (!buffer->cpu_data_valid_) {
    if (error) {
      *error = "thumbnail has no CPU-readable image data";
    }
    return false;
  }

  cv::Mat src = buffer->GetCPUData();
  if (src.empty()) {
    if (error) {
      *error = "thumbnail image data is empty";
    }
    return false;
  }

  cv::Mat src8;
  if (src.depth() == CV_8U) {
    src8 = src.isContinuous() ? src : src.clone();
  } else if (src.depth() == CV_32F) {
    src.convertTo(src8, CV_MAKETYPE(CV_8U, src.channels()), 255.0);
  } else {
    src.convertTo(src8, CV_MAKETYPE(CV_8U, src.channels()));
  }

  cv::Mat rgba8;
  switch (src8.channels()) {
    case 1:
      cv::cvtColor(src8, rgba8, cv::COLOR_GRAY2RGBA);
      break;
    case 3:
      cv::cvtColor(src8, rgba8, cv::COLOR_RGB2RGBA);
      break;
    case 4:
      rgba8 = src8.isContinuous() ? src8 : src8.clone();
      break;
    default:
      if (error) {
        *error = "thumbnail image has unsupported channel count for RGBA8 conversion";
      }
      return false;
  }

  if (!rgba8.isContinuous()) {
    rgba8 = rgba8.clone();
  }

  const auto           byte_count = static_cast<size_t>(rgba8.total()) * rgba8.elemSize();
  std::vector<uint8_t> bytes(byte_count);
  std::copy(rgba8.data, rgba8.data + byte_count, bytes.begin());
  if (format_hint) {
    *format_hint = "rgba8:" + std::to_string(rgba8.cols) + "x" + std::to_string(rgba8.rows);
  }
  *rgba8_image = std::move(bytes);
  return true;
}

auto MatchesExpectedModelInfo(const SemanticRuntimeModelInfo& actual,
                              const SemanticRuntimeModelInfo& expected, std::string* error)
    -> bool {
  if (actual.model_id != expected.model_id) {
    if (error) {
      *error = "semantic runtime model id mismatch: expected " + expected.model_id + ", got " +
               actual.model_id;
    }
    return false;
  }
  if (actual.revision != expected.revision) {
    if (error) {
      *error = "semantic runtime revision mismatch: expected " + expected.revision + ", got " +
               actual.revision;
    }
    return false;
  }
  if (actual.embedding_dimension != expected.embedding_dimension) {
    if (error) {
      *error = "semantic runtime embedding dimension mismatch: expected " +
               std::to_string(expected.embedding_dimension) + ", got " +
               std::to_string(actual.embedding_dimension);
    }
    return false;
  }
  if (actual.image_size != expected.image_size) {
    if (error) {
      *error = "semantic runtime image size mismatch: expected " +
               std::to_string(expected.image_size) + ", got " + std::to_string(actual.image_size);
    }
    return false;
  }
  return true;
}

auto ValidateFiniteNonZeroVector(const std::vector<float>& embedding, uint32_t expected_dimension,
                                 std::string* error) -> bool {
  if (embedding.size() != expected_dimension) {
    if (error) {
      *error = "semantic embedding dimension mismatch: expected " +
               std::to_string(expected_dimension) + ", got " + std::to_string(embedding.size());
    }
    return false;
  }

  double norm_sq = 0.0;
  for (const float value : embedding) {
    if (!std::isfinite(value)) {
      if (error) {
        *error = "semantic embedding contains NaN or infinity";
      }
      return false;
    }
    norm_sq += static_cast<double>(value) * static_cast<double>(value);
  }
  if (norm_sq <= 0.0) {
    if (error) {
      *error = "semantic embedding norm is zero";
    }
    return false;
  }
  return true;
}

auto EnsureCachedLabelPrototypes(const SemanticGenerationPersistenceOptions&           persistence,
                                 const std::shared_ptr<ISemanticImageEmbeddingClient>& client,
                                 std::chrono::milliseconds timeout, std::string* error) -> bool {
  diag::TraceScope trace(diag::semanticLog(), QStringLiteral("semantic.label_prototypes.ensure"),
                         QStringLiteral("model_key=%1 prompt_config=%2")
                             .arg(QString::fromStdString(persistence.model_key),
                                  QString::fromStdString(persistence.prompt_config_hash)));
  if (!persistence.storage_controller) {
    if (error) {
      *error = "semantic storage controller is not available";
    }
    return false;
  }
  if (persistence.model_key.empty()) {
    if (error) {
      *error = "semantic persistence model key is empty";
    }
    return false;
  }

  const auto query_count =
      persistence.storage_controller->CountLabelQueries(persistence.prompt_config_hash);
  if (query_count == 0) {
    if (error) {
      *error = "semantic label query table has no rows for prompt config " +
               persistence.prompt_config_hash;
    }
    return false;
  }

  const auto prototype_count = persistence.storage_controller->CountLabelPrototypes(
      persistence.model_key, persistence.prompt_config_hash);
  qCInfo(diag::semanticLog).noquote()
      << QStringLiteral("semantic.label_prototypes.counts model_key=%1 queries=%2 cached=%3")
             .arg(QString::fromStdString(persistence.model_key))
             .arg(static_cast<qulonglong>(query_count))
             .arg(static_cast<qulonglong>(prototype_count));
  if (prototype_count >= query_count) {
    return true;
  }

  const auto queries =
      persistence.storage_controller->ListLabelQueries(persistence.prompt_config_hash, error);
  if (queries.empty()) {
    if (error && error->empty()) {
      *error = "semantic label query table has no rows for prompt config " +
               persistence.prompt_config_hash;
    }
    return false;
  }

  std::vector<SemanticLabelPrototypeRecord> prototypes;
  prototypes.reserve(queries.size());
  const auto batch_size = std::max<size_t>(1, persistence.label_prototype_batch_size);
  for (size_t offset = 0; offset < queries.size(); offset += batch_size) {
    const auto                                end = std::min(queries.size(), offset + batch_size);
    std::vector<SemanticTextEmbeddingRequest> requests;
    requests.reserve(end - offset);
    for (size_t i = offset; i < end; ++i) {
      const auto& query = queries[i];
      requests.push_back(SemanticTextEmbeddingRequest{
          .request_id = "semantic-label-" + persistence.prompt_config_hash + "-" + query.label_,
          .text       = query.query_text_,
      });
    }

    qCInfo(diag::semanticLog).noquote()
        << QStringLiteral("semantic.label_prototypes.embedding.request offset=%1 count=%2")
               .arg(static_cast<qulonglong>(offset))
               .arg(static_cast<qulonglong>(requests.size()));
    auto batch_results = client->EmbedTextBatch(requests, timeout);
    qCInfo(diag::semanticLog).noquote()
        << QStringLiteral("semantic.label_prototypes.embedding.response offset=%1 count=%2")
               .arg(static_cast<qulonglong>(offset))
               .arg(static_cast<qulonglong>(batch_results.size()));
    std::unordered_map<std::string, SemanticEmbeddingResult> by_request_id;
    by_request_id.reserve(batch_results.size());
    for (auto& result : batch_results) {
      if (!result.request_id.empty()) {
        by_request_id[result.request_id] = std::move(result);
      }
    }

    for (size_t i = offset; i < end; ++i) {
      const auto& query   = queries[i];
      const auto& request = requests[i - offset];
      const auto  found   = by_request_id.find(request.request_id);
      if (found == by_request_id.end()) {
        if (error) {
          *error = "semantic label text embedding batch response is missing " + request.request_id;
        }
        return false;
      }
      auto& embedding = found->second;
      if (!embedding.ok) {
        if (error) {
          *error =
              embedding.error.empty() ? "semantic label text embedding failed" : embedding.error;
        }
        return false;
      }
      if (!ValidateFiniteNonZeroVector(embedding.embedding, embedding.dimension, error)) {
        return false;
      }
      SemanticLabelPrototypeRecord record;
      record.model_key_          = persistence.model_key;
      record.label_              = query.label_;
      record.prompt_config_hash_ = persistence.prompt_config_hash;
      record.embedding_          = std::move(embedding.embedding);
      prototypes.push_back(std::move(record));
    }
  }

  return persistence.storage_controller->UpsertLabelPrototypes(prototypes, error);
}

struct ThumbnailBatchWaitState {
  std::mutex                                         lock;
  std::condition_variable                            cv;
  std::vector<std::optional<ThumbnailRequestResult>> results;
  std::vector<bool>                                  done;
  size_t                                             remaining = 0;
  bool                                               abandoned = false;
};

auto WaitForThumbnailBatch(const std::shared_ptr<SemanticGenerationJob>&      job,
                           const std::shared_ptr<ISemanticThumbnailProvider>& provider,
                           const std::vector<SemanticGenerationItem>&         items,
                           ThumbnailResolution resolution) -> std::vector<ThumbnailRequestResult> {
  auto state       = std::make_shared<ThumbnailBatchWaitState>();
  state->results   = std::vector<std::optional<ThumbnailRequestResult>>(items.size());
  state->done      = std::vector<bool>(items.size(), false);
  state->remaining = items.size();

  for (size_t i = 0; i < items.size(); ++i) {
    const auto item = items[i];
    try {
      provider->RequestThumbnail(
          item, resolution, [state, provider, index = i](ThumbnailRequestResult result) {
            bool              release_late_guard = false;
            ThumbnailCacheKey late_key{};
            {
              std::unique_lock lock(state->lock);
              release_late_guard = state->abandoned && result.guard != nullptr;
              late_key           = result.key;
              if (!state->abandoned && index < state->results.size() && !state->done[index]) {
                state->results[index] = std::move(result);
                state->done[index]    = true;
                if (state->remaining > 0) {
                  state->remaining--;
                }
              }
            }
            if (release_late_guard) {
              provider->ReleaseThumbnail(late_key);
            }
            state->cv.notify_all();
          });
    } catch (const std::exception& e) {
      std::unique_lock       lock(state->lock);
      ThumbnailRequestResult result;
      result.key        = ThumbnailCacheKey{item.element_id, resolution};
      result.status     = ThumbnailRequestStatus::kError;
      result.message    = std::string("thumbnail request failed: ") + e.what();
      state->results[i] = std::move(result);
      state->done[i]    = true;
      if (state->remaining > 0) {
        state->remaining--;
      }
      state->cv.notify_all();
    } catch (...) {
      std::unique_lock       lock(state->lock);
      ThumbnailRequestResult result;
      result.key        = ThumbnailCacheKey{item.element_id, resolution};
      result.status     = ThumbnailRequestStatus::kError;
      result.message    = "thumbnail request failed";
      state->results[i] = std::move(result);
      state->done[i]    = true;
      if (state->remaining > 0) {
        state->remaining--;
      }
      state->cv.notify_all();
    }
  }

  {
    std::unique_lock lock(state->lock);
    while (state->remaining > 0) {
      if (job->IsCanceled()) {
        state->abandoned = true;
        lock.unlock();
        for (const auto& item : items) {
          provider->CancelThumbnail(ThumbnailCacheKey{item.element_id, resolution});
        }
        std::vector<ThumbnailRequestResult> canceled_results;
        canceled_results.reserve(items.size());
        for (const auto& item : items) {
          ThumbnailRequestResult result;
          result.key     = ThumbnailCacheKey{item.element_id, resolution};
          result.status  = ThumbnailRequestStatus::kCanceled;
          result.message = "semantic generation job was canceled";
          canceled_results.push_back(std::move(result));
        }
        return canceled_results;
      }
      state->cv.wait_for(lock, std::chrono::milliseconds(25));
    }
  }

  std::vector<ThumbnailRequestResult> results;
  results.reserve(items.size());
  for (size_t i = 0; i < items.size(); ++i) {
    if (state->results[i].has_value()) {
      results.push_back(std::move(*state->results[i]));
    } else {
      ThumbnailRequestResult result;
      result.key     = ThumbnailCacheKey{items[i].element_id, resolution};
      result.status  = ThumbnailRequestStatus::kError;
      result.message = "thumbnail request did not return a result";
      results.push_back(std::move(result));
    }
  }
  return results;
}

auto MapEmbeddingBatchResults(const std::vector<SemanticImageEmbeddingInput>& inputs,
                              std::vector<SemanticImageEmbeddingBatchResult>  batch_results)
    -> std::vector<SemanticImageEmbeddingBatchResult> {
  std::unordered_map<std::string, SemanticImageEmbeddingBatchResult> by_request_id;
  std::unordered_set<std::string>                                    duplicates;
  by_request_id.reserve(batch_results.size());

  for (auto& result : batch_results) {
    const auto request_id = result.embedding.request_id;
    if (by_request_id.contains(request_id)) {
      duplicates.insert(request_id);
      continue;
    }
    by_request_id.emplace(request_id, std::move(result));
  }

  std::vector<SemanticImageEmbeddingBatchResult> mapped;
  mapped.reserve(inputs.size());
  for (const auto& input : inputs) {
    if (duplicates.contains(input.request_id)) {
      SemanticImageEmbeddingBatchResult result;
      result.item                 = input.item;
      result.embedding.request_id = input.request_id;
      result.embedding.ok         = false;
      result.embedding.error      = "duplicate image embedding response for request id";
      mapped.push_back(std::move(result));
      continue;
    }

    auto found = by_request_id.find(input.request_id);
    if (found == by_request_id.end()) {
      SemanticImageEmbeddingBatchResult result;
      result.item                 = input.item;
      result.embedding.request_id = input.request_id;
      result.embedding.ok         = false;
      result.embedding.error      = "missing image embedding response for request id";
      mapped.push_back(std::move(result));
      continue;
    }

    found->second.item = input.item;
    mapped.push_back(std::move(found->second));
  }
  return mapped;
}

}  // namespace

auto ToString(SemanticGenerationItemStatus status) -> const char* {
  switch (status) {
    case SemanticGenerationItemStatus::kPending:
      return "pending";
    case SemanticGenerationItemStatus::kThumbnailReady:
      return "thumbnail_ready";
    case SemanticGenerationItemStatus::kEmbeddingRequested:
      return "embedding_requested";
    case SemanticGenerationItemStatus::kEmbedded:
      return "embedded";
    case SemanticGenerationItemStatus::kSkipped:
      return "skipped";
    case SemanticGenerationItemStatus::kCanceled:
      return "canceled";
    case SemanticGenerationItemStatus::kError:
      return "error";
  }
  return "unknown";
}

ThumbnailServiceSemanticThumbnailProvider::ThumbnailServiceSemanticThumbnailProvider(
    std::shared_ptr<ThumbnailService> service)
    : service_(std::move(service)) {}

void ThumbnailServiceSemanticThumbnailProvider::RequestThumbnail(
    const SemanticGenerationItem& item, ThumbnailResolution resolution,
    SemanticThumbnailRequestCallback callback) {
  if (!service_) {
    ThumbnailRequestResult result;
    result.key     = ThumbnailCacheKey{item.element_id, resolution};
    result.status  = ThumbnailRequestStatus::kError;
    result.message = "ThumbnailService is not available";
    callback(std::move(result));
    return;
  }

  service_->GetThumbnailDetailed(item.element_id, item.image_id, std::move(callback), true, nullptr,
                                 resolution);
}

void ThumbnailServiceSemanticThumbnailProvider::CancelThumbnail(const ThumbnailCacheKey& key) {
  if (service_) {
    service_->CancelPending(key);
  }
}

void ThumbnailServiceSemanticThumbnailProvider::ReleaseThumbnail(const ThumbnailCacheKey& key) {
  if (service_) {
    service_->ReleaseThumbnail(key);
  }
}

auto ISemanticImageEmbeddingClient::EmbedTextBatch(
    const std::vector<SemanticTextEmbeddingRequest>& requests, std::chrono::milliseconds timeout)
    -> std::vector<SemanticEmbeddingResult> {
  std::vector<SemanticEmbeddingResult> results;
  results.reserve(requests.size());
  for (const auto& request : requests) {
    results.push_back(EmbedText(request.request_id, request.text, timeout));
  }
  return results;
}

MockSemanticImageEmbeddingClient::MockSemanticImageEmbeddingClient(
    std::chrono::milliseconds response_delay, uint32_t embedding_dimension)
    : response_delay_(response_delay), embedding_dimension_(embedding_dimension) {}

void MockSemanticImageEmbeddingClient::EmbedImageBatch(
    std::vector<SemanticImageEmbeddingInput> inputs, std::chrono::milliseconds timeout,
    SemanticImageEmbeddingBatchCallback callback) {
  (void)timeout;
  std::unordered_set<std::string> fail_ids;
  {
    std::unique_lock lock(lock_);
    fail_ids = fail_request_ids_;
  }

  const auto delay = response_delay_;
  const auto dim   = embedding_dimension_;
  std::thread([inputs = std::move(inputs), callback = std::move(callback),
               fail_ids = std::move(fail_ids), delay, dim]() mutable {
    if (delay.count() > 0) {
      std::this_thread::sleep_for(delay);
    }

    std::vector<SemanticImageEmbeddingBatchResult> results;
    results.reserve(inputs.size());
    for (const auto& input : inputs) {
      SemanticImageEmbeddingBatchResult batch_result;
      batch_result.item                 = input.item;
      batch_result.embedding.request_id = input.request_id;
      batch_result.embedding.model_name = "mock/mobileclip";
      batch_result.embedding.dimension  = dim;
      if (fail_ids.contains(input.request_id)) {
        batch_result.embedding.ok    = false;
        batch_result.embedding.error = "mock image embedding failure";
      } else {
        batch_result.embedding.ok = true;
        batch_result.embedding.embedding.resize(dim, 0.0f);
        if (dim > 0) {
          const auto byte_sum =
              std::accumulate(input.rgba8_image.begin(), input.rgba8_image.end(), uint64_t{0});
          batch_result.embedding.embedding[0] = static_cast<float>((byte_sum % 997) + 1) / 997.0f;
        }
        if (dim > 1) {
          batch_result.embedding.embedding[1] =
              static_cast<float>((input.rgba8_image.size() % 991) + 1) / 991.0f;
        }
      }
      results.push_back(std::move(batch_result));
    }

    if (callback) {
      callback(std::move(results));
    }
  }).detach();
}

auto MockSemanticImageEmbeddingClient::GetModelInfo(SemanticRuntimeModelInfo* info,
                                                    std::string*              error) -> bool {
  (void)error;
  if (info) {
    info->model_id            = "mock/mobileclip";
    info->revision            = "mock-revision";
    info->embedding_dimension = embedding_dimension_;
    info->image_size          = 256;
    info->provider            = "mock";
  }
  return true;
}

auto MockSemanticImageEmbeddingClient::EmbedText(const std::string&        request_id,
                                                 const std::string&        text,
                                                 std::chrono::milliseconds timeout)
    -> SemanticEmbeddingResult {
  (void)timeout;
  SemanticEmbeddingResult result;
  result.request_id = request_id;
  result.model_name = "mock/mobileclip";
  result.dimension  = embedding_dimension_;
  result.ok         = true;
  result.embedding.resize(embedding_dimension_, 0.0F);
  if (embedding_dimension_ > 0) {
    const auto byte_sum =
        std::accumulate(text.begin(), text.end(), uint64_t{0},
                        [](uint64_t sum, char ch) { return sum + static_cast<unsigned char>(ch); });
    result.embedding[0] = static_cast<float>((byte_sum % 997) + 1) / 997.0F;
  }
  if (embedding_dimension_ > 1) {
    result.embedding[1] = static_cast<float>((text.size() % 991) + 1) / 991.0F;
  }
  return result;
}

void MockSemanticImageEmbeddingClient::FailRequestIds(std::unordered_set<std::string> request_ids) {
  std::unique_lock lock(lock_);
  fail_request_ids_ = std::move(request_ids);
}

SemanticRuntimeImageEmbeddingClient::SemanticRuntimeImageEmbeddingClient(
    std::shared_ptr<SemanticRuntimeService> runtime)
    : runtime_(std::move(runtime)) {}

auto SemanticRuntimeImageEmbeddingClient::GetModelInfo(SemanticRuntimeModelInfo* info,
                                                       std::string*              error) -> bool {
  if (!runtime_) {
    if (error) {
      *error = "semantic runtime service is not available";
    }
    return false;
  }

  const auto status = runtime_->Status();
  if (status.state != SemanticRuntimeState::kReady) {
    if (error) {
      *error = "semantic runtime is not ready";
    }
    return false;
  }
  if (!status.model_info.has_value()) {
    if (error) {
      *error = "semantic runtime model info is not available";
    }
    return false;
  }
  if (info) {
    *info = *status.model_info;
  }
  return true;
}

auto SemanticRuntimeImageEmbeddingClient::EmbedText(const std::string&        request_id,
                                                    const std::string&        text,
                                                    std::chrono::milliseconds timeout)
    -> SemanticEmbeddingResult {
  if (!runtime_) {
    SemanticEmbeddingResult result;
    result.request_id = request_id;
    result.ok         = false;
    result.error      = "semantic runtime service is not available";
    return result;
  }
  return runtime_->EmbedText(request_id, text, timeout);
}

auto SemanticRuntimeImageEmbeddingClient::EmbedTextBatch(
    const std::vector<SemanticTextEmbeddingRequest>& requests, std::chrono::milliseconds timeout)
    -> std::vector<SemanticEmbeddingResult> {
  if (!runtime_) {
    std::vector<SemanticEmbeddingResult> results;
    results.reserve(requests.size());
    for (const auto& request : requests) {
      SemanticEmbeddingResult result;
      result.request_id = request.request_id;
      result.ok         = false;
      result.error      = "semantic runtime service is not available";
      results.push_back(std::move(result));
    }
    return results;
  }
  return runtime_->EmbedTextBatch(requests, timeout);
}

void SemanticRuntimeImageEmbeddingClient::EmbedImageBatch(
    std::vector<SemanticImageEmbeddingInput> inputs, std::chrono::milliseconds timeout,
    SemanticImageEmbeddingBatchCallback callback) {
  auto runtime = runtime_;
  std::thread([runtime = std::move(runtime), inputs = std::move(inputs), timeout,
               callback = std::move(callback)]() mutable {
    std::vector<SemanticImageEmbeddingBatchResult> results;
    if (!runtime) {
      results.reserve(inputs.size());
      for (const auto& input : inputs) {
        SemanticImageEmbeddingBatchResult result;
        result.item                 = input.item;
        result.embedding.request_id = input.request_id;
        result.embedding.ok         = false;
        result.embedding.error      = "semantic runtime service is not available";
        results.push_back(std::move(result));
      }
      if (callback) {
        callback(std::move(results));
      }
      return;
    }

    std::vector<SemanticImageEmbeddingRequest> requests;
    requests.reserve(inputs.size());
    for (auto& input : inputs) {
      SemanticImageEmbeddingRequest request;
      request.request_id  = std::move(input.request_id);
      request.rgba8_image = std::move(input.rgba8_image);
      request.format_hint = std::move(input.format_hint);
      requests.push_back(std::move(request));
    }

    const auto runtime_results = runtime->EmbedImageBatch(std::move(requests), timeout);
    results.reserve(runtime_results.size());
    for (size_t i = 0; i < runtime_results.size(); ++i) {
      SemanticImageEmbeddingBatchResult result;
      if (i < inputs.size()) {
        result.item = inputs[i].item;
      }
      result.embedding = runtime_results[i];
      results.push_back(std::move(result));
    }
    if (callback) {
      callback(std::move(results));
    }
  }).detach();
}

SemanticGenerationJob::~SemanticGenerationJob() {
  Cancel();
  if (worker_.joinable()) {
    if (worker_.get_id() == std::this_thread::get_id()) {
      worker_.detach();
    } else {
      worker_.join();
    }
  }
}

void SemanticGenerationJob::Cancel() { canceled_.store(true); }

auto SemanticGenerationJob::IsCanceled() const -> bool { return canceled_.load(); }

void SemanticGenerationJob::Wait() {
  {
    std::unique_lock lock(lock_);
    finished_cv_.wait(lock, [this]() { return finished_; });
  }
  if (worker_.joinable() && worker_.get_id() != std::this_thread::get_id()) {
    worker_.join();
  }
}

auto SemanticGenerationJob::SnapshotProgress() const -> SemanticGenerationProgress {
  std::unique_lock lock(lock_);
  return progress_;
}

auto SemanticGenerationJob::Results() const -> std::vector<SemanticGenerationItemResult> {
  std::unique_lock lock(lock_);
  return results_;
}

void SemanticGenerationJob::UpdateProgress(
    const std::function<void(SemanticGenerationProgress&)>& updater) {
  std::unique_lock lock(lock_);
  updater(progress_);
}

void SemanticGenerationJob::AppendResult(SemanticGenerationItemResult result) {
  std::unique_lock lock(lock_);
  results_.push_back(std::move(result));
}

void SemanticGenerationJob::SetWorkerThread(std::thread worker) { worker_ = std::move(worker); }

void SemanticGenerationJob::Finish() {
  {
    std::unique_lock lock(lock_);
    finished_ = true;
  }
  finished_cv_.notify_all();
}

SemanticGenerationService::SemanticGenerationService(
    std::shared_ptr<ISemanticThumbnailProvider>    thumbnail_provider,
    std::shared_ptr<ISemanticImageEmbeddingClient> embedding_client)
    : thumbnail_provider_(std::move(thumbnail_provider)),
      embedding_client_(std::move(embedding_client)) {
  if (!thumbnail_provider_) {
    throw std::invalid_argument("SemanticGenerationService requires a thumbnail provider");
  }
  if (!embedding_client_) {
    throw std::invalid_argument("SemanticGenerationService requires an embedding client");
  }
}

auto SemanticGenerationService::EnsureLabelPrototypes(
    const SemanticGenerationPersistenceOptions&           persistence,
    const std::shared_ptr<ISemanticImageEmbeddingClient>& client, std::chrono::milliseconds timeout,
    std::string* error) -> bool {
  if (!client) {
    if (error) {
      *error = "semantic embedding client is not available";
    }
    return false;
  }
  return EnsureCachedLabelPrototypes(persistence, client, timeout, error);
}

auto SemanticGenerationService::StartGeneration(std::vector<SemanticGenerationItem> items,
                                                SemanticGenerationOptions           options,
                                                SemanticGenerationProgressCallback  on_progress,
                                                SemanticGenerationFinishedCallback  on_finished)
    -> std::shared_ptr<SemanticGenerationJob> {
  if (options.thumbnail_batch_size == 0) {
    options.thumbnail_batch_size = 1;
  }
  if (options.embedding_batch_size == 0) {
    options.embedding_batch_size = 1;
  }
  if (options.persistence.has_value() && options.persistence->label_prototype_batch_size == 0) {
    options.persistence->label_prototype_batch_size = 1;
  }

  auto job = std::make_shared<SemanticGenerationJob>();
  job->UpdateProgress(
      [total = items.size()](SemanticGenerationProgress& progress) { progress.total = total; });

  auto thumbnail_provider = thumbnail_provider_;
  auto embedding_client   = embedding_client_;
  auto worker             = std::thread(
      [job, items = std::move(items), options, on_progress = std::move(on_progress),
       on_finished = std::move(on_finished), thumbnail_provider = std::move(thumbnail_provider),
       embedding_client = std::move(embedding_client)]() mutable {
        RunJob(job, items, options, std::move(on_progress), std::move(on_finished),
                           std::move(thumbnail_provider), std::move(embedding_client));
      });
  qCInfo(diag::semanticLog).noquote()
      << QStringLiteral(
             "semantic.generation.start total=%1 thumbnail_batch=%2 embedding_batch=%3 force=%4")
             .arg(static_cast<qulonglong>(job->SnapshotProgress().total))
             .arg(static_cast<qulonglong>(options.thumbnail_batch_size))
             .arg(static_cast<qulonglong>(options.embedding_batch_size))
             .arg(options.force_regenerate ? QStringLiteral("true") : QStringLiteral("false"));
  job->SetWorkerThread(std::move(worker));

  return job;
}

void SemanticGenerationService::RunJob(
    const std::shared_ptr<SemanticGenerationJob>& job,
    const std::vector<SemanticGenerationItem>& items, SemanticGenerationOptions options,
    SemanticGenerationProgressCallback on_progress, SemanticGenerationFinishedCallback on_finished,
    std::shared_ptr<ISemanticThumbnailProvider>    thumbnail_provider,
    std::shared_ptr<ISemanticImageEmbeddingClient> embedding_client) {
  auto finish = [&]() {
    auto results = job->Results();
    job->Finish();
    if (on_finished) {
      on_finished(std::move(results));
    }
  };

  if (options.expected_model_info.has_value()) {
    SemanticRuntimeModelInfo actual_model_info;
    std::string              model_error;
    if (!embedding_client->GetModelInfo(&actual_model_info, &model_error) ||
        !MatchesExpectedModelInfo(actual_model_info, *options.expected_model_info, &model_error)) {
      for (const auto& item : items) {
        SemanticGenerationItemResult result;
        result.item       = item;
        result.request_id = MakeRequestId(item);
        result.status     = SemanticGenerationItemStatus::kError;
        result.error =
            model_error.empty() ? "semantic runtime model info is not compatible" : model_error;
        job->AppendResult(std::move(result));
      }
      job->UpdateProgress([count = items.size()](SemanticGenerationProgress& progress) {
        progress.failed += count;
      });
      DispatchProgress(job, on_progress);
      finish();
      return;
    }
  }

  std::vector<SemanticGenerationItem> pending_items = items;
  if (options.persistence.has_value() && !options.force_regenerate) {
    std::vector<SemanticGenerationItem> remaining_items;
    remaining_items.reserve(items.size());
    const bool require_label = true;
    for (const auto& item : items) {
      if (options.persistence->storage_controller &&
          options.persistence->storage_controller->HasReadyImageEmbedding(
              item.element_id, item.image_id, options.persistence->model_key, require_label)) {
        SemanticGenerationItemResult result;
        result.item       = item;
        result.request_id = MakeRequestId(item);
        result.status     = SemanticGenerationItemStatus::kSkipped;
        result.error      = "semantic embedding already exists for the active model";
        job->AppendResult(std::move(result));
        job->UpdateProgress([](SemanticGenerationProgress& progress) { progress.skipped++; });
      } else {
        remaining_items.push_back(item);
      }
    }
    pending_items = std::move(remaining_items);
    if (pending_items.empty()) {
      DispatchProgress(job, on_progress);
      finish();
      return;
    }
    if (pending_items.size() != items.size()) {
      DispatchProgress(job, on_progress);
    }
  }

  if (options.persistence.has_value()) {
    std::string label_error;
    if (!EnsureLabelPrototypes(*options.persistence, embedding_client, options.embedding_timeout,
                               &label_error)) {
      for (const auto& item : pending_items) {
        SemanticGenerationItemResult result;
        result.item       = item;
        result.request_id = MakeRequestId(item);
        result.status     = SemanticGenerationItemStatus::kError;
        result.error =
            label_error.empty() ? "semantic label prototype cache is not available" : label_error;
        job->AppendResult(std::move(result));
      }
      job->UpdateProgress([count = pending_items.size()](SemanticGenerationProgress& progress) {
        progress.failed += count;
      });
      DispatchProgress(job, on_progress);
      finish();
      return;
    }
  }

  std::vector<SemanticImageEmbeddingInput> batch;
  batch.reserve(options.embedding_batch_size);

  struct InFlightEmbeddingBatch {
    std::vector<SemanticImageEmbeddingInput>                    inputs;
    std::future<std::vector<SemanticImageEmbeddingBatchResult>> future;
    std::chrono::steady_clock::time_point                       deadline;
  };

  std::deque<InFlightEmbeddingBatch> in_flight_batches;
  constexpr size_t                   kMaxInFlightEmbeddingBatches = 1;

  auto append_batch_canceled_results = [&](const std::vector<SemanticImageEmbeddingInput>& inputs,
                                           const std::string& message) {
    for (const auto& input : inputs) {
      SemanticGenerationItemResult result;
      result.item       = input.item;
      result.request_id = input.request_id;
      result.status     = SemanticGenerationItemStatus::kCanceled;
      result.error      = message;
      job->AppendResult(std::move(result));
    }
    job->UpdateProgress([count = inputs.size()](SemanticGenerationProgress& progress) {
      progress.canceled += count;
    });
    DispatchProgress(job, on_progress);
  };

  auto append_batch_error_results = [&](const std::vector<SemanticImageEmbeddingInput>& inputs,
                                        const std::string&                              message) {
    for (const auto& input : inputs) {
      SemanticGenerationItemResult result;
      result.item       = input.item;
      result.request_id = input.request_id;
      result.status     = SemanticGenerationItemStatus::kError;
      result.error      = message;
      job->AppendResult(std::move(result));
    }
    job->UpdateProgress([count = inputs.size()](SemanticGenerationProgress& progress) {
      progress.failed += count;
    });
    DispatchProgress(job, on_progress);
  };

  auto process_embedding_batch = [&](const std::vector<SemanticImageEmbeddingInput>& inputs,
                                     std::vector<SemanticImageEmbeddingBatchResult> batch_results) {
    if (job->IsCanceled()) {
      append_batch_canceled_results(inputs, "semantic generation job was canceled");
      return;
    }

    batch_results = MapEmbeddingBatchResults(inputs, std::move(batch_results));

    // First pass: build item results in batch order. Successful, valid embeddings are
    // collected so the whole batch can be persisted in a single DuckDB transaction
    // instead of one transaction per image.
    std::vector<SemanticGenerationItemResult> item_results;
    item_results.reserve(batch_results.size());
    std::vector<size_t>                       persist_indices;
    std::vector<SemanticImageEmbeddingRecord> persist_records;
    if (options.persistence.has_value()) {
      persist_indices.reserve(batch_results.size());
      persist_records.reserve(batch_results.size());
    }

    for (size_t i = 0; i < batch_results.size(); ++i) {
      auto& batch_result = batch_results[i];
      SemanticGenerationItemResult item_result;
      item_result.item       = batch_result.item;
      item_result.request_id = batch_result.embedding.request_id;
      if (!batch_result.embedding.ok) {
        item_result.status = SemanticGenerationItemStatus::kError;
        item_result.error  = std::move(batch_result.embedding.error);
        item_results.push_back(std::move(item_result));
        continue;
      }

      item_result.embedding_dimension = batch_result.embedding.dimension;
      if (options.persistence.has_value()) {
        std::string validate_error;
        if (!ValidateFiniteNonZeroVector(batch_result.embedding.embedding,
                                         batch_result.embedding.dimension, &validate_error)) {
          item_result.status = SemanticGenerationItemStatus::kError;
          item_result.error  = std::move(validate_error);
          item_results.push_back(std::move(item_result));
          continue;
        }
        SemanticImageEmbeddingRecord record;
        record.file_id_              = batch_result.item.element_id;
        record.image_id_             = batch_result.item.image_id;
        record.model_key_            = options.persistence->model_key;
        record.embedding_            = batch_result.embedding.embedding;
        record.thumbnail_resolution_ = static_cast<int>(options.thumbnail_resolution);
        persist_indices.push_back(i);
        persist_records.push_back(std::move(record));
        item_results.push_back(std::move(item_result));  // status filled after persist
      } else {
        item_result.status    = SemanticGenerationItemStatus::kEmbedded;
        item_result.embedding = std::move(batch_result.embedding.embedding);
        item_results.push_back(std::move(item_result));
      }
    }

    // Persist the whole batch in one transaction.
    std::vector<SemanticImageLabelRecord> assigned_labels;
    if (options.persistence.has_value() && !persist_records.empty()) {
      SemanticLabelAssignmentOptions assignment;
      assignment.prompt_config_hash_          = options.persistence->prompt_config_hash;
      assignment.confidence_score_threshold_  = options.persistence->confidence_score_threshold;
      assignment.confidence_margin_threshold_ = options.persistence->confidence_margin_threshold;
      assignment.top_score_count_             = options.persistence->top_score_count;

      std::string persist_error;
      qCInfo(diag::semanticDbLog).noquote()
          << QStringLiteral("semantic.db.embedding.persist.request count=%1 ids=%2 model_key=%3")
                 .arg(static_cast<qulonglong>(persist_records.size()))
                 .arg(SummarizeInputIds(inputs))
                 .arg(QString::fromStdString(options.persistence->model_key));
      if (!options.persistence->storage_controller->UpsertImageEmbeddingsAndAssignLabels(
              persist_records, assignment, &assigned_labels, &persist_error)) {
        const std::string message =
            persist_error.empty() ? "semantic persistence failed" : std::move(persist_error);
        qCWarning(diag::semanticDbLog).noquote()
            << QStringLiteral("semantic.db.embedding.persist.failed count=%1 error=%2")
                   .arg(static_cast<qulonglong>(persist_records.size()))
                   .arg(QString::fromStdString(message));
        for (const size_t index : persist_indices) {
          item_results[index].status = SemanticGenerationItemStatus::kError;
          item_results[index].error  = message;
        }
        persist_indices.clear();
      } else {
        qCInfo(diag::semanticDbLog).noquote()
            << QStringLiteral("semantic.db.embedding.persist.ok count=%1 labels=%2")
                   .arg(static_cast<qulonglong>(persist_records.size()))
                   .arg(static_cast<qulonglong>(assigned_labels.size()));
      }
    }

    // Fill in success results for persisted items (labels come back in input order).
    for (size_t k = 0; k < persist_indices.size(); ++k) {
      auto& item_result = item_results[persist_indices[k]];
      item_result.status    = SemanticGenerationItemStatus::kEmbedded;
      item_result.embedding = std::move(persist_records[k].embedding_);
      const auto& label     = assigned_labels[k];
      if (!label.label_.empty()) {
        item_result.has_label          = true;
        item_result.label              = label.label_;
        item_result.label_score        = label.score_;
        item_result.second_label       = label.second_label_;
        item_result.second_label_score = label.second_score_.value_or(0.0);
        item_result.label_margin       = label.margin_;
        item_result.label_confident    = label.confident_;
      }
    }

    for (auto& item_result : item_results) {
      qCInfo(diag::semanticLog).noquote()
          << QStringLiteral(
                 "semantic.embedding.item file_id=%1 image_id=%2 request_id=%3 status=%4 "
                 "label=%5 score=%6 error=%7")
                 .arg(static_cast<qulonglong>(item_result.item.element_id))
                 .arg(static_cast<qulonglong>(item_result.item.image_id))
                 .arg(QString::fromStdString(item_result.request_id))
                 .arg(QString::fromLatin1(ToString(item_result.status)))
                 .arg(QString::fromStdString(item_result.label))
                 .arg(item_result.label_score, 0, 'f', 4)
                 .arg(QString::fromStdString(item_result.error));
      switch (item_result.status) {
        case SemanticGenerationItemStatus::kEmbedded:
          job->UpdateProgress(
              [](SemanticGenerationProgress& progress) { progress.embedded++; });
          break;
        case SemanticGenerationItemStatus::kError:
          job->UpdateProgress([](SemanticGenerationProgress& progress) { progress.failed++; });
          break;
        default:
          break;
      }
      job->AppendResult(std::move(item_result));
    }
    DispatchProgress(job, on_progress);
  };

  auto drain_embedding_batches = [&](bool wait_for_one) {
    bool drained_any = false;
    while (true) {
      bool made_progress = false;
      for (auto it = in_flight_batches.begin(); it != in_flight_batches.end();) {
        if (job->IsCanceled()) {
          append_batch_canceled_results(it->inputs, "semantic generation job was canceled");
          it            = in_flight_batches.erase(it);
          drained_any   = true;
          made_progress = true;
          continue;
        }

        if (it->future.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
          auto                                           inputs = std::move(it->inputs);
          std::vector<SemanticImageEmbeddingBatchResult> results;
          try {
            results = it->future.get();
          } catch (const std::exception& e) {
            it = in_flight_batches.erase(it);
            append_batch_error_results(inputs,
                                       std::string("image embedding batch failed: ") + e.what());
            drained_any   = true;
            made_progress = true;
            continue;
          }
          it = in_flight_batches.erase(it);
          process_embedding_batch(inputs, std::move(results));
          drained_any   = true;
          made_progress = true;
          continue;
        }

        if (std::chrono::steady_clock::now() >= it->deadline) {
          append_batch_error_results(it->inputs, "image embedding batch timed out");
          it            = in_flight_batches.erase(it);
          drained_any   = true;
          made_progress = true;
          continue;
        }
        ++it;
      }

      if (made_progress || !wait_for_one || in_flight_batches.empty()) {
        return drained_any;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
  };

  auto strip_embedding_inputs_for_result_mapping =
      [](const std::vector<SemanticImageEmbeddingInput>& source) {
        std::vector<SemanticImageEmbeddingInput> stripped;
        stripped.reserve(source.size());
        for (const auto& input : source) {
          SemanticImageEmbeddingInput copy;
          copy.item        = input.item;
          copy.request_id  = input.request_id;
          copy.format_hint = input.format_hint;
          stripped.push_back(std::move(copy));
        }
        return stripped;
      };

  auto flush_batch = [&]() {
    if (batch.empty()) {
      return;
    }

    const auto batch_count   = batch.size();
    auto       pending_batch = std::move(batch);
    batch.clear();

    while (in_flight_batches.size() >= kMaxInFlightEmbeddingBatches) {
      drain_embedding_batches(true);
    }

    job->UpdateProgress([batch_count](SemanticGenerationProgress& progress) {
      progress.embedding_requested += batch_count;
    });
    DispatchProgress(job, on_progress);

    qCInfo(diag::semanticLog).noquote()
        << QStringLiteral("semantic.embedding.request count=%1 ids=%2")
               .arg(static_cast<qulonglong>(batch_count))
               .arg(SummarizeInputIds(pending_batch));

    auto promise = std::make_shared<std::promise<std::vector<SemanticImageEmbeddingBatchResult>>>();
    auto future  = promise->get_future();
    auto inputs_for_result_mapping = strip_embedding_inputs_for_result_mapping(pending_batch);
    embedding_client->EmbedImageBatch(
        std::move(pending_batch), options.embedding_timeout,
        [promise](std::vector<SemanticImageEmbeddingBatchResult> results) {
          try {
            promise->set_value(std::move(results));
          } catch (...) {
          }
        });

    in_flight_batches.push_back(InFlightEmbeddingBatch{
        .inputs   = std::move(inputs_for_result_mapping),
        .future   = std::move(future),
        .deadline = std::chrono::steady_clock::now() + options.embedding_timeout});
  };

  for (size_t offset = 0; offset < pending_items.size();) {
    drain_embedding_batches(false);

    if (job->IsCanceled()) {
      for (; offset < pending_items.size(); ++offset) {
        const auto&                  item = pending_items[offset];
        SemanticGenerationItemResult result;
        result.item       = item;
        result.request_id = MakeRequestId(item);
        result.status     = SemanticGenerationItemStatus::kCanceled;
        result.error      = "semantic generation job was canceled";
        job->UpdateProgress([](SemanticGenerationProgress& progress) { progress.canceled++; });
        job->AppendResult(std::move(result));
      }
      break;
    }

    const size_t chunk_size = std::min(options.thumbnail_batch_size, pending_items.size() - offset);
    std::vector<SemanticGenerationItem> thumbnail_chunk(
        pending_items.begin() + static_cast<std::ptrdiff_t>(offset),
        pending_items.begin() + static_cast<std::ptrdiff_t>(offset + chunk_size));
    offset += chunk_size;

    auto thumbnail_results = WaitForThumbnailBatch(job, thumbnail_provider, thumbnail_chunk,
                                                   options.thumbnail_resolution);
    qCInfo(diag::semanticLog).noquote()
        << QStringLiteral("semantic.thumbnail.batch.ready count=%1 ids=%2")
               .arg(static_cast<qulonglong>(thumbnail_chunk.size()))
               .arg(SummarizeItemIds(thumbnail_chunk));

    for (size_t i = 0; i < thumbnail_chunk.size(); ++i) {
      const auto& item             = thumbnail_chunk[i];
      auto&       thumbnail_result = thumbnail_results[i];

      if (thumbnail_result.status == ThumbnailRequestStatus::kCanceled || job->IsCanceled()) {
        SemanticGenerationItemResult result;
        result.item       = item;
        result.request_id = MakeRequestId(item);
        result.status     = SemanticGenerationItemStatus::kCanceled;
        result.error      = thumbnail_result.message.empty() ? "thumbnail request was canceled"
                                                             : thumbnail_result.message;
        if (thumbnail_result.guard) {
          thumbnail_provider->ReleaseThumbnail(thumbnail_result.key);
        }
        job->UpdateProgress([](SemanticGenerationProgress& progress) { progress.canceled++; });
        job->AppendResult(std::move(result));
        DispatchProgress(job, on_progress);
        continue;
      }

      if (thumbnail_result.status != ThumbnailRequestStatus::kReady || !thumbnail_result.guard) {
        SemanticGenerationItemResult result;
        result.item       = item;
        result.request_id = MakeRequestId(item);
        result.status     = SemanticGenerationItemStatus::kError;
        result.error      = thumbnail_result.message.empty() ? "thumbnail request failed"
                                                             : thumbnail_result.message;
        job->UpdateProgress([](SemanticGenerationProgress& progress) { progress.failed++; });
        job->AppendResult(std::move(result));
        DispatchProgress(job, on_progress);
        continue;
      }

      job->UpdateProgress(
          [](SemanticGenerationProgress& progress) { progress.thumbnails_ready++; });

      SemanticImageEmbeddingInput input;
      input.item       = item;
      input.request_id = MakeRequestId(item);
      std::string encode_error;
      const bool  materialized = MaterializeThumbnailRgba8(
          *thumbnail_result.guard, &input.rgba8_image, &input.format_hint, &encode_error);
      thumbnail_provider->ReleaseThumbnail(thumbnail_result.key);

      if (!materialized) {
        SemanticGenerationItemResult result;
        result.item       = item;
        result.request_id = input.request_id;
        result.status     = SemanticGenerationItemStatus::kError;
        result.error      = std::move(encode_error);
        job->UpdateProgress([](SemanticGenerationProgress& progress) { progress.failed++; });
        job->AppendResult(std::move(result));
        DispatchProgress(job, on_progress);
        continue;
      }

      batch.push_back(std::move(input));
      if (batch.size() >= options.embedding_batch_size) {
        flush_batch();
      }
    }
  }

  flush_batch();
  while (!in_flight_batches.empty()) {
    drain_embedding_batches(true);
  }
  finish();
}

}  // namespace alcedo
