//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/semantic_query_embedding_cache.hpp"

#include <algorithm>

namespace alcedo {

SemanticQueryEmbeddingCache::SemanticQueryEmbeddingCache(size_t capacity)
    : capacity_(std::max<size_t>(capacity, 1)) {}

auto SemanticQueryEmbeddingCache::GetOrEmbed(const std::string& model_key, const std::string& text,
                                             const Encoder& encode) -> std::vector<float> {
  Key key{model_key, text};
  {
    std::lock_guard lock(mutex_);
    if (const auto found = index_.find(key); found != index_.end()) {
      entries_.splice(entries_.begin(), entries_, found->second);
      return found->second->second;
    }
  }

  auto embedding = encode();

  std::lock_guard lock(mutex_);
  StoreLocked(std::move(key), embedding);
  return embedding;
}

auto SemanticQueryEmbeddingCache::Contains(const std::string& model_key,
                                           const std::string& text) const -> bool {
  std::lock_guard lock(mutex_);
  return index_.contains(Key{model_key, text});
}

auto SemanticQueryEmbeddingCache::Size() const -> size_t {
  std::lock_guard lock(mutex_);
  return entries_.size();
}

void SemanticQueryEmbeddingCache::StoreLocked(Key key, std::vector<float> embedding) {
  if (const auto found = index_.find(key); found != index_.end()) {
    found->second->second = std::move(embedding);
    entries_.splice(entries_.begin(), entries_, found->second);
    return;
  }
  entries_.emplace_front(key, std::move(embedding));
  index_.emplace(std::move(key), entries_.begin());
  while (entries_.size() > capacity_) {
    index_.erase(entries_.back().first);
    entries_.pop_back();
  }
}

}  // namespace alcedo
