//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/semantic_generation_service.hpp"

#include <duckdb.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <limits>
#include <mutex>
#include <numeric>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "app/import_service.hpp"
#include "app/pipeline_service.hpp"
#include "app/project_service.hpp"
#include "app/semantic_input_image_service.hpp"
#include "image/image.hpp"
#include "support/semantic_stores_test_support.hpp"
#include "support/test_temp_directory.hpp"
#include "type/supported_file_type.hpp"
#include "utils/clock/time_provider.hpp"
#include "../support/storage_test_access.hpp"

namespace alcedo {
namespace {
using namespace std::chrono_literals;

auto MakeTextEmbeddingResult(const std::string& request_id, uint32_t dimension)
    -> SemanticEmbeddingResult {
  SemanticEmbeddingResult result;
  result.request_id = request_id;
  result.model_name = "mock/mobileclip";
  result.dimension  = dimension;
  result.ok         = true;
  result.embedding.assign(dimension, 0.0F);
  if (dimension > 0) {
    result.embedding[0] = 1.0F;
  }
  return result;
}

class RecordingEmbeddingClient final : public ISemanticImageEmbeddingClient {
 public:
  explicit RecordingEmbeddingClient(std::chrono::milliseconds delay = 0ms) : delay_(delay) {}

  auto GetModelInfo(AiSidecarRuntimeModelInfo* info, std::string* error) -> bool override {
    (void)error;
    if (info) {
      info->model_id            = "mock/mobileclip";
      info->revision            = "mock-revision";
      info->embedding_dimension = 2;
      info->image_size          = 256;
      info->provider            = "mock";
    }
    return true;
  }

  auto EmbedText(const std::string& request_id, const std::string& text,
                 std::chrono::milliseconds timeout) -> SemanticEmbeddingResult override {
    (void)text;
    (void)timeout;
    return MakeTextEmbeddingResult(request_id, 2);
  }

  void EmbedImageBatch(std::vector<SemanticImageEmbeddingInput> inputs,
                       std::chrono::milliseconds                timeout,
                       SemanticImageEmbeddingBatchCallback      callback) override {
    (void)timeout;
    {
      std::unique_lock lock(lock_);
      batch_sizes_.push_back(inputs.size());
      for (const auto& input : inputs) {
        EXPECT_FALSE(input.rgba8_image.empty());
        EXPECT_TRUE(input.format_hint.starts_with("rgba8:"));
      }
    }

    const auto delay = delay_;
    std::thread([inputs = std::move(inputs), callback = std::move(callback), delay]() mutable {
      if (delay.count() > 0) {
        std::this_thread::sleep_for(delay);
      }

      std::vector<SemanticImageEmbeddingBatchResult> results;
      results.reserve(inputs.size());
      for (const auto& input : inputs) {
        SemanticImageEmbeddingBatchResult result;
        result.item                 = input.item;
        result.embedding.request_id = input.request_id;
        result.embedding.ok         = true;
        result.embedding.dimension  = 2;
        result.embedding.embedding  = {1.0f, 0.0f};
        results.push_back(std::move(result));
      }
      callback(std::move(results));
    }).detach();
  }

  auto BatchSizes() const -> std::vector<size_t> {
    std::unique_lock lock(lock_);
    return batch_sizes_;
  }

 private:
  std::chrono::milliseconds delay_;
  mutable std::mutex        lock_;
  std::vector<size_t>       batch_sizes_;
};

/// The real input service with request and source counts.
class CountingRealInputSource final : public ISemanticInputImageSource {
 public:
  explicit CountingRealInputSource(std::shared_ptr<SemanticInputImageService> inner)
      : inner_(std::move(inner)) {}

  void Request(sl_element_id_t element_id, image_id_t image_id, std::function<bool()> canceled,
               SemanticInputImageCallback callback) override {
    request_count_.fetch_add(1);
    inner_->Request(element_id, image_id, std::move(canceled),
                    [this, callback = std::move(callback)](SemanticInputImageResult result) {
                      if (result.status == SemanticInputImageStatus::kReady) {
                        (result.image.source == SemanticInputImageSource::kEmbeddedPreview
                             ? preview_count_
                             : render_count_)
                            .fetch_add(1);
                      }
                      callback(std::move(result));
                    });
  }

  auto ConcurrencyLimit() const -> size_t override { return inner_->ConcurrencyLimit(); }

  auto RequestCount() const -> int { return request_count_.load(); }
  auto PreviewCount() const -> int { return preview_count_.load(); }
  auto RenderCount() const -> int { return render_count_.load(); }

 private:
  std::shared_ptr<SemanticInputImageService> inner_;
  std::atomic<int>                           request_count_{0};
  std::atomic<int>                           preview_count_{0};
  std::atomic<int>                           render_count_{0};
};

auto MakeReadyInput(sl_element_id_t element_id, image_id_t image_id) -> SemanticInputImageResult {
  SemanticInputImageResult result;
  result.element_id   = element_id;
  result.image_id     = image_id;
  result.status       = SemanticInputImageStatus::kReady;
  result.image.width  = 2;
  result.image.height = 3;
  result.image.source = SemanticInputImageSource::kEmbeddedPreview;
  result.image.rgba8.assign(2 * 3 * 4, 128);
  return result;
}

/// Answers every request at once on the caller's thread with a 2x3 RGBA8 image.
class ImmediateInputSource final : public ISemanticInputImageSource {
 public:
  void Request(sl_element_id_t element_id, image_id_t image_id, std::function<bool()> canceled,
               SemanticInputImageCallback callback) override {
    (void)canceled;
    request_count_.fetch_add(1);
    callback(MakeReadyInput(element_id, image_id));
  }

  auto ConcurrencyLimit() const -> size_t override { return 4; }
  auto RequestCount() const -> int { return request_count_.load(); }

 private:
  std::atomic<int> request_count_{0};
};

/// Holds the requests that @p hold selects until the test answers them; answers the others at
/// once. Records the largest number of requests that were outstanding at one time.
class HeldInputSource final : public ISemanticInputImageSource {
 public:
  HeldInputSource(size_t concurrency_limit, std::function<bool(sl_element_id_t)> hold)
      : concurrency_limit_(concurrency_limit), hold_(std::move(hold)) {}

  void Request(sl_element_id_t element_id, image_id_t image_id, std::function<bool()> canceled,
               SemanticInputImageCallback callback) override {
    (void)canceled;
    {
      std::unique_lock lock(lock_);
      ++request_count_;
      ++outstanding_;
      max_outstanding_ = std::max(max_outstanding_, outstanding_);
      if (hold_(element_id)) {
        held_.push_back(Held{element_id, image_id, std::move(callback)});
        return;
      }
      --outstanding_;
    }
    callback(MakeReadyInput(element_id, image_id));
  }

  auto ConcurrencyLimit() const -> size_t override { return concurrency_limit_; }

  /// Answers every held request with a ready image.
  void ReleaseAll() {
    std::vector<Held> held;
    {
      std::unique_lock lock(lock_);
      held.swap(held_);
      outstanding_ -= held.size();
    }
    for (auto& request : held) {
      request.callback(MakeReadyInput(request.element_id, request.image_id));
    }
  }

  auto RequestCount() const -> size_t {
    std::unique_lock lock(lock_);
    return request_count_;
  }
  auto HeldCount() const -> size_t {
    std::unique_lock lock(lock_);
    return held_.size();
  }
  auto MaxOutstanding() const -> size_t {
    std::unique_lock lock(lock_);
    return max_outstanding_;
  }

 private:
  struct Held {
    sl_element_id_t            element_id = 0;
    image_id_t                 image_id   = 0;
    SemanticInputImageCallback callback;
  };

