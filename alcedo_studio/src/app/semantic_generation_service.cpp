//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/semantic_generation_service.hpp"

#include <QStringList>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <exception>
#include <future>
#include <mutex>
#include <numeric>
#include <optional>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "utils/diagnostics/app_logging.hpp"

namespace alcedo {
namespace {

auto MakeRequestId(const SemanticGenerationItem& item) -> std::string {
  return "semantic-image-" + std::to_string(item.element_id) + "-" + std::to_string(item.image_id);
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

auto MatchesExpectedModelInfo(const AiSidecarRuntimeModelInfo& actual,
                              const AiSidecarRuntimeModelInfo& expected, std::string* error)
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
  if (!persistence.label_store || !persistence.embedding_store) {
    if (error) {
      *error = "semantic storage is not available";
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
      persistence.label_store->CountLabelQueries(persistence.prompt_config_hash);
  if (query_count == 0) {
    if (error) {
      *error = "semantic label query table has no rows for prompt config " +
               persistence.prompt_config_hash;
    }
    return false;
  }

  const auto prototype_count = persistence.label_store->CountLabelPrototypes(
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
      persistence.label_store->ListLabelQueries(persistence.prompt_config_hash, error);
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

  return persistence.label_store->UpsertLabelPrototypes(prototypes, error);
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
    case SemanticGenerationItemStatus::kInputReady:
      return "input_ready";
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

auto MockSemanticImageEmbeddingClient::GetModelInfo(AiSidecarRuntimeModelInfo* info,
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

AiSidecarRuntimeImageEmbeddingClient::AiSidecarRuntimeImageEmbeddingClient(
    std::shared_ptr<AiSidecarRuntimeService> runtime)
    : runtime_(std::move(runtime)) {}

auto AiSidecarRuntimeImageEmbeddingClient::GetModelInfo(AiSidecarRuntimeModelInfo* info,
                                                       std::string*              error) -> bool {
  if (!runtime_) {
    if (error) {
      *error = "semantic runtime service is not available";
    }
    return false;
  }

  const auto status = runtime_->Status();
  if (status.state != AiSidecarRuntimeState::kReady) {
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

auto AiSidecarRuntimeImageEmbeddingClient::EmbedText(const std::string&        request_id,
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
  const auto session = runtime_->ClientSession();
  if (!session || runtime_->Status().state != AiSidecarRuntimeState::kReady) {
    SemanticEmbeddingResult result;
    result.request_id = request_id;
    result.ok         = false;
    result.error      = "semantic runtime is not ready";
    return result;
  }
  return session->semantic().EmbedText(request_id, text, timeout);
}

auto AiSidecarRuntimeImageEmbeddingClient::EmbedTextBatch(
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
  const auto session = runtime_->ClientSession();
  if (!session || runtime_->Status().state != AiSidecarRuntimeState::kReady) {
    std::vector<SemanticEmbeddingResult> results;
    results.reserve(requests.size());
    for (const auto& request : requests) {
      SemanticEmbeddingResult result;
      result.request_id = request.request_id;
      result.ok         = false;
      result.error      = "semantic runtime is not ready";
      results.push_back(std::move(result));
    }
    return results;
  }
  return session->semantic().EmbedTextBatch(requests, timeout);
}

void AiSidecarRuntimeImageEmbeddingClient::EmbedImageBatch(
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

    const auto session = runtime->ClientSession();
    std::vector<SemanticEmbeddingResult> runtime_results;
    if (!session || runtime->Status().state != AiSidecarRuntimeState::kReady) {
      runtime_results.reserve(requests.size());
      for (const auto& request : requests) {
        SemanticEmbeddingResult result;
        result.request_id = request.request_id;
        result.ok         = false;
        result.error      = "semantic runtime is not ready";
        runtime_results.push_back(std::move(result));
      }
    } else {
      runtime_results = session->semantic().EmbedImageBatch(std::move(requests), timeout);
    }
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
    std::shared_ptr<ISemanticInputImageSource>     input_source,
    std::shared_ptr<ISemanticImageEmbeddingClient> embedding_client)
    : input_source_(std::move(input_source)), embedding_client_(std::move(embedding_client)) {
  if (!input_source_) {
    throw std::invalid_argument("SemanticGenerationService requires an input image source");
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
  if (options.embedding_batch_size == 0) {
    options.embedding_batch_size = 1;
  }
  if (options.max_in_flight_embedding_batches == 0) {
    options.max_in_flight_embedding_batches = 1;
  }
  if (options.persistence.has_value() && options.persistence->label_prototype_batch_size == 0) {
    options.persistence->label_prototype_batch_size = 1;
  }

  auto job = std::make_shared<SemanticGenerationJob>();
  job->UpdateProgress(
      [total = items.size()](SemanticGenerationProgress& progress) { progress.total = total; });

  qCInfo(diag::semanticLog).noquote()
      << QStringLiteral(
             "semantic.generation.start total=%1 input_window=%2 embedding_batch=%3 "
             "embedding_batches_in_flight=%4 force=%5")
             .arg(static_cast<qulonglong>(items.size()))
             .arg(static_cast<qulonglong>(input_source_->ConcurrencyLimit()))
             .arg(static_cast<qulonglong>(options.embedding_batch_size))
             .arg(static_cast<qulonglong>(options.max_in_flight_embedding_batches))
             .arg(options.force_regenerate ? QStringLiteral("true") : QStringLiteral("false"));

  auto input_source     = input_source_;
  auto embedding_client = embedding_client_;
  auto worker =
      std::thread([job, items = std::move(items), options, on_progress = std::move(on_progress),
                   on_finished = std::move(on_finished), input_source = std::move(input_source),
                   embedding_client = std::move(embedding_client)]() mutable {
        RunJob(job, items, options, std::move(on_progress), std::move(on_finished),
               std::move(input_source), std::move(embedding_client));
      });
  job->SetWorkerThread(std::move(worker));

  return job;
}

void SemanticGenerationService::RunJob(
    const std::shared_ptr<SemanticGenerationJob>& job,
    const std::vector<SemanticGenerationItem>& items, SemanticGenerationOptions options,
    SemanticGenerationProgressCallback on_progress, SemanticGenerationFinishedCallback on_finished,
    std::shared_ptr<ISemanticInputImageSource>     input_source,
    std::shared_ptr<ISemanticImageEmbeddingClient> embedding_client) {
  using Clock                 = std::chrono::steady_clock;
  const auto job_start        = Clock::now();
  auto       last_publish     = Clock::time_point{};
  const auto publish_progress = [&](bool force) {
    const auto now = Clock::now();
    if (!force && now - last_publish < options.progress_interval) {
      return;
    }
    last_publish = now;
    DispatchProgress(job, on_progress);
  };

  // Released before the finish callback: the source joins its workers when it ends, and the
  // owner of the job may wait for this thread right after the callback.
  const auto finish = [&]() {
    input_source.reset();
    publish_progress(true);
    auto results = job->Results();
    job->Finish();
    if (on_finished) {
      on_finished(std::move(results));
    }
  };

  const auto fail_all = [&](const std::vector<SemanticGenerationItem>& failed_items,
                            const std::string&                         message) {
    for (const auto& item : failed_items) {
      SemanticGenerationItemResult result;
      result.item       = item;
      result.request_id = MakeRequestId(item);
      result.status     = SemanticGenerationItemStatus::kError;
      result.error      = message;
      job->AppendResult(std::move(result));
    }
    job->UpdateProgress([count = failed_items.size()](SemanticGenerationProgress& progress) {
      progress.failed += count;
    });
    qCWarning(diag::semanticLog).noquote()
        << QStringLiteral("semantic.generation.failed count=%1 error=%2")
               .arg(static_cast<qulonglong>(failed_items.size()))
               .arg(QString::fromStdString(message));
  };

  if (options.expected_model_info.has_value()) {
    AiSidecarRuntimeModelInfo actual_model_info;
    std::string              model_error;
    if (!embedding_client->GetModelInfo(&actual_model_info, &model_error) ||
        !MatchesExpectedModelInfo(actual_model_info, *options.expected_model_info, &model_error)) {
      fail_all(items,
               model_error.empty() ? "semantic runtime model info is not compatible" : model_error);
      finish();
      return;
    }
  }

  // Images that already have a ready, labeled embedding of the model: one query for the job.
  std::vector<SemanticGenerationItem> pending_items;
  pending_items.reserve(items.size());
  if (options.persistence.has_value() && !options.force_regenerate &&
      options.persistence->embedding_store) {
    std::string ready_error;
    const auto  ready = options.persistence->embedding_store->ListReadyLabeledImages(
        options.persistence->model_key, &ready_error);
    if (!ready_error.empty()) {
      fail_all(items, "cannot read the images that have labels: " + ready_error);
      finish();
      return;
    }
    std::unordered_set<uint64_t> ready_keys;
    ready_keys.reserve(ready.size());
    for (const auto& [file_id, image_id] : ready) {
      ready_keys.insert((static_cast<uint64_t>(file_id) << 32U) | image_id);
    }
    size_t skipped = 0;
    for (const auto& item : items) {
      if (ready_keys.contains((static_cast<uint64_t>(item.element_id) << 32U) | item.image_id)) {
        SemanticGenerationItemResult result;
        result.item       = item;
        result.request_id = MakeRequestId(item);
        result.status     = SemanticGenerationItemStatus::kSkipped;
        result.error      = "semantic embedding already exists for the active model";
        job->AppendResult(std::move(result));
        ++skipped;
      } else {
        pending_items.push_back(item);
      }
    }
    job->UpdateProgress(
        [skipped](SemanticGenerationProgress& progress) { progress.skipped += skipped; });
    qCInfo(diag::semanticLog).noquote()
        << QStringLiteral("semantic.generation.pending total=%1 skipped=%2 pending=%3")
               .arg(static_cast<qulonglong>(items.size()))
               .arg(static_cast<qulonglong>(skipped))
               .arg(static_cast<qulonglong>(pending_items.size()));
    if (pending_items.empty()) {
      finish();
      return;
    }
    publish_progress(true);
  } else {
    pending_items = items;
  }

  if (options.persistence.has_value()) {
    std::string label_error;
    if (!EnsureLabelPrototypes(*options.persistence, embedding_client, options.embedding_timeout,
                               &label_error)) {
      fail_all(pending_items, label_error.empty()
                                  ? "semantic label prototype cache is not available"
                                  : label_error);
      finish();
      return;
    }
  }

  // Stage timing for the throughput log lines.
  struct StageTotals {
    size_t                    preview_inputs = 0;
    size_t                    render_inputs  = 0;
    std::chrono::milliseconds embed_time{0};
    std::chrono::milliseconds persist_time{0};
    std::chrono::milliseconds embed_wait_time{0};
  } totals;
  const auto processing_start = Clock::now();
  auto       last_rate_log    = processing_start;
  const auto log_throughput   = [&](const char* event) {
    const auto progress = job->SnapshotProgress();
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - processing_start);
    const double seconds = std::max(0.001, static_cast<double>(elapsed.count()) / 1000.0);
    qCInfo(diag::semanticLog).noquote()
        << QStringLiteral(
               "%1 embedded=%2 failed=%3 canceled=%4 elapsed_ms=%5 images_per_s=%6 preview=%7 "
                 "render=%8 embed_ms=%9 embed_wait_ms=%10 persist_ms=%11")
               .arg(QString::fromLatin1(event))
               .arg(static_cast<qulonglong>(progress.embedded))
               .arg(static_cast<qulonglong>(progress.failed))
               .arg(static_cast<qulonglong>(progress.canceled))
               .arg(static_cast<qlonglong>(elapsed.count()))
               .arg(static_cast<double>(progress.embedded + progress.failed) / seconds, 0, 'f', 2)
               .arg(static_cast<qulonglong>(totals.preview_inputs))
               .arg(static_cast<qulonglong>(totals.render_inputs))
               .arg(static_cast<qlonglong>(totals.embed_time.count()))
               .arg(static_cast<qlonglong>(totals.embed_wait_time.count()))
               .arg(static_cast<qlonglong>(totals.persist_time.count()));
  };

  std::vector<SemanticImageEmbeddingInput> batch;
  batch.reserve(options.embedding_batch_size);

  struct InFlightEmbeddingBatch {
    std::vector<SemanticImageEmbeddingInput>                    inputs;
    std::future<std::vector<SemanticImageEmbeddingBatchResult>> future;
    Clock::time_point                                           submitted;
    Clock::time_point                                           deadline;
  };
  std::deque<InFlightEmbeddingBatch> in_flight_batches;

  const auto append_status_results = [&](const std::vector<SemanticImageEmbeddingInput>& inputs,
                                         SemanticGenerationItemStatus                    status,
                                         const std::string&                              message) {
    for (const auto& input : inputs) {
      SemanticGenerationItemResult result;
      result.item       = input.item;
      result.request_id = input.request_id;
      result.status     = status;
      result.error      = message;
      job->AppendResult(std::move(result));
    }
    job->UpdateProgress([count = inputs.size(), status](SemanticGenerationProgress& progress) {
      if (status == SemanticGenerationItemStatus::kCanceled) {
        progress.canceled += count;
      } else {
        progress.failed += count;
      }
    });
    if (status == SemanticGenerationItemStatus::kError) {
      qCWarning(diag::semanticLog).noquote()
          << QStringLiteral("semantic.embedding.batch.failed count=%1 ids=%2 error=%3")
                 .arg(static_cast<qulonglong>(inputs.size()))
                 .arg(SummarizeInputIds(inputs))
                 .arg(QString::fromStdString(message));
    }
    publish_progress(false);
  };

  const auto process_embedding_batch =
      [&](const std::vector<SemanticImageEmbeddingInput>& inputs,
          std::vector<SemanticImageEmbeddingBatchResult>  batch_results) {
        batch_results = MapEmbeddingBatchResults(inputs, std::move(batch_results));

        // Build item results in batch order. Valid embeddings are persisted in one DuckDB
        // transaction for the batch.
        std::vector<SemanticGenerationItemResult> item_results;
        item_results.reserve(batch_results.size());
        std::vector<size_t>                       persist_indices;
        std::vector<SemanticImageEmbeddingRecord> persist_records;

        for (size_t i = 0; i < batch_results.size(); ++i) {
          auto&                        batch_result = batch_results[i];
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
          if (!options.persistence.has_value()) {
            item_result.status    = SemanticGenerationItemStatus::kEmbedded;
            item_result.embedding = std::move(batch_result.embedding.embedding);
            item_results.push_back(std::move(item_result));
            continue;
          }
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
          record.embedding_            = std::move(batch_result.embedding.embedding);
          record.thumbnail_resolution_ = static_cast<int>(inputs[i].long_edge);
          persist_indices.push_back(i);
          persist_records.push_back(std::move(record));
          item_results.push_back(std::move(item_result));  // status filled after persist
        }

        std::vector<SemanticImageLabelRecord> assigned_labels;
        if (options.persistence.has_value() && !persist_records.empty()) {
          SemanticLabelAssignmentOptions assignment;
          assignment.prompt_config_hash_         = options.persistence->prompt_config_hash;
          assignment.confidence_score_threshold_ = options.persistence->confidence_score_threshold;
          assignment.confidence_margin_threshold_ =
              options.persistence->confidence_margin_threshold;
          assignment.top_score_count_ = options.persistence->top_score_count;

          std::string persist_error;
          const auto  persist_start = Clock::now();
          const bool  persisted =
              options.persistence->embedding_store->UpsertImageEmbeddingsAndAssignLabels(
                  persist_records, assignment, &assigned_labels, &persist_error);
          totals.persist_time +=
              std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - persist_start);
          if (!persisted) {
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
          }
        }

        // Labels come back in the order of the persisted records.
        for (size_t k = 0; k < persist_indices.size(); ++k) {
          auto& item_result  = item_results[persist_indices[k]];
          item_result.status = SemanticGenerationItemStatus::kEmbedded;
          const auto& label  = assigned_labels[k];
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

        size_t embedded = 0;
        size_t failed   = 0;
        for (auto& item_result : item_results) {
          if (item_result.status == SemanticGenerationItemStatus::kEmbedded) {
            ++embedded;
          } else {
            ++failed;
            qCWarning(diag::semanticLog).noquote()
                << QStringLiteral("semantic.embedding.item.failed file_id=%1 image_id=%2 error=%3")
                       .arg(static_cast<qulonglong>(item_result.item.element_id))
                       .arg(static_cast<qulonglong>(item_result.item.image_id))
                       .arg(QString::fromStdString(item_result.error));
          }
          job->AppendResult(std::move(item_result));
        }
        job->UpdateProgress([embedded, failed](SemanticGenerationProgress& progress) {
          progress.embedded += embedded;
          progress.failed += failed;
        });
        publish_progress(false);
      };

  // Takes finished embedding batches. With @p wait_for_one, waits until at least one finished.
  const auto drain_embedding_batches = [&](bool wait_for_one) {
    const auto wait_start = Clock::now();
    while (true) {
      bool made_progress = false;
      for (auto it = in_flight_batches.begin(); it != in_flight_batches.end();) {
        if (job->IsCanceled()) {
          append_status_results(it->inputs, SemanticGenerationItemStatus::kCanceled,
                                "semantic generation job was canceled");
          it            = in_flight_batches.erase(it);
          made_progress = true;
          continue;
        }
        if (it->future.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
          totals.embed_time +=
              std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - it->submitted);
          auto                                           inputs = std::move(it->inputs);
          std::vector<SemanticImageEmbeddingBatchResult> results;
          try {
            results = it->future.get();
          } catch (const std::exception& e) {
            it = in_flight_batches.erase(it);
            append_status_results(inputs, SemanticGenerationItemStatus::kError,
                                  std::string("image embedding batch failed: ") + e.what());
            made_progress = true;
            continue;
          }
          it = in_flight_batches.erase(it);
          process_embedding_batch(inputs, std::move(results));
          made_progress = true;
          continue;
        }
        if (Clock::now() >= it->deadline) {
          append_status_results(it->inputs, SemanticGenerationItemStatus::kError,
                                "image embedding batch timed out");
          it            = in_flight_batches.erase(it);
          made_progress = true;
          continue;
        }
        ++it;
      }
      if (made_progress || !wait_for_one || in_flight_batches.empty()) {
        if (wait_for_one) {
          totals.embed_wait_time +=
              std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - wait_start);
        }
        return;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
  };

  const auto flush_batch = [&]() {
    if (batch.empty()) {
      return;
    }
    while (in_flight_batches.size() >= options.max_in_flight_embedding_batches) {
      drain_embedding_batches(true);
    }
    auto pending_batch = std::move(batch);
    batch.clear();
    batch.reserve(options.embedding_batch_size);

    job->UpdateProgress([count = pending_batch.size()](SemanticGenerationProgress& progress) {
      progress.embedding_requested += count;
    });

    auto promise = std::make_shared<std::promise<std::vector<SemanticImageEmbeddingBatchResult>>>();
    auto future  = promise->get_future();
    std::vector<SemanticImageEmbeddingInput> inputs_for_result_mapping;
    inputs_for_result_mapping.reserve(pending_batch.size());
    for (const auto& input : pending_batch) {
      SemanticImageEmbeddingInput copy;
      copy.item        = input.item;
      copy.request_id  = input.request_id;
      copy.format_hint = input.format_hint;
      copy.long_edge   = input.long_edge;
      inputs_for_result_mapping.push_back(std::move(copy));
    }
    const auto submitted = Clock::now();
    embedding_client->EmbedImageBatch(
        std::move(pending_batch), options.embedding_timeout,
        [promise](std::vector<SemanticImageEmbeddingBatchResult> results) {
          try {
            promise->set_value(std::move(results));
          } catch (...) {
          }
        });
    in_flight_batches.push_back(
        InFlightEmbeddingBatch{.inputs    = std::move(inputs_for_result_mapping),
                               .future    = std::move(future),
                               .submitted = submitted,
                               .deadline  = submitted + options.embedding_timeout});
  };

  // Input results arrive on the source's workers; this thread takes them from the queue.
  struct InputQueue {
    std::mutex                                              lock;
    std::condition_variable                                 cv;
    std::deque<std::pair<size_t, SemanticInputImageResult>> ready;
  };
  auto              queue          = std::make_shared<InputQueue>();
  // Read by the source's workers; this thread sets it when the job is canceled or ends, so a
  // request that has not started does no work.
  auto              stop           = std::make_shared<std::atomic<bool>>(false);
  const auto        stop_requested = [stop]() { return stop->load(); };

  const size_t      window         = std::max<size_t>(1, input_source->ConcurrencyLimit());
  std::vector<bool> outstanding(pending_items.size(), false);
  size_t            next_index   = 0;
  size_t            in_flight    = 0;

  const auto        handle_input = [&](size_t index, SemanticInputImageResult input_result) {
    const auto& item = pending_items[index];
    if (input_result.status == SemanticInputImageStatus::kCanceled) {
      SemanticGenerationItemResult result;
      result.item       = item;
      result.request_id = MakeRequestId(item);
      result.status     = SemanticGenerationItemStatus::kCanceled;
      result.error      = std::move(input_result.message);
      job->AppendResult(std::move(result));
      job->UpdateProgress([](SemanticGenerationProgress& progress) { progress.canceled++; });
      return;
    }
    if (input_result.status != SemanticInputImageStatus::kReady ||
        input_result.image.rgba8.empty()) {
      SemanticGenerationItemResult result;
      result.item       = item;
      result.request_id = MakeRequestId(item);
      result.status     = SemanticGenerationItemStatus::kError;
      result.error      = input_result.message.empty() ? "the model input image is empty"
                                                              : std::move(input_result.message);
      qCWarning(diag::semanticLog).noquote()
          << QStringLiteral("semantic.input.failed file_id=%1 image_id=%2 error=%3")
                 .arg(static_cast<qulonglong>(item.element_id))
                 .arg(static_cast<qulonglong>(item.image_id))
                 .arg(QString::fromStdString(result.error));
      job->AppendResult(std::move(result));
      job->UpdateProgress([](SemanticGenerationProgress& progress) { progress.failed++; });
      return;
    }

    const bool from_preview =
        input_result.image.source == SemanticInputImageSource::kEmbeddedPreview;
    (from_preview ? totals.preview_inputs : totals.render_inputs)++;
    job->UpdateProgress([from_preview](SemanticGenerationProgress& progress) {
      progress.inputs_ready++;
      (from_preview ? progress.preview_inputs : progress.render_inputs)++;
    });

    SemanticImageEmbeddingInput input;
    input.item        = item;
    input.request_id  = MakeRequestId(item);
    input.format_hint = "rgba8:" + std::to_string(input_result.image.width) + "x" +
                        std::to_string(input_result.image.height);
    input.long_edge   = std::max(input_result.image.width, input_result.image.height);
    input.rgba8_image = std::move(input_result.image.rgba8);
    batch.push_back(std::move(input));
    if (batch.size() >= options.embedding_batch_size) {
      flush_batch();
    }
  };

  while (true) {
    if (job->IsCanceled()) {
      stop->store(true);
      break;
    }
    while (in_flight < window && next_index < pending_items.size()) {
      const size_t index = next_index++;
      const auto&  item  = pending_items[index];
      outstanding[index] = true;
      ++in_flight;
      input_source->Request(item.element_id, item.image_id, stop_requested,
                            [queue, index](SemanticInputImageResult result) {
                              {
                                std::scoped_lock lock(queue->lock);
                                queue->ready.emplace_back(index, std::move(result));
                              }
                              queue->cv.notify_one();
                            });
    }
    if (in_flight == 0) {
      break;
    }

    std::deque<std::pair<size_t, SemanticInputImageResult>> ready;
    {
      std::unique_lock lock(queue->lock);
      queue->cv.wait_for(lock, std::chrono::milliseconds(25),
                         [&queue]() { return !queue->ready.empty(); });
      ready.swap(queue->ready);
    }
    for (auto& [index, result] : ready) {
      outstanding[index] = false;
      --in_flight;
      handle_input(index, std::move(result));
    }
    drain_embedding_batches(false);
    publish_progress(false);

    if (Clock::now() - last_rate_log >= std::chrono::seconds(30)) {
      last_rate_log = Clock::now();
      log_throughput("semantic.generation.throughput");
    }
  }

  if (job->IsCanceled()) {
    // Requests that did not answer and items that were never requested end as canceled; the
    // source drops their work when it ends.
    size_t canceled = 0;
    for (size_t index = 0; index < pending_items.size(); ++index) {
      if (outstanding[index] || index >= next_index) {
        SemanticGenerationItemResult result;
        result.item       = pending_items[index];
        result.request_id = MakeRequestId(pending_items[index]);
        result.status     = SemanticGenerationItemStatus::kCanceled;
        result.error      = "semantic generation job was canceled";
        job->AppendResult(std::move(result));
        ++canceled;
      }
    }
    job->UpdateProgress(
        [canceled](SemanticGenerationProgress& progress) { progress.canceled += canceled; });
    append_status_results(batch, SemanticGenerationItemStatus::kCanceled,
                          "semantic generation job was canceled");
    batch.clear();
    drain_embedding_batches(false);
  } else {
    flush_batch();
    while (!in_flight_batches.empty()) {
      drain_embedding_batches(true);
    }
  }
  stop->store(true);

  log_throughput("semantic.generation.finished");
  qCInfo(diag::semanticLog).noquote()
      << QStringLiteral("semantic.generation.job_ms=%1")
             .arg(static_cast<qlonglong>(
                 std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - job_start)
                     .count()));
  finish();
}

}  // namespace alcedo