  size_t                               concurrency_limit_;
  std::function<bool(sl_element_id_t)> hold_;
  mutable std::mutex                   lock_;
  std::vector<Held>                    held_;
  size_t                               request_count_   = 0;
  size_t                               outstanding_     = 0;
  size_t                               max_outstanding_ = 0;
};

class ScriptedEmbeddingClient final : public ISemanticImageEmbeddingClient {
 public:
  auto GetModelInfo(AiSidecarRuntimeModelInfo* info, std::string* error) -> bool override {
    (void)error;
    if (info) {
      info->model_id            = "mock/mobileclip";
      info->revision            = "mock-revision";
      info->embedding_dimension = 2;
      info->image_size          = 256;
      info->provider            = "mock";
    }
    return true;
  }

  auto EmbedText(const std::string& request_id, const std::string& text,
                 std::chrono::milliseconds timeout) -> SemanticEmbeddingResult override {
    (void)text;
    (void)timeout;
    return MakeTextEmbeddingResult(request_id, 2);
  }

  void EmbedImageBatch(std::vector<SemanticImageEmbeddingInput> inputs,
                       std::chrono::milliseconds                timeout,
                       SemanticImageEmbeddingBatchCallback      callback) override {
    (void)timeout;
    std::vector<SemanticImageEmbeddingBatchResult> results;
    results.reserve(inputs.size());
    for (size_t i = 0; i < inputs.size(); ++i) {
      SemanticImageEmbeddingBatchResult result;
      result.item                 = inputs[i].item;
      result.embedding.request_id = inputs[i].request_id;
      result.embedding.dimension  = 2;
      if (i == 1) {
        result.embedding.ok    = false;
        result.embedding.error = "scripted partial failure";
      } else {
        result.embedding.ok        = true;
        result.embedding.embedding = {1.0f, 0.0f};
      }
      if (i == 2) {
        result.embedding.request_id = "unexpected-request-id";
      }
      results.push_back(std::move(result));
    }
    callback(std::move(results));
  }
};

class NeverRespondingEmbeddingClient final : public ISemanticImageEmbeddingClient {
 public:
  auto GetModelInfo(AiSidecarRuntimeModelInfo* info, std::string* error) -> bool override {
    (void)error;
    if (info) {
      info->model_id            = "mock/mobileclip";
      info->revision            = "mock-revision";
      info->embedding_dimension = 2;
      info->image_size          = 256;
    }
    return true;
  }

  auto EmbedText(const std::string& request_id, const std::string& text,
                 std::chrono::milliseconds timeout) -> SemanticEmbeddingResult override {
    (void)text;
    (void)timeout;
    return MakeTextEmbeddingResult(request_id, 2);
  }

  void EmbedImageBatch(std::vector<SemanticImageEmbeddingInput> inputs,
                       std::chrono::milliseconds                timeout,
                       SemanticImageEmbeddingBatchCallback      callback) override {
    (void)inputs;
    (void)timeout;
    std::unique_lock lock(lock_);
    callbacks_.push_back(std::move(callback));
  }

 private:
  std::mutex                                       lock_;
  std::vector<SemanticImageEmbeddingBatchCallback> callbacks_;
};

auto OneHot512(size_t index) -> std::vector<float> {
  std::vector<float> embedding(kSemanticEmbeddingDim, 0.0F);
  embedding.at(index) = 1.0F;
  return embedding;
}

auto Mixed512(size_t primary, size_t secondary, float secondary_score) -> std::vector<float> {
  auto embedding          = OneHot512(primary);
  embedding.at(secondary) = secondary_score;
  return embedding;
}

auto LabelIndex(const std::string& label) -> size_t {
  const auto& queries = DefaultSemanticPhotographyLabelQueries();
  for (size_t i = 0; i < queries.size(); ++i) {
    if (queries[i].label == label) {
      return i;
    }
  }
  return 0;
}

auto LabelIndex(SemanticLabelLanguage language, const std::string& label) -> size_t {
  const auto& queries = DefaultSemanticPhotographyLabelQueries(language);
  for (size_t i = 0; i < queries.size(); ++i) {
    if (queries[i].label == label) {
      return i;
    }
  }
  return 0;
}

void RegisterSemanticTestModel(const semantic_test::SemanticStores& semantic) {
  std::string error;
  ASSERT_TRUE(semantic.models_.UpsertModel(SemanticModelRecord{.model_key_     = "mobileclip-test",
                                                       .model_id_      = "mock/mobileclip",
                                                       .revision_      = "mock-revision",
                                                       .embedding_dim_ = kSemanticEmbeddingDim,
                                                       .image_size_    = 256},
                                   &error))
      << error;
}

void RegisterLocalizedSemanticTestModel(const semantic_test::SemanticStores& semantic) {
  std::string error;
  ASSERT_TRUE(semantic.models_.UpsertModel(
      SemanticModelRecord{.model_key_     = "localized-zh-test",
                          .model_id_      = "mock/localized-zh",
                          .revision_      = "mock-zh-revision",
                          .embedding_dim_ = kSemanticEmbeddingDim,
                          .image_size_    = 256,
                          .engine_id_     = "mock-localized",
                          .profile_id_    = "mock-localized-zh",
                          .supported_text_languages_json_ =
                              SemanticSupportedTextLanguagesJson(SemanticLabelLanguage::kChinese),
                          .prompt_config_hash_ = kDefaultSemanticPhotographyZhPromptConfigHash,
                          .active_             = true},
      &error))
      << error;
}

class Fixed512EmbeddingClient final : public ISemanticImageEmbeddingClient {
 public:
  explicit Fixed512EmbeddingClient(std::vector<float> embedding)
      : embedding_(std::move(embedding)) {}

  auto GetModelInfo(AiSidecarRuntimeModelInfo* info, std::string* error) -> bool override {
    (void)error;
    if (info) {
      info->model_id            = "mock/mobileclip";
      info->revision            = "mock-revision";
      info->embedding_dimension = kSemanticEmbeddingDim;
      info->image_size          = 256;
      info->provider            = "mock";
    }
    return true;
  }

  auto EmbedText(const std::string& request_id, const std::string& text,
                 std::chrono::milliseconds timeout) -> SemanticEmbeddingResult override {
    (void)timeout;
    SemanticEmbeddingResult result;
    result.request_id = request_id;
    result.model_name = "mock/mobileclip";
    result.dimension  = kSemanticEmbeddingDim;
    result.ok         = true;
    std::string label;
    for (const auto& query : DefaultSemanticPhotographyLabelQueries()) {
      if (query.query == text) {
        label = query.label;
        break;
      }
    }
    if (label == "street") {
      result.embedding = OneHot512(8);
    } else if (label == "landscape") {
      result.embedding    = OneHot512(2);
      result.embedding[8] = 0.25F;
    } else if (label == "portrait") {
      result.embedding    = OneHot512(9);
      result.embedding[8] = 0.1F;
    } else {
      result.embedding = OneHot512(0);
    }
    return result;
  }

  auto EmbedTextBatch(const std::vector<SemanticTextEmbeddingRequest>& requests,
                      std::chrono::milliseconds                        timeout)
      -> std::vector<SemanticEmbeddingResult> override {
    {
      std::unique_lock lock(lock_);
      text_batch_sizes_.push_back(requests.size());
    }
    std::vector<SemanticEmbeddingResult> results;
    results.reserve(requests.size());
    for (const auto& request : requests) {
      results.push_back(EmbedText(request.request_id, request.text, timeout));
    }
    return results;
  }

  void EmbedImageBatch(std::vector<SemanticImageEmbeddingInput> inputs,
                       std::chrono::milliseconds                timeout,
                       SemanticImageEmbeddingBatchCallback      callback) override {
    (void)timeout;
    image_batch_call_count_.fetch_add(1);
    image_item_count_.fetch_add(static_cast<int>(inputs.size()));
    std::vector<SemanticImageEmbeddingBatchResult> results;
    results.reserve(inputs.size());
    for (const auto& input : inputs) {
      SemanticImageEmbeddingBatchResult result;
      result.item                 = input.item;
      result.embedding.request_id = input.request_id;
      result.embedding.ok         = true;
      result.embedding.dimension  = kSemanticEmbeddingDim;
      result.embedding.embedding  = embedding_;
      results.push_back(std::move(result));
    }
    callback(std::move(results));
  }

  auto ImageBatchCallCount() const -> int { return image_batch_call_count_.load(); }
  auto ImageItemCount() const -> int { return image_item_count_.load(); }
  auto TextBatchSizes() const -> std::vector<size_t> {
    std::unique_lock lock(lock_);
    return text_batch_sizes_;
  }

 private:
  std::vector<float>  embedding_;
  std::atomic<int>    image_batch_call_count_{0};
  std::atomic<int>    image_item_count_{0};
  mutable std::mutex  lock_;
  std::vector<size_t> text_batch_sizes_;
};

class Routed512EmbeddingClient final : public ISemanticImageEmbeddingClient {
 public:
  explicit Routed512EmbeddingClient(std::unordered_map<sl_element_id_t, std::vector<float>> routes)
      : routes_(std::move(routes)) {
    size_t index = 0;
    for (const auto& query : DefaultSemanticPhotographyLabelQueries()) {
      label_to_index_.emplace(query.label, index);
      query_to_index_.emplace(query.query, index);
      ++index;
    }
  }

  auto GetModelInfo(AiSidecarRuntimeModelInfo* info, std::string* error) -> bool override {
    (void)error;
    if (info) {
      info->model_id            = "mock/mobileclip";
      info->revision            = "mock-revision";
      info->embedding_dimension = kSemanticEmbeddingDim;
      info->image_size          = 256;
      info->provider            = "mock";
    }
    return true;
  }

  auto EmbedText(const std::string& request_id, const std::string& text,
                 std::chrono::milliseconds timeout) -> SemanticEmbeddingResult override {
    (void)timeout;
    SemanticEmbeddingResult result;
    result.request_id = request_id;
    result.model_name = "mock/mobileclip";
    result.dimension  = kSemanticEmbeddingDim;
    result.ok         = true;
    const auto found  = query_to_index_.find(text);
    result.embedding  = OneHot512(found == query_to_index_.end() ? 0 : found->second);
    return result;
  }

  void EmbedImageBatch(std::vector<SemanticImageEmbeddingInput> inputs,
                       std::chrono::milliseconds                timeout,
                       SemanticImageEmbeddingBatchCallback      callback) override {
    (void)timeout;
    image_item_count_.fetch_add(static_cast<int>(inputs.size()));
    std::vector<SemanticImageEmbeddingBatchResult> results;
    results.reserve(inputs.size());
    for (const auto& input : inputs) {
      SemanticImageEmbeddingBatchResult result;
      result.item                 = input.item;
      result.embedding.request_id = input.request_id;
      result.embedding.ok         = true;
      result.embedding.dimension  = kSemanticEmbeddingDim;
      const auto found            = routes_.find(input.item.element_id);
      result.embedding.embedding =
          found == routes_.end() ? OneHot512(IndexForLabel("product")) : found->second;
      results.push_back(std::move(result));
    }
    callback(std::move(results));
  }

  auto IndexForLabel(const std::string& label) const -> size_t {
    const auto found = label_to_index_.find(label);
    return found == label_to_index_.end() ? 0 : found->second;
  }

  auto ImageItemCount() const -> int { return image_item_count_.load(); }

 private:
  std::unordered_map<sl_element_id_t, std::vector<float>> routes_;
  std::unordered_map<std::string, size_t>                 label_to_index_;
  std::unordered_map<std::string, size_t>                 query_to_index_;
  std::atomic<int>                                        image_item_count_{0};
};

class Localized512EmbeddingClient final : public ISemanticImageEmbeddingClient {
 public:
  Localized512EmbeddingClient(SemanticLabelLanguage language, std::string image_label)
      : language_(language), image_label_(std::move(image_label)) {
    size_t index = 0;
    for (const auto& query : DefaultSemanticPhotographyLabelQueries(language_)) {
      query_to_index_.emplace(query.query, index++);
    }
  }

  auto GetModelInfo(AiSidecarRuntimeModelInfo* info, std::string* error) -> bool override {
    (void)error;
    if (info) {
      info->profile_id          = "mock-localized-zh";
      info->model_id            = "mock/localized-zh";
      info->revision            = "mock-zh-revision";
      info->embedding_dimension = kSemanticEmbeddingDim;
      info->image_size          = 256;
      info->provider            = "mock";
      info->language            = "zh";
    }
    return true;
  }

  auto EmbedText(const std::string& request_id, const std::string& text,
                 std::chrono::milliseconds timeout) -> SemanticEmbeddingResult override {
    (void)timeout;
    SemanticEmbeddingResult result;
    result.request_id = request_id;
    result.model_name = "mock/localized-zh";
    result.dimension  = kSemanticEmbeddingDim;
    result.ok         = true;
    const auto found  = query_to_index_.find(text);
    result.embedding  = OneHot512(found == query_to_index_.end() ? 0 : found->second);
    return result;
  }

  void EmbedImageBatch(std::vector<SemanticImageEmbeddingInput> inputs,
                       std::chrono::milliseconds                timeout,
                       SemanticImageEmbeddingBatchCallback      callback) override {
    (void)timeout;
    std::vector<SemanticImageEmbeddingBatchResult> results;
    results.reserve(inputs.size());
    const auto image_index = LabelIndex(language_, image_label_);
    for (const auto& input : inputs) {
      SemanticImageEmbeddingBatchResult result;
      result.item                 = input.item;
      result.embedding.request_id = input.request_id;
      result.embedding.ok         = true;
      result.embedding.dimension  = kSemanticEmbeddingDim;
      result.embedding.embedding  = OneHot512(image_index);
      results.push_back(std::move(result));
    }
    callback(std::move(results));
  }

 private:
  SemanticLabelLanguage                   language_;
  std::string                             image_label_;
  std::unordered_map<std::string, size_t> query_to_index_;
};

auto RawScalarInt64(duckdb_connection connection, const std::string& sql) -> int64_t {
  duckdb_result result;
  if (duckdb_query(connection, sql.c_str(), &result) != DuckDBSuccess) {
    const char* raw_error = duckdb_result_error(&result);
    std::string message   = raw_error ? raw_error : "raw DuckDB query failed";
    duckdb_destroy_result(&result);
    throw std::runtime_error(message);
  }
  int64_t value = 0;
  if (duckdb_row_count(&result) > 0 && duckdb_column_count(&result) > 0 &&
      !duckdb_value_is_null(&result, 0, 0)) {
    value = duckdb_value_int64(&result, 0, 0);
  }
  duckdb_destroy_result(&result);
  return value;
}

template <typename Predicate>
auto WaitUntil(Predicate predicate, std::chrono::milliseconds timeout = 120s) -> bool {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate()) {
      return true;
    }
    std::this_thread::sleep_for(10ms);
  }
  return predicate();
}

// Collects RAW fixture paths for the real-import generation tests.
//
// CI pulls a curated RAW set via git-lfs under TEST_IMG_PATH/ci_rawfiles, and
// that is the only fixture directory guaranteed to exist there. Local dev may
// keep larger samples under raw/batch_import (flat) or raw/cameras (recursive),
// so those are used as fallbacks when present. Returns an empty vector when no
// fixtures are available; callers are expected to GTEST_SKIP() in that case.
//
// git-lfs pointer files (a ~130-byte text stub) have the right extension but
// are not decodable RAW, so a checkout whose LFS objects were not pulled would
// otherwise make libraw fail mid-test. Such pointers are filtered out here; if
// every fixture is an unpulled pointer, the result is empty and callers skip.
auto IsLfsPointer(const std::filesystem::path& path) -> bool {
  std::ifstream file(path, std::ios::binary);
  if (!file.is_open()) {
    return false;
  }
  char magic[32] = {0};
  file.read(magic, sizeof(magic) - 1);
  return std::string_view(magic).starts_with("version https://git-lfs");
}

auto CollectSemanticRawFixturePaths(size_t max_count, bool recursive) -> std::vector<image_path_t> {
  const auto try_collect = [&](const std::filesystem::path& dir) -> std::vector<image_path_t> {
    std::vector<image_path_t> paths;
    if (!std::filesystem::exists(dir)) {
      return paths;
    }
    if (recursive) {
      for (const auto& entry : std::filesystem::recursive_directory_iterator(dir)) {
        if (!entry.is_regular_file() || !is_supported_file(entry.path())) {
          continue;
        }
        if (IsLfsPointer(entry.path())) {
          continue;
        }
        paths.push_back(entry.path());
        if (max_count != 0 && paths.size() >= max_count) {
          break;
        }
      }
    } else {
      for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        if (!entry.is_regular_file() || !is_supported_file(entry.path())) {
          continue;
        }
        if (IsLfsPointer(entry.path())) {
          continue;
        }
        paths.push_back(entry.path());
        if (max_count != 0 && paths.size() >= max_count) {
          break;
        }
      }
    }
    std::sort(paths.begin(), paths.end());
    return paths;
  };

  auto paths = try_collect(std::filesystem::path(TEST_IMG_PATH) / "ci_rawfiles");
  if (!paths.empty()) {
    return paths;
  }
  if (recursive) {
    for (const auto* name : {"cameras", "camera"}) {
      paths = try_collect(std::filesystem::path(TEST_IMG_PATH) / "raw" / name);
      if (!paths.empty()) {
        return paths;
      }
    }
  } else {
    paths = try_collect(std::filesystem::path(TEST_IMG_PATH) / "raw" / "batch_import");
    if (!paths.empty()) {
      return paths;
    }
  }
  return paths;
}

class SemanticGenerationServiceTest : public ::testing::Test {
 protected:
  test::TestTempDirectory temp_dir_;
  void SetUp() override {
    TimeProvider::Refresh();
    db_path_   = temp_dir_ / "project.db";
    meta_path_ = temp_dir_ / "project.json";
  }

  auto ImportItems(ProjectService& project, size_t count) -> std::vector<SemanticGenerationItem> {
    auto                      fs_service = project.GetSleeveService();
    auto                      img_pool   = project.GetImagePoolService();

    std::vector<image_path_t> paths = CollectSemanticRawFixturePaths(count, /*recursive=*/false);
    if (paths.size() < count) {
      // Fixtures missing — return empty so the caller GTEST_SKIP()s from the test
      // body. GTEST_SKIP() expands to a `return`, so it can only short-circuit a
      // void test body, not this vector-returning helper.
      return {};
    }

    // Product import writes camera/profile data into the stored document. Thumbnail
    // DAG rendering reads that document; omitting PipelineMgmtService leaves a Default
    // graph without camera matrices and Metal/CUDA camera color fails.
    auto                       pipelines = std::make_shared<PipelineMgmtService>(project.GetStorage());
    ImportServiceImpl          import_service(fs_service, img_pool, pipelines);
    auto                       import_job = std::make_shared<ImportJob>();
    std::promise<ImportResult> done;
    auto                       future = done.get_future();
    import_job->on_finished_ = [&done](const ImportResult& result) { done.set_value(result); };

    import_job               = import_service.ImportToFolder(paths, L"", {}, import_job);
    EXPECT_NE(import_job, nullptr);
    EXPECT_EQ(future.wait_for(120s), std::future_status::ready);
    const auto result = future.get();
    EXPECT_EQ(result.failed_, 0u);

    const auto snapshot = import_job->import_log_->Snapshot();
    import_service.SyncImports(snapshot, L"");
    fs_service->Sync();
    img_pool->SyncWithStorage();
    project.SaveProject(meta_path_);

    std::vector<SemanticGenerationItem> items;
    items.reserve(snapshot.created_.size());
    for (const auto& created : snapshot.created_) {
      items.push_back(SemanticGenerationItem{created.element_id_, created.image_id_});
    }
    return items;
  }

  auto ImportPaths(ProjectService& project, const std::vector<image_path_t>& paths)
      -> std::vector<SemanticGenerationItem> {
    auto                       fs_service = project.GetSleeveService();
    auto                       img_pool   = project.GetImagePoolService();

    // Same as ImportItems: assemble and persist camera/profile data before thumbnail DAG render.
    auto                       pipelines = std::make_shared<PipelineMgmtService>(project.GetStorage());
    ImportServiceImpl          import_service(fs_service, img_pool, pipelines);
    auto                       import_job = std::make_shared<ImportJob>();
    std::promise<ImportResult> done;
    auto                       future = done.get_future();
    import_job->on_finished_ = [&done](const ImportResult& result) { done.set_value(result); };

    import_job               = import_service.ImportToFolder(paths, L"", {}, import_job);
    EXPECT_NE(import_job, nullptr);
    EXPECT_EQ(future.wait_for(300s), std::future_status::ready);
    const auto result = future.get();
    EXPECT_GT(result.imported_, 0u);

    const auto snapshot = import_job->import_log_->Snapshot();
    import_service.SyncImports(snapshot, L"");
    fs_service->Sync();
    img_pool->SyncWithStorage();
    project.SaveProject(meta_path_);

    std::vector<SemanticGenerationItem> items;
    items.reserve(snapshot.created_.size());
    for (const auto& created : snapshot.created_) {
      if (created.element_id_ != 0 && created.image_id_ != 0) {
        items.push_back(SemanticGenerationItem{created.element_id_, created.image_id_});
      }
    }
    return items;
  }

  auto CollectCameraSampleImages() const -> std::vector<image_path_t> {
    return CollectSemanticRawFixturePaths(/*max_count=*/0, /*recursive=*/true);
  }

  std::filesystem::path db_path_;
  std::filesystem::path meta_path_;
};

}  // namespace

TEST_F(SemanticGenerationServiceTest, UsesRealInputServiceAndBatchesMockEmbedding) {
  ProjectService project(db_path_, meta_path_, ProjectOpenMode::kCreateNew);
  auto           items = ImportItems(project, 3);
  if (items.size() < 3) {
    GTEST_SKIP() << "CI RAW fixtures missing under TEST_IMG_PATH/ci_rawfiles; pull the "
                    "git-lfs fixtures to exercise real-import semantic generation";
  }

  auto pipeline_service = std::make_shared<PipelineMgmtService>(project.GetStorage());
  auto inputs =
      std::make_shared<CountingRealInputSource>(std::make_shared<SemanticInputImageService>(
          project.GetImagePoolService(), pipeline_service, 256));
  auto                      embedder = std::make_shared<RecordingEmbeddingClient>();
  SemanticGenerationService service(inputs, embedder);

  SemanticGenerationOptions options;
  options.embedding_batch_size            = 2;
  options.max_in_flight_embedding_batches = 1;

  auto job                                = service.StartGeneration(items, options);
  job->Wait();

  const auto progress = job->SnapshotProgress();
  EXPECT_EQ(progress.total, 3u);
  EXPECT_EQ(progress.inputs_ready, 3u);
  EXPECT_EQ(progress.preview_inputs + progress.render_inputs, 3u);
  EXPECT_EQ(progress.embedding_requested, 3u);
  EXPECT_EQ(progress.embedded, 3u);
  EXPECT_EQ(progress.failed, 0u);
  EXPECT_EQ(progress.canceled, 0u);
  EXPECT_EQ(inputs->RequestCount(), 3);
  EXPECT_EQ(inputs->PreviewCount() + inputs->RenderCount(), 3);

  const std::vector<size_t> expected_batches{2, 1};
  EXPECT_EQ(embedder->BatchSizes(), expected_batches);

  const auto results = job->Results();
  ASSERT_EQ(results.size(), 3u);
  for (const auto& result : results) {
    EXPECT_EQ(result.status, SemanticGenerationItemStatus::kEmbedded);
    EXPECT_EQ(result.embedding_dimension, 2u);
    EXPECT_FALSE(result.embedding.empty());
  }

  pipeline_service->Sync();
}

// The job keeps the source's concurrency limit of requests outstanding and starts the next request
// when one answers; it does not wait for a whole group. The embedding batch size is independent.
TEST_F(SemanticGenerationServiceTest, KeepsTheInputWindowFullAndBatchesEmbeddingsApart) {
  auto inputs   = std::make_shared<HeldInputSource>(2, [](sl_element_id_t) { return true; });
  auto embedder = std::make_shared<RecordingEmbeddingClient>(100ms);
  SemanticGenerationService service(inputs, embedder);

  SemanticGenerationOptions options;
  options.embedding_batch_size = 4;
  options.embedding_timeout    = 3s;
  auto job = service.StartGeneration({{1, 10}, {2, 20}, {3, 30}, {4, 40}}, options);

  ASSERT_TRUE(WaitUntil([&]() { return inputs->HeldCount() == 2U; }, 2s));
  std::this_thread::sleep_for(50ms);
  EXPECT_EQ(inputs->RequestCount(), 2U);

  inputs->ReleaseAll();
  ASSERT_TRUE(
      WaitUntil([&]() { return inputs->HeldCount() == 2U && inputs->RequestCount() == 4U; }, 2s));
  inputs->ReleaseAll();
  job->Wait();

  EXPECT_EQ(inputs->MaxOutstanding(), 2U);
  const auto progress = job->SnapshotProgress();
  EXPECT_EQ(progress.inputs_ready, 4U);
  EXPECT_EQ(progress.embedded, 4U);
  const std::vector<size_t> expected_batches{4};
  EXPECT_EQ(embedder->BatchSizes(), expected_batches);
}

// One input that takes long does not stop the others: every other image is requested and embedded
// while that request is still outstanding.
TEST_F(SemanticGenerationServiceTest, SlowInputDoesNotStopTheOtherImages) {
  auto inputs   = std::make_shared<HeldInputSource>(3, [](sl_element_id_t id) { return id == 1; });
  auto embedder = std::make_shared<RecordingEmbeddingClient>();
  SemanticGenerationService service(inputs, embedder);

  SemanticGenerationOptions options;
  options.embedding_batch_size = 2;
  auto job =
      service.StartGeneration({{1, 10}, {2, 20}, {3, 30}, {4, 40}, {5, 50}, {6, 60}}, options);

  ASSERT_TRUE(WaitUntil([&]() { return job->SnapshotProgress().embedded == 4U; }, 3s));
  EXPECT_EQ(inputs->RequestCount(), 6U);
  EXPECT_EQ(inputs->HeldCount(), 1U);

  inputs->ReleaseAll();
  job->Wait();
  EXPECT_EQ(job->SnapshotProgress().embedded, 6U);
  EXPECT_LE(inputs->MaxOutstanding(), 3U);
}

// Cancel ends the job without waiting for outstanding inputs; every image that has no embedding is
// reported as canceled, and a late answer of the source changes nothing.
TEST_F(SemanticGenerationServiceTest, CancelEndsTheJobWithOutstandingInputsCanceled) {
  auto inputs   = std::make_shared<HeldInputSource>(2, [](sl_element_id_t) { return true; });
  auto embedder = std::make_shared<RecordingEmbeddingClient>();
  SemanticGenerationService service(inputs, embedder);

  auto                      job = service.StartGeneration({{1, 10}, {2, 20}, {3, 30}});
  ASSERT_TRUE(WaitUntil([&]() { return inputs->HeldCount() == 2U; }, 2s));

  job->Cancel();
  job->Wait();
  inputs->ReleaseAll();

  const auto progress = job->SnapshotProgress();
  EXPECT_EQ(progress.embedded, 0U);
  EXPECT_EQ(progress.canceled, 3U);
  const auto results = job->Results();
  ASSERT_EQ(results.size(), 3U);
  for (const auto& result : results) {
    EXPECT_EQ(result.status, SemanticGenerationItemStatus::kCanceled);
  }
  EXPECT_TRUE(embedder->BatchSizes().empty());
}

TEST_F(SemanticGenerationServiceTest, RealInputFailureSkipsMockEmbeddingForThatItem) {
  ProjectService project(db_path_, meta_path_, ProjectOpenMode::kCreateNew);
  auto           items = ImportItems(project, 1);
  if (items.empty()) {
    GTEST_SKIP() << "CI RAW fixtures missing under TEST_IMG_PATH/ci_rawfiles; pull the "
                    "git-lfs fixtures to exercise real-import semantic generation";
  }
  items.push_back(SemanticGenerationItem{999999u, 999999u});

  auto pipeline_service = std::make_shared<PipelineMgmtService>(project.GetStorage());
  auto inputs =
      std::make_shared<CountingRealInputSource>(std::make_shared<SemanticInputImageService>(
          project.GetImagePoolService(), pipeline_service, 256));
  auto                      embedder = std::make_shared<RecordingEmbeddingClient>();
  SemanticGenerationService service(inputs, embedder);

  SemanticGenerationOptions options;
  options.embedding_batch_size = 8;

  auto job                     = service.StartGeneration(items, options);
  job->Wait();

  const auto progress = job->SnapshotProgress();
  EXPECT_EQ(progress.total, 2u);
  EXPECT_EQ(progress.inputs_ready, 1u);
  EXPECT_EQ(progress.embedded, 1u);
  EXPECT_EQ(progress.failed, 1u);
  EXPECT_EQ(inputs->RequestCount(), 2);

  const std::vector<size_t> expected_batches{1};
  EXPECT_EQ(embedder->BatchSizes(), expected_batches);

  pipeline_service->Sync();
}

TEST_F(SemanticGenerationServiceTest, CancelDuringMockEmbeddingEndsWithTheImageCanceled) {
  ProjectService project(db_path_, meta_path_, ProjectOpenMode::kCreateNew);
  auto           items = ImportItems(project, 1);
  if (items.empty()) {
    GTEST_SKIP() << "CI RAW fixtures missing under TEST_IMG_PATH/ci_rawfiles; pull the "
                    "git-lfs fixtures to exercise real-import semantic generation";
  }

  auto pipeline_service = std::make_shared<PipelineMgmtService>(project.GetStorage());
  auto inputs =
      std::make_shared<CountingRealInputSource>(std::make_shared<SemanticInputImageService>(
          project.GetImagePoolService(), pipeline_service, 256));
  auto                      embedder = std::make_shared<RecordingEmbeddingClient>(500ms);
  SemanticGenerationService service(inputs, embedder);

  auto                      job = service.StartGeneration(items);
  ASSERT_TRUE(WaitUntil([&]() { return embedder->BatchSizes().size() == 1; }));

  job->Cancel();
  job->Wait();

  const auto progress = job->SnapshotProgress();
  EXPECT_EQ(progress.embedded, 0u);
  EXPECT_EQ(progress.canceled, 1u);

  pipeline_service->Sync();
}

TEST_F(SemanticGenerationServiceTest, RejectsModelInfoMismatchBeforeRequestingInputs) {
  auto                       inputs   = std::make_shared<ImmediateInputSource>();
  auto                       embedder = std::make_shared<RecordingEmbeddingClient>();
  SemanticGenerationService  service(inputs, embedder);

  SemanticGenerationOptions options;
  AiSidecarRuntimeModelInfo  expected;
  expected.model_id            = "mock/mobileclip";
  expected.revision            = "mock-revision";
  expected.embedding_dimension = 512;
  expected.image_size          = 256;
  expected.provider            = "mock";
  options.expected_model_info  = expected;

  auto job                     = service.StartGeneration({{1, 10}, {2, 20}}, options);
  job->Wait();

  const auto progress = job->SnapshotProgress();
  EXPECT_EQ(progress.total, 2u);
  EXPECT_EQ(progress.failed, 2u);
  EXPECT_EQ(progress.inputs_ready, 0u);
  EXPECT_EQ(inputs->RequestCount(), 0);

  const auto results = job->Results();
  ASSERT_EQ(results.size(), 2u);
  for (const auto& result : results) {
    EXPECT_EQ(result.status, SemanticGenerationItemStatus::kError);
    EXPECT_NE(result.error.find("embedding dimension mismatch"), std::string::npos);
  }
}

TEST_F(SemanticGenerationServiceTest, MapsPartialFailureAndRequestIdMismatchPerItem) {
  auto                      inputs   = std::make_shared<ImmediateInputSource>();
  auto                      embedder = std::make_shared<ScriptedEmbeddingClient>();
  SemanticGenerationService service(inputs, embedder);

  SemanticGenerationOptions options;
  options.embedding_batch_size = 3;
  AiSidecarRuntimeModelInfo expected;
  expected.model_id            = "mock/mobileclip";
  expected.revision            = "mock-revision";
  expected.embedding_dimension = 2;
  expected.image_size          = 256;
  expected.provider            = "mock";
  options.expected_model_info  = expected;

  auto job                     = service.StartGeneration({{1, 10}, {2, 20}, {3, 30}}, options);
  job->Wait();

  const auto progress = job->SnapshotProgress();
  EXPECT_EQ(progress.total, 3u);
  EXPECT_EQ(progress.inputs_ready, 3u);
  EXPECT_EQ(progress.embedding_requested, 3u);
  EXPECT_EQ(progress.embedded, 1u);
  EXPECT_EQ(progress.failed, 2u);

  const auto results = job->Results();
  ASSERT_EQ(results.size(), 3u);
  EXPECT_EQ(results[0].status, SemanticGenerationItemStatus::kEmbedded);
  EXPECT_EQ(results[1].status, SemanticGenerationItemStatus::kError);
  EXPECT_EQ(results[1].error, "scripted partial failure");
  EXPECT_EQ(results[2].status, SemanticGenerationItemStatus::kError);
  EXPECT_NE(results[2].error.find("missing image embedding response"), std::string::npos);
}

TEST_F(SemanticGenerationServiceTest, EmbeddingTimeoutFailsEveryPendingItem) {
  auto                      inputs   = std::make_shared<ImmediateInputSource>();
  auto                      embedder = std::make_shared<NeverRespondingEmbeddingClient>();
  SemanticGenerationService service(inputs, embedder);

  SemanticGenerationOptions options;
  options.embedding_batch_size = 2;
  options.embedding_timeout    = 50ms;

  auto job                     = service.StartGeneration({{1, 10}, {2, 20}}, options);
  job->Wait();

  const auto progress = job->SnapshotProgress();
  EXPECT_EQ(progress.embedding_requested, 2u);
  EXPECT_EQ(progress.embedded, 0u);
  EXPECT_EQ(progress.failed, 2u);

  const auto results = job->Results();
  ASSERT_EQ(results.size(), 2u);
  for (const auto& result : results) {
    EXPECT_EQ(result.status, SemanticGenerationItemStatus::kError);
    EXPECT_NE(result.error.find("timed out"), std::string::npos);
  }
}

TEST_F(SemanticGenerationServiceTest, DefaultPhotographyLabelsLiveInConfigHeader) {
  const auto& labels = DefaultSemanticPhotographyLabels();
  EXPECT_GE(labels.size(), 40U);
  EXPECT_NE(std::find(labels.begin(), labels.end(), "portrait"), labels.end());
  EXPECT_NE(std::find(labels.begin(), labels.end(), "landscape"), labels.end());
  EXPECT_NE(std::find(labels.begin(), labels.end(), "street"), labels.end());
  EXPECT_NE(std::find(labels.begin(), labels.end(), "product"), labels.end());
}

TEST_F(SemanticGenerationServiceTest, PersistsEmbeddingsAndAssignedLabels) {
  ProjectService project(db_path_, meta_path_, ProjectOpenMode::kCreateNew);
  auto           semantic = semantic_test::StoresOf(*project.GetStorage());
  RegisterSemanticTestModel(semantic);

  auto                      inputs   = std::make_shared<ImmediateInputSource>();
  auto                      embedder = std::make_shared<Fixed512EmbeddingClient>(OneHot512(8));
  SemanticGenerationService service(inputs, embedder);

  SemanticGenerationOptions options;
  options.expected_model_info =
      AiSidecarRuntimeModelInfo{.model_id            = "mock/mobileclip",
                               .revision            = "mock-revision",
                               .embedding_dimension = kSemanticEmbeddingDim,
                               .image_size          = 256,
                               .provider            = "mock"};
  SemanticGenerationPersistenceOptions persistence;
  persistence.label_store                = &semantic.labels_;
  persistence.embedding_store            = &semantic.embeddings_;
  persistence.model_key                  = "mobileclip-test";
  persistence.label_prototype_batch_size = 7;
  options.persistence                    = persistence;

  std::string error;
  auto        job = service.StartGeneration({{42, 420}}, options);
  job->Wait();

  const auto progress = job->SnapshotProgress();
  EXPECT_EQ(progress.embedded, 1U);
  EXPECT_EQ(progress.failed, 0U);
  EXPECT_EQ(
      semantic.labels_.CountLabelPrototypes("mobileclip-test", kDefaultSemanticPhotographyPromptConfigHash),
      DefaultSemanticPhotographyLabelQueries().size());
  const auto text_batch_sizes = embedder->TextBatchSizes();
  ASSERT_GT(text_batch_sizes.size(), 1U);
  EXPECT_EQ(text_batch_sizes.front(), 7U);
  EXPECT_EQ(std::accumulate(text_batch_sizes.begin(), text_batch_sizes.end(), size_t{0}),
            DefaultSemanticPhotographyLabelQueries().size());
  EXPECT_EQ(semantic.embeddings_.CountImageEmbeddingsForFile(42, "mobileclip-test"), 1U);
  EXPECT_EQ(semantic.labels_.CountImageLabelsForFile(42, "mobileclip-test"), 1U);

  const auto results = job->Results();
  ASSERT_EQ(results.size(), 1U);
  EXPECT_EQ(results.front().status, SemanticGenerationItemStatus::kEmbedded);
  EXPECT_TRUE(results.front().has_label);
  EXPECT_EQ(results.front().label, "street");
  EXPECT_TRUE(results.front().label_confident);
  // With persistence the store owns the vector; the job keeps no copy of it.
  EXPECT_TRUE(results.front().embedding.empty());

  const auto stored_label = semantic.labels_.GetImageLabelForFile(42, "mobileclip-test", &error);
  ASSERT_TRUE(stored_label.has_value()) << error;
  EXPECT_EQ(stored_label->label_, "street");
  // The image is a pure one-hot "street" vector; every other prototype scores at most 0.25
  // (landscape shares the street axis synthetically). The elbow sees a cliff after the top
  // match and keeps only one label, so no spurious second tag is assigned.
  EXPECT_TRUE(stored_label->second_label_.empty());
}

// A Chinese prompt profile ranks Chinese label text (风景). The label store keeps the canonical
// key, and the UI translates that key for each language.
TEST_F(SemanticGenerationServiceTest, PersistsCanonicalKeysForChinesePromptsAndMapsDisplayText) {
  ProjectService project(db_path_, meta_path_, ProjectOpenMode::kCreateNew);
  auto           semantic = semantic_test::StoresOf(*project.GetStorage());
  RegisterLocalizedSemanticTestModel(semantic);

  auto inputs   = std::make_shared<ImmediateInputSource>();
  auto embedder = std::make_shared<Localized512EmbeddingClient>(SemanticLabelLanguage::kChinese,
                                                                "\xE9\xA3\x8E\xE6\x99\xAF");
  SemanticGenerationService service(inputs, embedder);

  SemanticGenerationOptions options;
  options.expected_model_info =
      AiSidecarRuntimeModelInfo{.profile_id          = "mock-localized-zh",
                               .model_id            = "mock/localized-zh",
                               .revision            = "mock-zh-revision",
                               .language            = "zh",
                               .embedding_dimension = kSemanticEmbeddingDim,
                               .image_size          = 256,
                               .provider            = "mock"};
  SemanticGenerationPersistenceOptions persistence;
  persistence.label_store     = &semantic.labels_;
  persistence.embedding_store = &semantic.embeddings_;
  persistence.model_key          = "localized-zh-test";
  persistence.prompt_config_hash = kDefaultSemanticPhotographyZhPromptConfigHash;
  options.persistence            = persistence;

  std::string error;
  auto        job = service.StartGeneration({{42, 420}}, options);
  job->Wait();

  const auto progress = job->SnapshotProgress();
  EXPECT_EQ(progress.embedded, 1U);
  EXPECT_EQ(progress.failed, 0U);
  EXPECT_EQ(semantic.labels_.CountLabelPrototypes("localized-zh-test",
                                          kDefaultSemanticPhotographyZhPromptConfigHash),
            DefaultSemanticPhotographyLabelQueries(SemanticLabelLanguage::kChinese).size());
  EXPECT_EQ(semantic.labels_.CountImageLabelsForFile(42, "localized-zh-test"), 1U);

  const auto results = job->Results();
  ASSERT_EQ(results.size(), 1U);
  EXPECT_EQ(results.front().status, SemanticGenerationItemStatus::kEmbedded);
  EXPECT_TRUE(results.front().has_label);
  EXPECT_EQ(results.front().label, "landscape");

  const auto stored_label = semantic.labels_.GetImageLabelForFile(42, "localized-zh-test", &error);
  ASSERT_TRUE(stored_label.has_value()) << error;
  EXPECT_EQ(stored_label->label_, "landscape");
  EXPECT_EQ(SemanticLabelDisplayText(stored_label->label_, SemanticLabelLanguage::kEnglish),
            "landscape");
  EXPECT_EQ(SemanticLabelDisplayText(stored_label->label_, SemanticLabelLanguage::kChinese),
            "\xE9\xA3\x8E\xE6\x99\xAF");
}

TEST_F(SemanticGenerationServiceTest, SkipsReadyEmbeddingsUnlessForceRegenerate) {
  ProjectService project(db_path_, meta_path_, ProjectOpenMode::kCreateNew);
  auto           semantic = semantic_test::StoresOf(*project.GetStorage());
  RegisterSemanticTestModel(semantic);

  auto inputs   = std::make_shared<ImmediateInputSource>();
  auto embedder = std::make_shared<Fixed512EmbeddingClient>(OneHot512(LabelIndex("street")));
  SemanticGenerationService            service(inputs, embedder);

  SemanticGenerationOptions            options;
  SemanticGenerationPersistenceOptions persistence;
  persistence.label_store     = &semantic.labels_;
  persistence.embedding_store = &semantic.embeddings_;
  persistence.model_key          = "mobileclip-test";
  options.persistence            = persistence;

  auto first                     = service.StartGeneration({{42, 420}}, options);
  first->Wait();
  EXPECT_EQ(first->SnapshotProgress().embedded, 1U);
  EXPECT_EQ(inputs->RequestCount(), 1);
  EXPECT_EQ(embedder->ImageItemCount(), 1);

  auto retry = service.StartGeneration({{42, 420}}, options);
  retry->Wait();
  const auto retry_progress = retry->SnapshotProgress();
  EXPECT_EQ(retry_progress.skipped, 1U);
  EXPECT_EQ(retry_progress.embedded, 0U);
  EXPECT_EQ(inputs->RequestCount(), 1);
  EXPECT_EQ(embedder->ImageItemCount(), 1);
  ASSERT_EQ(retry->Results().size(), 1U);
  EXPECT_EQ(retry->Results().front().status, SemanticGenerationItemStatus::kSkipped);

  options.force_regenerate = true;
  auto forced              = service.StartGeneration({{42, 420}}, options);
  forced->Wait();
  EXPECT_EQ(forced->SnapshotProgress().embedded, 1U);
  EXPECT_EQ(inputs->RequestCount(), 2);
  EXPECT_EQ(embedder->ImageItemCount(), 2);
}

TEST_F(SemanticGenerationServiceTest, GeneratesLabelsForRecursiveCameraSampleDatabaseAndSqlChecks) {
  const auto paths = CollectCameraSampleImages();
  if (paths.empty()) {
    GTEST_SKIP() << "CI RAW fixtures missing under TEST_IMG_PATH/ci_rawfiles; pull the "
                    "git-lfs fixtures to exercise recursive real-import semantic generation";
  }
  ASSERT_GT(paths.size(), 0U);

  ProjectService project(db_path_, meta_path_, ProjectOpenMode::kCreateNew);
  auto           semantic = semantic_test::StoresOf(*project.GetStorage());
  RegisterSemanticTestModel(semantic);

  const auto items = ImportPaths(project, paths);
  ASSERT_GT(items.size(), 0U);

  std::unordered_map<sl_element_id_t, std::vector<float>> routes;
  routes.reserve(items.size());
  for (size_t i = 0; i < items.size(); ++i) {
    const auto& item = items[i];
    const auto  path = project.GetImagePoolService()->Read<std::filesystem::path>(
        item.image_id, [](const std::shared_ptr<Image>& image) {
          return image ? image->image_path_ : std::filesystem::path{};
        });
    const auto generic = path.generic_string();
    if (generic.find("/building/") != std::string::npos) {
      routes[item.element_id] =
          Mixed512(LabelIndex("architecture"), LabelIndex("cityscape"), 0.15F);
    } else if (i % 5 == 0) {
      routes[item.element_id] = Mixed512(LabelIndex("street"), LabelIndex("landscape"), 0.985F);
    } else {
      routes[item.element_id] = Mixed512(LabelIndex("product"), LabelIndex("still life"), 0.12F);
    }
  }

  auto pipeline_service = std::make_shared<PipelineMgmtService>(project.GetStorage());
  auto inputs =
      std::make_shared<CountingRealInputSource>(std::make_shared<SemanticInputImageService>(
          project.GetImagePoolService(), pipeline_service, 256));
  auto embedder = std::make_shared<Routed512EmbeddingClient>(std::move(routes));
  SemanticGenerationService service(inputs, embedder);

  SemanticGenerationOptions options;
  options.embedding_batch_size = 64;
  options.embedding_timeout    = 300s;
  options.expected_model_info =
      AiSidecarRuntimeModelInfo{.model_id            = "mock/mobileclip",
                               .revision            = "mock-revision",
                               .embedding_dimension = kSemanticEmbeddingDim,
                               .image_size          = 256,
                               .provider            = "mock"};
  SemanticGenerationPersistenceOptions persistence;
  persistence.label_store     = &semantic.labels_;
  persistence.embedding_store = &semantic.embeddings_;
  persistence.model_key          = "mobileclip-test";
  options.persistence            = persistence;

  auto job                       = service.StartGeneration(items, options);
  job->Wait();

  const auto progress = job->SnapshotProgress();
  EXPECT_EQ(progress.total, items.size());
  EXPECT_EQ(progress.embedded, items.size());
  EXPECT_EQ(progress.failed, 0U);
  EXPECT_EQ(progress.canceled, 0U);
  EXPECT_EQ(inputs->RequestCount(), static_cast<int>(items.size()));
  EXPECT_EQ(inputs->PreviewCount() + inputs->RenderCount(), static_cast<int>(items.size()));
  EXPECT_EQ(embedder->ImageItemCount(), static_cast<int>(items.size()));

  const auto item_count = static_cast<int64_t>(items.size());
  auto       sql_guard = StorageTestAccess::GetDatabase(*project.GetStorage()).GetConnectionGuard();
  EXPECT_EQ(RawScalarInt64(sql_guard.conn_,
                           "SELECT COUNT(*) FROM SemanticImageEmbedding "
                           "WHERE model_key = 'mobileclip-test' AND status = 'ready';"),
            item_count);
  EXPECT_EQ(RawScalarInt64(sql_guard.conn_,
                           "SELECT COUNT(*) FROM SemanticImageLabel "
                           "WHERE model_key = 'mobileclip-test';"),
            item_count);
  EXPECT_GT(RawScalarInt64(sql_guard.conn_,
                           "SELECT COUNT(*) FROM SemanticImageLabel "
                           "WHERE model_key = 'mobileclip-test' "
                           "AND label IN ('product', 'street', 'architecture');"),
            0);
  // The Mixed512(street, landscape, 0.985) route yields a near-tied top-2 (landscape then
  // street), so the elbow keeps both: at least one stored row carries more than one tag in
  // its top_scores JSON (entries are separated by "},{").
  EXPECT_GT(RawScalarInt64(sql_guard.conn_,
                           "SELECT COUNT(*) FROM SemanticImageLabel "
                           "WHERE model_key = 'mobileclip-test' "
                           "AND top_scores::VARCHAR LIKE '%},{%';"),
            0);
}

TEST_F(SemanticGenerationServiceTest, PersistenceRejectsBadVectorsWithoutWritingRows) {
  ProjectService project(db_path_, meta_path_, ProjectOpenMode::kCreateNew);
  auto           semantic = semantic_test::StoresOf(*project.GetStorage());
  RegisterSemanticTestModel(semantic);

  auto bad_embedding                   = OneHot512(3);
  bad_embedding[5]                     = std::numeric_limits<float>::quiet_NaN();
  auto                      inputs     = std::make_shared<ImmediateInputSource>();
  auto                      embedder   = std::make_shared<Fixed512EmbeddingClient>(bad_embedding);
  SemanticGenerationService service(inputs, embedder);

  SemanticGenerationOptions options;
  SemanticGenerationPersistenceOptions persistence;
  persistence.label_store     = &semantic.labels_;
  persistence.embedding_store = &semantic.embeddings_;
  persistence.model_key          = "mobileclip-test";
  options.persistence            = persistence;

  auto job                       = service.StartGeneration({{77, 770}}, options);
  job->Wait();

  const auto progress = job->SnapshotProgress();
  EXPECT_EQ(progress.embedded, 0U);
  EXPECT_EQ(progress.failed, 1U);
  EXPECT_EQ(semantic.embeddings_.CountImageEmbeddingsForFile(77, "mobileclip-test"), 0U);
  EXPECT_EQ(semantic.labels_.CountImageLabelsForFile(77, "mobileclip-test"), 0U);

  const auto results = job->Results();
  ASSERT_EQ(results.size(), 1U);
  EXPECT_EQ(results.front().status, SemanticGenerationItemStatus::kError);
  EXPECT_NE(results.front().error.find("NaN"), std::string::npos);
}

// The job reads the images that already have labels with one query. It selects exactly the images
// for which the per-image check HasReadyImageEmbedding(..., require_label) is true: a labeled
// embedding counts; an embedding without a label, another image id, and another model do not.
TEST_F(SemanticGenerationServiceTest, ReadyLabeledImageListMatchesThePerImageCheck) {
  ProjectService project(db_path_, meta_path_, ProjectOpenMode::kCreateNew);
  auto           semantic = semantic_test::StoresOf(*project.GetStorage());
  RegisterSemanticTestModel(semantic);

  auto inputs   = std::make_shared<ImmediateInputSource>();
  auto embedder = std::make_shared<Fixed512EmbeddingClient>(OneHot512(LabelIndex("street")));
  SemanticGenerationService            service(inputs, embedder);
  SemanticGenerationOptions            options;
  SemanticGenerationPersistenceOptions persistence;
  persistence.label_store     = &semantic.labels_;
  persistence.embedding_store = &semantic.embeddings_;
  persistence.model_key       = "mobileclip-test";
  options.persistence         = persistence;
  auto job                    = service.StartGeneration({{42, 420}, {43, 430}}, options);
  job->Wait();
  ASSERT_EQ(job->SnapshotProgress().embedded, 2U);

  // An embedding without a label.
  SemanticImageEmbeddingRecord unlabeled;
  unlabeled.file_id_   = 44;
  unlabeled.image_id_  = 440;
  unlabeled.model_key_ = "mobileclip-test";
  unlabeled.embedding_ = OneHot512(3);
  std::string error;
  ASSERT_TRUE(semantic.embeddings_.UpsertImageEmbedding(unlabeled, &error)) << error;

  const auto ready = semantic.embeddings_.ListReadyLabeledImages("mobileclip-test", &error);
  EXPECT_TRUE(error.empty()) << error;
  const std::vector<SemanticGenerationItem> probes{{42, 420}, {43, 430}, {44, 440}, {42, 999}};
  for (const auto& probe : probes) {
    const bool listed = std::find(ready.begin(), ready.end(),
                                  std::pair<sl_element_id_t, image_id_t>{
                                      probe.element_id, probe.image_id}) != ready.end();
    EXPECT_EQ(listed, semantic.embeddings_.HasReadyImageEmbedding(probe.element_id, probe.image_id,
                                                                  "mobileclip-test", true))
        << probe.element_id << ":" << probe.image_id;
  }
  EXPECT_EQ(ready.size(), 2U);
  EXPECT_TRUE(semantic.embeddings_.ListReadyLabeledImages("another-model", &error).empty());
}

}  // namespace alcedo
